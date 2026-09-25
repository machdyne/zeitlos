/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Memory protection unit (docs/mpu.md)
 *
 * Contains bugs in trusted apps. It is not a security boundary: the
 * goal is that a wild pointer or a bad jump in one app ends that app
 * with a report, instead of silently corrupting another app, the
 * kernel, or hardware state that outlives the app.
 *
 *   picorv32_wb --> wb_mtu --> [ wb_mpu ] --> wb_cache / wb_icache /
 *   zeitlos32_wb ------------^   checks         (none) --> bus
 *
 * Sits on the CPU's own path, BEFORE the cache: with `DCACHE a store
 * is acknowledged as soon as it enters the write buffer, and a load
 * can be served from a line without touching the bus, so a check
 * anywhere downstream would be too late. Works with or without either
 * cache. Other bus masters are not checked.
 *
 * -- Privilege --
 *
 * Privilege comes from where the running code was FETCHED: code from
 * BIOS BRAM (0x0000_0000-0x0000_1fff) or kernel text (0x4000_0000 up
 * to KTEXT) is privileged, anything else is not. Both CPUs are
 * non-pipelined, so every load and store belongs to the most recently
 * fetched instruction; `priv` is updated when a fetch completes and
 * stays stable for the whole of the next instruction's data access.
 * Nothing in software sets or clears it.
 *
 * Unprivileged code may enter privileged code only at the GATE (the
 * syscall entry the kernel publishes at reg_kernel, 0x0000_000c) or
 * the IRQ vector (0x0000_0010). Otherwise a bug that jumps into the
 * middle of the kernel would become privileged.
 *
 * -- What unprivileged code may do --
 *
 *   fetch   only inside its own block (or the gate / IRQ vector)
 *   load    anything whose address nibble is enabled in MASK
 *   store   own block; peripheral nibbles enabled in MASK, EXCEPT the
 *           fixed kernel-only set: BRAM (nibble 0), flash and its
 *           write/erase registers (nibble 1), other main memory,
 *           cache control (0x7000_01xx), the FPGA reconfigure key
 *           (0x7000_0218), MTU/MPU (nibble 9)
 *
 * "Own block" is [MTU base, MTU base + SIZE). To keep this off the
 * MTU's adder, it is decided from the CPU's UNTRANSLATED address: an
 * access through the app window (0x8xxx_xxxx, translated when the MTU
 * base is nonzero and XLATE_ON_BUS, i.e. picorv32) is inside the
 * block exactly when its offset is below SIZE; any other address is
 * physical and is compared against [base, limit). zeitlos32 translates
 * inside the core (sysctl.v's MTU_ON_BUS=0), so its addresses are
 * always physical here.
 *
 * -- Violations --
 *
 * Recorded (first one latched: address, PC of the offending
 * instruction, kind, reason; plus a count) and signalled on irq_o.
 * With ENFORCE the access never reaches the cache or bus: a store is
 * dropped, a load reads 0, and a fetch returns 0x0000_0000, which is
 * an illegal instruction, so the CPU traps as well. Without ENFORCE
 * (report-only) the access proceeds normally and is only recorded.
 *
 * -- Registers (CFG_BASE, answered here, never on the bus) --
 *
 *   +0x00 CTRL      bit0 enable, bit1 enforce, bit2 irq enable (reset 0)
 *   +0x04 KTEXT     end of kernel text (physical), exclusive
 *   +0x08 GATE      syscall entry address
 *   +0x0C SIZE      size of the current app's block (0: none)
 *   +0x10 MASK      bit n: unprivileged code may use nibble n
 *   +0x14 FAULT_ADDR  first latched violation: the address accessed
 *   +0x18 FAULT_PC    ...its instruction's address (as fetched)
 *   +0x1C FAULT_INFO  { valid, 3'b0, reason[3:0], 6'b0, kind[1:0],
 *                       sel[3:0], 12'b0 }, write anything: clear
 *   +0x20 COUNT     violations since cleared (write: clear)
 *   +0x24 INFO      { 16'h3A50, 16'd1 } -- magic, version
 *
 *   kind:   0 fetch, 1 load, 2 store
 *   reason: 1 outside own block, 2 kernel-only, 3 nibble masked,
 *           4 entered kernel code other than at the gate
 *
 * On a bitstream without `MPU the MTU answers the whole 0x9 nibble, so
 * these addresses read and WRITE the MTU base. Software must check
 * INFO's magic before writing anything here (sw/common/zsoc.h does).
 * The magic's top nibble (3) cannot be a main-memory MTU base.
 */

module wb_mpu #(
    // 1: main memory is 0x4000_0000-0x5fff_ffff (512MB, `MAIN_512MB in
    // rtl/boards.vh) instead of 0x4xxx_xxxx. Everything that decides
    // "is this main memory" must agree, so rtl/sysctl.v sets this one
    // parameter from that one define and passes it to the caches and
    // the MPU. 0 gives exactly the original logic.
    //
    // Without it, a 512MB board's upper half is not "main memory" to
    // the MPU: stores there are gated only by MASK, whose default
    // allows nibble 5, so any app could write any other process's
    // memory above 0x5000_0000.
    parameter MAIN_512 = 0,
    parameter XLATE_ON_BUS = 1,           // 1: picorv32 (MTU on the bus)
    parameter [31:0] CFG_BASE = 32'h9000_0100
) (
    input wb_clk_i,
    input wb_rst_i,

    // -- CPU side (untranslated address) --
    input [31:0] c_adr_i,
    input [31:0] c_dat_i,
    input c_we_i,
    input [3:0] c_sel_i,
    input c_stb_i,
    input c_cyc_i,
    input c_instr_i,
    output [31:0] c_dat_o,
    output c_ack_o,

    // -- downstream: only stb/cyc are gated; everything else is taken
    //    straight from the CPU/MTU by the consumer --
    output d_stb_o,
    output d_cyc_o,
    input [31:0] d_dat_i,
    input d_ack_i,

    input [31:0] mtu_base_i,
    output irq_o
);

    // -- registers ---------------------------------------------------

    reg en;
    reg enforce;
    reg irq_en;
    reg [31:0] ktext;
    reg [31:0] gate;
    reg [31:0] size;
    reg [15:0] mask;
    reg [31:0] limit;          // mtu_base + size, registered (off the path)

    reg priv;                  // privilege of the current instruction
    reg [31:0] last_pc;        // address of the current instruction

    reg f_valid;
    reg [31:0] f_addr;
    reg [31:0] f_pc;
    reg [3:0] f_reason;
    reg [1:0] f_kind;
    reg [3:0] f_sel;
    reg [31:0] f_count;

    reg in_req;                // a request is in progress (seen once)

    // A completed fetch updates priv/last_pc one cycle LATER, from these.
    // Otherwise the cache's fast-hit acknowledge fans out straight into
    // 33 register enables -- the SOC's critical path on mozart_ml1 once
    // the return mux was fixed. Safe: neither CPU issues the fetched
    // instruction's load or store sooner than two cycles after the
    // fetch completes, and the next fetch is later still.
    reg fetch_done;
    reg fetch_priv;
    reg [31:0] fetch_pc;
    reg local_ack;             // answering locally: blocked or register
    reg [31:0] local_dat;

    // -- decode ------------------------------------------------------

    wire req = c_cyc_i && c_stb_i;
    wire cfg_sel = ((c_adr_i & 32'hffff_ff00) == CFG_BASE);

    wire [3:0] nib = c_adr_i[31:28];
    wire xlate = (XLATE_ON_BUS != 0) && (nib == 4'h8) &&
                 (mtu_base_i != 32'h0);
    wire [3:0] pnib = xlate ? 4'h4 : nib;    // nibble after translation

    // Main memory: nibble 4, or 4 and 5 on a 512MB board.
    wire nib_main  = MAIN_512 ? (nib[3:1] == 3'b010)  : (nib == 4'h4);
    wire pnib_main = MAIN_512 ? (pnib[3:1] == 3'b010) : (pnib == 4'h4);

    // inside the current app's block
    wire own = xlate ? ({4'h0, c_adr_i[27:0]} < size) :
               (nib_main && (c_adr_i >= mtu_base_i) &&
                (c_adr_i < limit) && (size != 32'h0));

    // fetching from here makes the instruction privileged
    wire priv_region = !xlate &&
        ((c_adr_i[31:13] == 19'd0) ||
         (nib_main && (c_adr_i < ktext)));

    wire at_gate = !xlate &&
        ((c_adr_i == gate) || (c_adr_i == 32'h0000_0010));

    wire kernel_only_store =
        (pnib == 4'h0) || (pnib == 4'h1) || (pnib == 4'h9) ||
        (pnib_main && !own) ||
        (c_adr_i[31:8] == 24'h7000_01) ||
        (c_adr_i[31:2] == 30'h1C00_0086);          // 0x7000_0218

    // Violation of the current request, and why. Depends only on the
    // request and on registers that do not change during it.
    reg [3:0] why;
    always @* begin
        why = 4'd0;
        if (en && !priv) begin
            if (c_instr_i) begin
                if (priv_region) begin
                    if (!at_gate) why = 4'd4;
                end else if (!own) begin
                    why = 4'd1;
                end
            end else if (!mask[pnib]) begin
                why = 4'd3;
            end else if (c_we_i && kernel_only_store) begin
                why = pnib_main ? 4'd1 : 4'd2;
            end
        end
    end
    wire violation = (why != 4'd0);

    // The register window is always answered here. An unprivileged
    // write to it is a violation (nibble 9 is kernel-only for stores)
    // and changes nothing, even in report-only mode; an unprivileged
    // read is allowed (mask bit 9) and returns the register.
    wire local_now = cfg_sel || (violation && enforce);

    assign d_stb_o = c_stb_i && !local_now;
    assign d_cyc_o = c_cyc_i && !local_now;
    // The return path must NOT depend on local_now: that would put the
    // address comparators in front of the cache's fast-hit acknowledge,
    // which is how the first place-and-route found it (the SOC's
    // critical path, 53.3MHz on mozart_ml1). It does not need to: when
    // the MPU answers locally the request never went downstream, so
    // d_ack_i cannot fire; when it does not, local_ack is 0. So an OR,
    // and a data mux selected by a register.
    assign c_ack_o = local_ack | d_ack_i;
    assign c_dat_o = local_ack ? local_dat : d_dat_i;
    assign irq_o = irq_en && f_valid;

    wire [3:0] cfg_reg = c_adr_i[5:2];

    always @(posedge wb_clk_i) begin
        if (wb_rst_i) begin
            en <= 1'b0;
            enforce <= 1'b0;
            irq_en <= 1'b0;
            ktext <= 32'h0;
            gate <= 32'h0;
            size <= 32'h0;
            mask <= 16'hffff;
            limit <= 32'h0;
            priv <= 1'b1;
            last_pc <= 32'h0;
            f_valid <= 1'b0;
            f_addr <= 32'h0;
            f_pc <= 32'h0;
            f_reason <= 4'd0;
            f_kind <= 2'd0;
            f_sel <= 4'd0;
            f_count <= 32'h0;
            in_req <= 1'b0;
            local_ack <= 1'b0;
            local_dat <= 32'h0;
            fetch_done <= 1'b0;
            fetch_priv <= 1'b1;
            fetch_pc <= 32'h0;
        end else begin
            limit <= mtu_base_i + size;
            local_ack <= 1'b0;

            // -- once per request: record a violation, answer locally
            if (req && !in_req) begin
                in_req <= 1'b1;
                if (violation) begin
                    f_count <= f_count + 1;
                    if (!f_valid) begin
                        f_valid <= 1'b1;
                        f_addr <= c_adr_i;
                        f_pc <= last_pc;
                        f_reason <= why;
                        f_kind <= c_instr_i ? 2'd0 : (c_we_i ? 2'd2 : 2'd1);
                        f_sel <= c_sel_i;
                    end
                end
                if (local_now) begin
                    local_ack <= 1'b1;
                    local_dat <= 32'h0;   // blocked: loads read 0,
                                          // fetches an illegal insn
                    if (cfg_sel && !c_we_i) begin
                        case (cfg_reg)
                            4'd0: local_dat <= { 29'b0, irq_en, enforce, en };
                            4'd1: local_dat <= ktext;
                            4'd2: local_dat <= gate;
                            4'd3: local_dat <= size;
                            4'd4: local_dat <= { 16'b0, mask };
                            4'd5: local_dat <= f_addr;
                            4'd6: local_dat <= f_pc;
                            4'd7: local_dat <= { f_valid, 3'b0, f_reason,
                                     6'b0, f_kind, f_sel, 12'b0 };
                            4'd8: local_dat <= f_count;
                            4'd9: local_dat <= { 16'h3A50, 16'd1 };
                            default: local_dat <= 32'h0;
                        endcase
                    end
                    // writes: only if not a violation (i.e. privileged)
                    if (cfg_sel && c_we_i && !violation) begin
                        case (cfg_reg)
                            4'd0: begin
                                en <= c_dat_i[0];
                                enforce <= c_dat_i[1];
                                irq_en <= c_dat_i[2];
                            end
                            4'd1: ktext <= c_dat_i;
                            4'd2: gate <= c_dat_i;
                            4'd3: size <= c_dat_i;
                            4'd4: mask <= c_dat_i[15:0];
                            4'd7: f_valid <= 1'b0;
                            4'd8: f_count <= 32'h0;
                            default: ;
                        endcase
                    end
                end
            end

            // -- a fetch in progress: what it WILL mean once it completes,
            //    from the request alone, not from the ack. A blocked
            //    fetch keeps the old privilege: the illegal instruction
            //    handed back must not "run" as kernel code just because
            //    of where it was aimed.
            if (req && c_instr_i) begin
                fetch_priv <= (violation && enforce) ? priv : (!en || priv_region);
                fetch_pc <= c_adr_i;
            end

            // -- end of request (the ack reaches only one flop here)
            fetch_done <= c_ack_o && c_instr_i;
            if (c_ack_o) in_req <= 1'b0;
            if (fetch_done) begin
                priv <= fetch_priv;
                last_pc <= fetch_pc;
            end
        end
    end

endmodule
