/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Redistribution and use in source, binary or physical forms, with or
 * without modification, is permitted provided that the following
 * condition is met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * THIS HARDWARE, SOFTWARE, DATA AND/OR DOCUMENTATION ("THE ASSETS") IS
 * PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE ASSETS OR THE USE OR OTHER DEALINGS IN THE ASSETS. USE AT
 * YOUR OWN RISK.
 *
 */

/*
 * Memory protection for zeitlos32p (docs/mpu.md, docs/zeitlos32p.md).
 *
 * The same rules, registers, fault recording and interrupt as
 * rtl/mpu.v's wb_mpu, which is untouched and is what every other
 * build uses. This one exists because wb_mpu takes a load's or
 * store's privilege from the most recent FETCH: right for a core with
 * one instruction in flight, wrong for a pipeline, whose fetches run
 * several instructions ahead and down paths that are thrown away.
 *
 * Here privilege belongs to the instruction:
 *
 *   - Fetches. The core asks chk_* about each instruction's address as
 *     it moves from D to X (privileged region? the gate or the IRQ
 *     vector? inside the app's block?), and decides in X, in program
 *     order, with the privilege of the instruction before it -- the
 *     rule wb_mpu applies at the fetch. A violation is reported to
 *     this module (fv_*) when the instruction leaves X; with ENFORCE
 *     the instruction is treated as illegal, as wb_mpu's blocked fetch
 *     hands the CPU 0x0000_0000. A fetch that is thrown away is never
 *     judged.
 *
 *   - Loads and stores carry their instruction's privilege (d_priv).
 *     Every access the cache takes is copied into registers here (tq_*,
 *     no enables) and judged by wb_mpu's rules in the next cycle, from
 *     those registers; the verdict (d_veto) reaches the cache in that
 *     cycle, before the access has touched the arrays, the write
 *     buffer or the bus. So the comparators follow a register, not
 *     the core's address adder. Blocked: a load reads 0, a store
 *     changes nothing. Accesses to this module's registers are
 *     recognised at the take (d_reg: an address match, no compare).
 *
 *   - Uncached fetches (anything but BIOS RAM and main memory) are
 *     blocked at the I port while the last instruction executed was
 *     unprivileged and the MPU enforces, so a jump into peripheral
 *     space cannot read a FIFO by fetching from it before X has
 *     judged it. Main memory and BIOS RAM have no read side effects
 *     and are left to X, which is exact.
 *
 * zeitlos32p translates through the MTU itself, so data addresses are
 * physical (wb_mpu's XLATE_ON_BUS = 0). Instruction addresses are
 * judged virtual, by wb_mpu's XLATE_ON_BUS = 1 rules (inside the app
 * window: inside the block exactly when the offset is below SIZE, and
 * never privileged or the gate), which agree with judging the
 * translated address for any app block that lies above KTEXT, as
 * every block the kernel hands out does. Fault addresses and PCs are
 * recorded physical, as wb_mpu with zeitlos32 records them.
 *
 * Register writes take effect in the cycle after the cache takes them
 * (LOOK); the cache answers them the cycle after that, so the next
 * load or store is judged by the new values. The core serialises
 * custom-0 instructions (retirq), so a context switch's register
 * writes are all in place before the app runs.
 */

