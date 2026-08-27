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
 *   2  VIDEO  bits 1:0: virtual phosphor mode, driven out to
 *             rtl/gpu/gpu_video.v. 00 white-on-black, 01 amber,
 *             10 green, 11 paper (black-on-white). Resets to
 *             VIDEO_MODE_RESET, which rtl/sysctl.v derives from the
 *             board's `GPU_AMBER/`GPU_GREEN/`GPU_PAPER defines so a
 *             board that used to synthesize green still COMES UP
 *             green -- the defines now choose a power-on default
 *             instead of a permanent wiring.
 *
 *             Reads back as { 16'h5643, 14'b0, mode }. The signature
 *             is not decoration: a bitstream can have socctl (so
 *             MAGIC is right) and still predate this register, and on
 *             one of those the register-2 read falls to the default
 *             case and returns 0 -- which is indistinguishable from a
 *             perfectly working block reporting white. Software
 *             checking the top half gets a real answer either way.
 *             Same trick, and the same reason for it, as
 *             rtl/cache.v's Z_ICACHE_MAGIC.
 *
 *             VIDEO is a SEPARATE register rather than spare bits in
 *             CTRL. That is not tidiness: z_cursor_set_busy()
 *             (sw/common/zsoc.h) writes CTRL as a whole word, so
 *             sharing would mean every busy/idle transition in
 *             sw/apps/wm silently reset the display to white. A
 *             read-modify-write in the C helper would fix that and
 *             would also be one non-atomic sequence away from doing
 *             it again the first time anything else touches CTRL.
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

module socctl_wb #(
    // Power-on virtual phosphor mode. Set by rtl/sysctl.v from the
    // board's own defines -- see this file's header and the VIDEO
    // register below. Defaults to white-on-black, matching the
    // behaviour of a board that defines none of them.
    parameter [1:0] VIDEO_MODE_RESET = 2'd0
)
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
    output wire cursor_busy,

    // Virtual phosphor mode -> rtl/gpu/gpu_video.v's own video_mode
    // input. Crosses into the pixel clock domain there rather than
    // here, because that is where the timing needed to make the change
    // land on a frame boundary lives -- see gpu_video.v's own
    // synchroniser comment. Declared unconditionally, exactly like
    // cursor_busy above: on a board built without `GPU it simply goes
    // nowhere.
    output wire [1:0] video_mode
);

    localparam MAGIC = 32'h5A43_5452;   // "ZCTR"

    // top half of the VIDEO register -- "VC", video colour. See this
    // file's header comment for why a second signature is needed when
    // MAGIC already exists.
    localparam VIDEO_SIG = 16'h5643;

    reg [31:0] ctrl;
    reg [1:0] video;

    assign cursor_busy = ctrl[0];
    assign video_mode = video;

    always @(posedge wb_clk_i) begin

        if (wb_rst_i) begin

            // busy at reset -- see this file's header comment
            ctrl <= 32'h0000_0001;
            video <= VIDEO_MODE_RESET;
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

                    // Only lane 0 matters -- the mode is two bits and
                    // the rest of the word is a read-only signature,
                    // so there is nothing for the upper lanes to
                    // write. Every value of wb_dat_i[1:0] is a legal
                    // mode, so no range check is needed or wanted:
                    // rejecting a write here would be invisible to
                    // software, which cannot see an error on this bus.
                    if (wb_adr_i == 32'd2) begin
                        if (wb_sel_i[0]) video <= wb_dat_i[1:0];
                    end

                end else begin

                    case (wb_adr_i)
                        32'd0: wb_dat_o <= ctrl;
                        32'd1: wb_dat_o <= MAGIC;
                        32'd2: wb_dat_o <= { VIDEO_SIG, 14'b0, video };
                        default: wb_dat_o <= 32'h0000_0000;
                    endcase

                end

            end

        end

    end

endmodule
