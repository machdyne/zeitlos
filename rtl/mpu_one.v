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
 * Memory protection for zeitlos32ps (rtl/cpu/zeitlos32ps), on its one
 * port. rtl/mpu_split.v (zeitlos32p's) with the same rules, registers,
 * fault recording and interrupt, which are wb_mpu's (rtl/mpu.v), and
 * privilege per instruction -- but where mpu_split has a second set of
 * comparators for the instruction checks, on D's virtual address, this
 * one judges every access the cache takes with ONE set, on the copy of
 * the request it registers (tq_*):
 *   - a load or store: wb_mpu's data rules, with the privilege of its
 *     instruction (d_priv); the verdict (d_veto) reaches the cache in
 *     the cycle after the take, before the access has touched the
 *     arrays, the write buffer or the bus;
 *   - a fetch: the three properties of its address that the decision
 *     needs (p_privreg: fetching it would make code privileged; p_gate:
 *     it is the gate or the IRQ vector; p_own: it is inside the app's
 *     block), from the cycle after the take until the next take -- the
 *     fetch's answer comes in that time, as the port has one access
 *     outstanding. The core keeps them with the instruction and decides
 *     in X, in program order, as zeitlos32p does with chk_*. The own-
 *     block compare is the data side's.
 * Fetch addresses are judged physical, as wb_mpu judges zeitlos32's
 * (XLATE_ON_BUS 0); for an app block above KTEXT, as every block the
 * kernel hands out is, that agrees with mpu_split's virtual rules.
 * Uncached fetches while the last instruction executed was unprivileged
 * are blocked at the take (i_veto), as mpu_split does at its I port.
 * Fault addresses and PCs are recorded physical, as the core supplies
 * them (d_pc, fv_adr, fv_pc).
 */

module wb_mpu_one #(
    parameter MAIN_512 = 0,
    parameter [31:0] CFG_BASE = 32'h9000_0100
) (
    input wb_clk_i,
    input wb_rst_i,

    // -- the port, as the cache sees it --
    input        c_req,
    input [31:0] c_adr,         // physical, word-aligned
    input        c_we,
    input  [3:0] c_sel,
    input [31:0] c_wdat,
    input        c_instr,       // a fetch
    input        c_rdy,         // the cache takes it this cycle if c_req
    input        d_priv,        // a load's or store's instruction's privilege
    input [31:0] d_pc,          // ...and its address (physical)
    input        i_priv,        // privilege of the last instruction executed
    output       d_veto,        // to the cache: last cycle's load/store is blocked
    output       d_reg,         // to the cache, with a load or store
    output reg [31:0] d_reg_dat,
    output       i_veto,        // to the cache, with a fetch

    // -- the last fetch taken, for the core (see above) --
    output       p_privreg,
    output       p_gate,
    output       p_own,

    // -- a fetch violation, reported by the core (registered there) --
    input        fv_ev,
    input [31:0] fv_adr,        // the instruction's address (physical)
    input [31:0] fv_pc,         // the one executed before it (physical)
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
    reg        tq_v;            // ...a load or store
    reg        tq_f;            // ...a fetch
    reg        p_privreg_h, p_gate_h, p_own_h;
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

    wire cfg_sel = !c_instr && ((c_adr & 32'hffff_ff00) == CFG_BASE);

    wire [3:0] nib = tq_adr[31:28];
    wire nib_main = MAIN_512 ? (nib[3:1] == 3'b010) : (nib == 4'h4);

    wire t_own = nib_main && (tq_adr >= mtu_base_i) && (tq_adr < limit) &&
                 (size != 32'h0);
    wire t_granted = nib_main && (tq_adr >= gbase) && (tq_adr < glimit);

    wire kernel_only_store =
        (nib == 4'h0) || (nib == 4'h1) || (nib == 4'h9) ||
        (nib_main && !t_own && !t_granted) ||
        (tq_adr[31:8] == 24'h7000_01) ||
        (tq_adr[31:2] == 30'h1C00_0086) ||        // 0x7000_0218
        (tq_adr[31:24] == 8'h7b);                  // DMA (rtl/dma.v): writes physical memory

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

    // -- fetches: no uncached fetch while unprivileged ----------------

    wire i_main = MAIN_512 ? (c_adr[31:29] == 3'b010) : (c_adr[31:28] == 4'h4);
    assign i_veto = en && enforce && !i_priv &&
                    !((c_adr[31:13] == 19'd0) || i_main);

    // -- a fetch's properties, on the same registered copy -------------

    wire p_privreg_c = (tq_adr[31:13] == 19'd0) || (nib_main && (tq_adr < ktext));
    wire p_gate_c    = (tq_adr == gate) || (tq_adr == 32'h0000_0010);
    assign p_privreg = tq_f ? p_privreg_c : p_privreg_h;
    assign p_gate    = tq_f ? p_gate_c    : p_gate_h;
    assign p_own     = tq_f ? t_own       : p_own_h;

    // the core supplies physical PCs (it keeps each instruction's fetch
    // address), so nothing is translated here
    wire [31:0] tq_ppc  = tq_pc;
    wire [31:0] fv_padr = fv_adr;
    wire [31:0] fv_ppc  = fv_pc;

    wire tq_viol = tq_v && (tq_why != 4'd0);
    wire [3:0] tq_reg = tq_adr[5:2];

    always @(posedge wb_clk_i) begin
        // what was taken this cycle, for next cycle (no enables)
        tq_v   <= c_req && c_rdy && !c_instr;
        tq_f   <= c_req && c_rdy && c_instr;
        tq_adr <= c_adr;
        tq_we  <= c_we;
        tq_sel <= c_sel;
        tq_dat <= c_wdat;
        if (tq_f) begin
            p_privreg_h <= p_privreg_c;
            p_gate_h    <= p_gate_c;
            p_own_h     <= t_own;
        end
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
            tq_f <= 1'b0;
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