module wb_mpu_split #(
    parameter MAIN_512 = 0,
    parameter [31:0] CFG_BASE = 32'h9000_0100
) (
    input wb_clk_i,
    input wb_rst_i,

    // -- the D port, as the cache sees it --
    input        d_req,
    input [31:0] d_adr,         // physical, word-aligned
    input        d_we,
    input  [3:0] d_sel,
    input [31:0] d_wdat,
    input        d_priv,        // the instruction's privilege
    input [31:0] d_pc,          // ...and its address (virtual)
    input        d_rdy,         // the cache takes it this cycle if d_req
    output       d_veto,        // to the cache: last cycle's take is blocked
    output       d_reg,         // to the cache, with the request
    output reg [31:0] d_reg_dat,

    // -- the I port --
    input [31:0] i_adr,         // physical
    input        i_priv,        // privilege of the last instruction executed
    output       i_veto,

    // -- instruction checks for the core, on D's address (virtual) --
    input [31:0] chk_pc,
    output       chk_privreg,
    output       chk_gate,
    output       chk_own,

    // -- a fetch violation, reported by the core (registered there) --
    input        fv_ev,
    input [31:0] fv_adr,        // the instruction's address (virtual)
    input [31:0] fv_pc,         // the one executed before it (virtual)
    input  [3:0] fv_why,

    output       en_o,
    output       enforce_o,

    input [31:0] mtu_base_i,
    output       irq_o
);

    // -- registers (as wb_mpu) ---------------------------------------

    reg en;
    reg enforce;
    reg irq_en;
    reg [31:0] ktext;
    reg [31:0] gate;
    reg [31:0] size;
    reg [15:0] mask;
    reg [31:0] limit;          // mtu_base + size, registered
    reg [31:0] gbase;
    reg [31:0] glimit;

    reg f_valid;
    reg [31:0] f_addr;
    reg [31:0] f_pc;
    reg [3:0] f_reason;
    reg [1:0] f_kind;
    reg [3:0] f_sel;
    reg [31:0] f_count;

    // the access the cache took last cycle, judged then
    reg        tq_v;
    reg [31:0] tq_adr;
    reg        tq_we;
    reg  [3:0] tq_sel;
    reg [31:0] tq_dat;
    reg [31:0] tq_pc;
    reg        tq_priv;
    reg        tq_cfg;

    assign en_o = en;
    assign enforce_o = enforce;
    assign irq_o = irq_en && f_valid;

    wire mtu_en = (mtu_base_i != 32'h0);

    // -- the data side: wb_mpu's rules, physical address, on the copy
    //    of what the cache took last cycle --------------------------

    wire cfg_sel = ((d_adr & 32'hffff_ff00) == CFG_BASE);

    wire [3:0] nib = tq_adr[31:28];
    wire nib_main = MAIN_512 ? (nib[3:1] == 3'b010) : (nib == 4'h4);

    wire t_own = nib_main && (tq_adr >= mtu_base_i) && (tq_adr < limit) &&
                 (size != 32'h0);
    wire t_granted = nib_main && (tq_adr >= gbase) && (tq_adr < glimit);

    wire kernel_only_store =
        (nib == 4'h0) || (nib == 4'h1) || (nib == 4'h9) ||
        (nib_main && !t_own && !t_granted) ||
        (tq_adr[31:8] == 24'h7000_01) ||
        (tq_adr[31:2] == 30'h1C00_0086);          // 0x7000_0218

    reg [3:0] tq_why;
    always @* begin
        tq_why = 4'd0;
        if (en && !tq_priv) begin
            if (!mask[nib])
                tq_why = 4'd3;
            else if (tq_we && kernel_only_store)
                tq_why = nib_main ? 4'd1 : 4'd2;
        end
    end

    // The register window is always answered here (an unprivileged
    // write is a violation and changes nothing; an unprivileged read is
    // allowed by mask bit 9, and returns the register even when it is
    // not -- as wb_mpu).
    assign d_reg  = cfg_sel;
    assign d_veto = tq_v && (tq_why != 4'd0) && enforce && !tq_cfg;

    // -- the I port: no uncached fetch while unprivileged --------------

    wire i_main = MAIN_512 ? (i_adr[31:29] == 3'b010) : (i_adr[31:28] == 4'h4);
    assign i_veto = en && enforce && !i_priv &&
                    !((i_adr[31:13] == 19'd0) || i_main);

    // -- instruction checks (virtual address) --------------------------

    wire [3:0] cnib = chk_pc[31:28];
    wire cxl = mtu_en && (cnib == 4'h8);
    wire cmain = MAIN_512 ? (cnib[3:1] == 3'b010) : (cnib == 4'h4);

    assign chk_privreg = !cxl &&
        ((chk_pc[31:13] == 19'd0) || (cmain && (chk_pc < ktext)));
    assign chk_gate = !cxl &&
        ((chk_pc == gate) || (chk_pc == 32'h0000_0010));
    assign chk_own = cxl ? ({4'h0, chk_pc[27:0]} < size) :
        (cmain && (chk_pc >= mtu_base_i) && (chk_pc < limit) &&
         (size != 32'h0));

    // -- virtual to physical, for the fault registers ------------------

    wire [31:0] tq_ppc = (mtu_en && tq_pc[31:28] == 4'h8) ?
        (mtu_base_i + { 4'h0, tq_pc[27:0] }) : tq_pc;
    wire [31:0] fv_padr = (mtu_en && fv_adr[31:28] == 4'h8) ?
        (mtu_base_i + { 4'h0, fv_adr[27:0] }) : fv_adr;
    wire [31:0] fv_ppc = (mtu_en && fv_pc[31:28] == 4'h8) ?
        (mtu_base_i + { 4'h0, fv_pc[27:0] }) : fv_pc;

    wire tq_viol = tq_v && (tq_why != 4'd0);
    wire [3:0] tq_reg = tq_adr[5:2];

    always @(posedge wb_clk_i) begin
        // what was taken this cycle, for next cycle (no enables)
        tq_v   <= d_req && d_rdy;
        tq_adr <= d_adr;
        tq_we  <= d_we;
        tq_sel <= d_sel;
        tq_dat <= d_wdat;
        tq_pc  <= d_pc;
        tq_priv <= d_priv;
        tq_cfg <= cfg_sel;

        if (wb_rst_i) begin
            en <= 1'b0;
            enforce <= 1'b0;
            irq_en <= 1'b0;
            ktext <= 32'h0;
            gate <= 32'h0;
            size <= 32'h0;
            mask <= 16'hffff;
            limit <= 32'h0;
            gbase <= 32'h0;
            glimit <= 32'h0;
            f_valid <= 1'b0;
            f_addr <= 32'h0;
            f_pc <= 32'h0;
            f_reason <= 4'd0;
            f_kind <= 2'd0;
            f_sel <= 4'd0;
            f_count <= 32'h0;
            tq_v <= 1'b0;
            d_reg_dat <= 32'h0;
        end else begin
            limit <= mtu_base_i + size;

            // -- violations: the fetch is the older of the two when both
            //    arrive in one cycle.  The core holds a fetch violation in
            //    X until every older access was answered (so their tq_*
            //    came earlier), and reports it from the cycle X moves on --
            //    the cycle its own load or store is first offered -- so a
            //    data violation seen together with it is that instruction's
            //    own (or a younger one's), never an older one
            if (tq_viol && fv_ev)
                f_count <= f_count + 2;
            else if (tq_viol || fv_ev)
                f_count <= f_count + 1;

            if (!f_valid && fv_ev) begin
                f_valid <= 1'b1;
                f_addr <= fv_padr;
                f_pc <= fv_ppc;
                f_reason <= fv_why;
                f_kind <= 2'd0;
                f_sel <= 4'b1111;
            end else if (!f_valid && tq_viol) begin
                f_valid <= 1'b1;
                f_addr <= tq_adr;
                f_pc <= tq_ppc;
                f_reason <= tq_why;
                f_kind <= tq_we ? 2'd2 : 2'd1;
                f_sel <= tq_sel;
            end

            // -- the register window, in the cycle after the take; the
            //    cache answers it the cycle after this one
            if (tq_v && tq_cfg) begin
                d_reg_dat <= 32'h0;
                if (!tq_we) begin
                    case (tq_reg)
                        4'd0: d_reg_dat <= { 29'b0, irq_en, enforce, en };
                        4'd1: d_reg_dat <= ktext;
                        4'd2: d_reg_dat <= gate;
                        4'd3: d_reg_dat <= size;
                        4'd4: d_reg_dat <= { 16'b0, mask };
                        4'd5: d_reg_dat <= f_addr;
                        4'd6: d_reg_dat <= f_pc;
                        4'd7: d_reg_dat <= { f_valid, 3'b0, f_reason,
                                 6'b0, f_kind, f_sel, 12'b0 };
                        4'd8: d_reg_dat <= f_count;
                        4'd9: d_reg_dat <= { 16'h3A50, 16'd2 };
                        4'd10: d_reg_dat <= gbase;
                        4'd11: d_reg_dat <= glimit;
                        default: d_reg_dat <= 32'h0;
                    endcase
                end
                // writes: only if not a violation (privileged)
                if (tq_we && (tq_why == 4'd0)) begin
                    case (tq_reg)
                        4'd0: begin
                            en <= tq_dat[0];
                            enforce <= tq_dat[1];
                            irq_en <= tq_dat[2];
                        end
                        4'd1: ktext <= tq_dat;
                        4'd2: gate <= tq_dat;
                        4'd3: size <= tq_dat;
                        4'd4: mask <= tq_dat[15:0];
                        4'd7: f_valid <= 1'b0;
                        4'd8: f_count <= 32'h0;
                        4'd10: gbase <= tq_dat;
                        4'd11: glimit <= tq_dat;
                        default: ;
                    endcase
                end
            end
        end
    end

endmodule
