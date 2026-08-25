/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SOC control -- a small bank of WRITABLE global configuration bits.
 *
 * -- Why this exists separately from rtl/csrs.v --
 *
 * csrs.v is the read-only sibling of this block: it answers "what does
 * this bitstream have", and its own header is explicit that it has no
 * state machine and no side effects. That inertness is the whole
 * reason it can be trusted, and it is why it is the one block with no
 * `ifdef guard -- it cannot itself be one of the things that might be
 * missing.
 *
 * Putting writable state in there would muddle that. So configuration
 * that software SETS lives here instead, in a sibling block with the
 * same always-present property but an explicit write path.
 *
 * -- Why not its own address nibble --
 *
 * There isn't a good one left. rtl/sysctl.v's map is nearly full, and
 * the only free top nibble (0x8) is the virtual window apps execute in
 * -- a stale app pointer dereferenced in kernel context would land on
 * control registers, which is a bad failure mode to invent for the
 * sake of tidiness.
 *
 * Nibble 0x7 is already subdivided (0x7000_00xx csrs, 0x7000_01xx
 * instruction cache), so this is the third tenant at 0x7000_02xx
 * rather than a new precedent.
 *
 * -- Why a whole block for one bit --
 *
 * Because the alternative was worse. The mouse cursor sprite
 * (rtl/gpu/gpu_cursor.v) takes its position straight from
 * rtl/usb_hid.v and has no wishbone connection at all, so changing its
 * SHAPE had no register to live in. Giving the cursor its own
 * peripheral, with its own decode and its own ack mux entry, would be
 * a lot of address map for one bit -- and the next global config bit
 * would face the same question again. This is the place for those.
 *
 * Register map (word-addressed -- wb_adr_i here is rtl/sysctl.v's
 * wbm_adr_sel_word, matching every other simple slave in this
 * codebase, e.g. rtl/debug.v, rtl/csrs.v):
 *
 *   0  CTRL   bit 0: cursor shape. 0 = normal (X), 1 = busy (Z).
 *             Resets to 1. bits 31:1 reserved, must be written 0.
 *   1  MAGIC  fixed 32'h5A43_5452 ("ZCTR"). Same purpose as csrs.v's
 *             own MAGIC: reading an address nothing decodes does not
 *             fault on this bus, so a known constant is the only way
 *             software can tell "this block is present" from "this is
 *             whatever the bus happened to resolve to".
 *
 * Reset state is BUSY (cursor = Z), not idle.
 *
 * That is deliberate and is the honest default: from power-on until
 * the window manager says otherwise, the system genuinely is still
 * coming up. Defaulting to the normal pointer would show a ready
 * cursor over a machine that is not ready yet, and would also mean the
 * Z never appears at all on the fast path where the core apps load
 * before wm first writes this register.
 *
 * It also fails in the right direction: on a board where wm never runs
 * (no apps, or a crash during startup) the cursor stays Z, which is
 * exactly what is true.
 */

module socctl_wb #()
(
    input wb_clk_i,
    input wb_rst_i,
    input [31:0] wb_adr_i,
    input [31:0] wb_dat_i,
    output reg [31:0] wb_dat_o,
    input wb_we_i,
    input [3:0] wb_sel_i,
    input wb_stb_i,
    output reg wb_ack_o,
    input wb_cyc_i,

    // 1 = draw the busy cursor. Feeds rtl/gpu/gpu_cursor.v's curs_alt
    // directly -- see that module. Crosses into the pixel clock domain
    // there, which is safe without synchronisation because it is a
    // single bit changed by a human-paced event (a window manager
    // deciding it is busy), and the worst case of sampling it mid-flip
    // is one frame drawn with the old shape.
    output wire cursor_busy
);

    localparam MAGIC = 32'h5A43_5452;   // "ZCTR"

    reg [31:0] ctrl;

    assign cursor_busy = ctrl[0];

    always @(posedge wb_clk_i) begin

        if (wb_rst_i) begin

            // busy at reset -- see this file's header comment
            ctrl <= 32'h0000_0001;
            wb_ack_o <= 1'b0;
            wb_dat_o <= 32'h0000_0000;

        end else begin

            wb_ack_o <= 1'b0;

            if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin

                wb_ack_o <= 1'b1;

                if (wb_we_i) begin

                    // byte lanes honoured like every other writable
                    // register in this codebase (see rtl/mtu.v), even
                    // though software only ever writes whole words --
                    // a byte store to a register that silently wrote
                    // all four lanes would be a genuinely puzzling bug
                    if (wb_adr_i == 32'd0) begin
                        if (wb_sel_i[0]) ctrl[7:0]   <= wb_dat_i[7:0];
                        if (wb_sel_i[1]) ctrl[15:8]  <= wb_dat_i[15:8];
                        if (wb_sel_i[2]) ctrl[23:16] <= wb_dat_i[23:16];
                        if (wb_sel_i[3]) ctrl[31:24] <= wb_dat_i[31:24];
                    end

                end else begin

                    case (wb_adr_i)
                        32'd0: wb_dat_o <= ctrl;
                        32'd1: wb_dat_o <= MAGIC;
                        default: wb_dat_o <= 32'h0000_0000;
                    endcase

                end

            end

        end

    end

endmodule
