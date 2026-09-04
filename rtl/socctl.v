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
 * codebase, e.g. rtl/gpio.v, rtl/csrs.v):
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
 *   3  GAME   bit 0: game mode. 0 = desktop (640x480 native, 1:1),
 *             1 = game (320x240 viewport, pixel-doubled on scanout).
 *             bit 1: wrap. 0 = the viewport is clamped so it can
 *             never leave the framebuffer; 1 = it wraps toroidally,
 *             so scrolling off the right edge comes back on the left.
 *             See VIEW below and rtl/gpu/gpu_video.v.
 *
 *             Reads back as { 16'h5A47, 13'b0, avail, wrap, en }.
 *             Same signature trick, and the same reason for it, as
 *             VIDEO above: socctl shipped before this register
 *             existed, and on one of those bitstreams a read here
 *             falls to the default case and returns 0 -- which is
 *             indistinguishable from a working block reporting
 *             "game mode off". Software checking the top half gets a
 *             real answer either way.
 *
 *             `avail` is GAME_AVAIL, from rtl/boards.vh's `GAME via
 *             rtl/sysctl.v. It is read-only, and when it is clear the
 *             enable bit is FORCED LOW rather than merely ignored --
 *             so a bitstream built without game mode reports the
 *             truth at every level (the CSR feature bit is clear, the
 *             avail bit here is clear, and the enable bit reads back
 *             0 no matter what software writes) instead of claiming a
 *             mode the scanout hardware cannot produce.
 *
 *   4  VIEW   viewport origin in FRAMEBUFFER pixels:
 *             bits 9:0 = x, bits 25:16 = y. Ignored entirely in
 *             desktop mode. Range-limited on write to 0..639 / 0..479
 *             so a wild write can never point scanout outside VRAM;
 *             the tighter clamp that keeps the whole 320x240 viewport
 *             on screen (x <= 320, y <= 240) is applied in
 *             rtl/gpu/gpu_video.v at the frame boundary instead, and
 *             ONLY when wrap is off -- see that file. Reads back what
 *             was written (after the 0..639/0..479 limit), not the
 *             clamped value actually in use, because the clamp
 *             depends on a mode bit in another register and software
 *             that wrote a coordinate should be able to read that
 *             coordinate back.
 *
 *             GAME and VIEW are latched into the pixel clock domain
 *             TOGETHER, as one payload, on a toggle flipped by a
 *             write to either -- so enabling game mode and setting an
 *             origin in two consecutive stores can never be observed
 *             as one-then-the-other with a frame of the wrong origin
 *             in between. See view_load below.
 *
 *   5  FRAME  read-only. { 15'b0, vblank, frame[15:0] } -- the
 *             framebuffer's own frame counter and current vertical
 *             blanking state, from rtl/gpu/gpu_video.v.
 *
 *             This is what makes tear-free page flipping possible for
 *             a game, and it is the reason it exists: the viewport
 *             origin is adopted at a frame boundary, so a flip IS a
 *             VIEW write, but software still has to know when the
 *             frame it drew has actually been shown. Polling a
 *             counter is much cheaper than a vblank interrupt line
 *             and is what a full-screen game's main loop wants
 *             anyway.
 *
 *             No signature of its own -- it ships with GAME above and
 *             GAME's signature covers both. On a board built without
 *             `GPU this reads back all zeroes forever, which is
 *             correct: there is no scanout, so there are no frames.
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
    parameter [1:0] VIDEO_MODE_RESET = 2'd0,

    // 1 if this bitstream was built with `GAME (rtl/boards.vh) AND
    // has a GPU to scan out with. Set by rtl/sysctl.v, which is where
    // both of those defines are visible; socctl gets a number, the
    // same arrangement VIDEO_MODE_RESET already uses.
    //
    // Defaults to 0 -- a socctl instantiated without being told
    // anything reports no game mode, which is the safe answer.
    parameter GAME_AVAIL = 1'b0
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
    output wire [1:0] video_mode,

    // -- game mode configuration -> rtl/gpu/gpu_video.v --
    //
    // These four are ONE payload, not four independent signals, and
    // they are deliberately not synchronised here. view_load is a
    // TOGGLE, flipped on any write to GAME or VIEW; gpu_video.v
    // synchronises that single bit into the pixel clock domain and
    // captures all of game_en/game_wrap/view_x/view_y on the edge.
    //
    // Two flops on a multi-bit value is not enough on its own -- the
    // bits can resolve on different cycles, so a 22-bit payload can
    // be observed as a mixture of the old and the new value. For a
    // colour mode that would be one frame drawn in the wrong colour,
    // which is why VIDEO above gets away with it. For a viewport
    // origin it is a visible one-frame jump in the middle of smooth
    // scrolling, and for a page flip it is a whole frame of the wrong
    // buffer -- both of which are exactly the artefacts this feature
    // exists to avoid.
    //
    // The toggle fixes that the same way rtl/gpu/gpu_video.v's own
    // refill_toggle/y_refill pair already does: the data is written
    // on the same wb_clk edge that flips the toggle, so by the time
    // the toggle's edge has propagated through the far side's
    // synchroniser the data has been stable for several pixel clocks.
    output wire        view_load,
    output wire        game_en,
    output wire        game_wrap,
    output wire [9:0]  view_x,
    output wire [9:0]  view_y,

    // -- scanout status, from rtl/gpu/gpu_video.v --
    //
    // Already in the wishbone clock domain when they arrive here (see
    // that file's own vblank_toggle crossing), so this block just
    // reads them out. Tied to zero by rtl/sysctl.v on a board with no
    // `GPU, which is the honest answer -- no scanout, no frames --
    // rather than a stuck counter software might wait on forever.
    input  wire [15:0] frame_ctr,
    input  wire        in_vblank
);

    localparam MAGIC = 32'h5A43_5452;   // "ZCTR"

    // top half of the VIDEO register -- "VC", video colour. See this
    // file's header comment for why a second signature is needed when
    // MAGIC already exists.
    localparam VIDEO_SIG = 16'h5643;

    // top half of the GAME register -- "ZG". Same purpose as
    // VIDEO_SIG directly above; see this file's header comment.
    localparam GAME_SIG = 16'h5A47;

    reg [31:0] ctrl;
    reg [1:0] video;

    reg game;
    reg wrap;
    reg [9:0] vx;
    reg [9:0] vy;
    reg vload;

    assign cursor_busy = ctrl[0];
    assign video_mode = video;

    // FORCED low, not merely ignored, on a bitstream without game
    // mode -- see the GAME register's note in this file's header on
    // why the lie is worth avoiding. This is the gate that makes it
    // true at the hardware level rather than only in the readback:
    // gpu_video.v is handed a 0 here regardless of what software
    // wrote, so there is no path by which a build with no game
    // support can be talked into half-entering it.
    assign game_en   = game && (GAME_AVAIL != 0);
    assign game_wrap = wrap;
    assign view_x    = vx;
    assign view_y    = vy;
    assign view_load = vload;

    // Range limit on the VIEW write path. This is NOT the clamp that
    // keeps the viewport on screen -- that one depends on the wrap
    // bit and lives in gpu_video.v (see the header). This is the much
    // blunter guarantee that scanout can never be pointed outside the
    // 9600 words vram_wb actually has, whatever software writes and
    // in whatever order it writes it. Reading past the end of that
    // array is undefined in simulation and returns whatever the BRAM
    // happens to hold on hardware, so it is worth two comparators to
    // make it structurally impossible rather than merely unlikely.
    wire [9:0] vx_wr = (wb_dat_i[9:0]   > 10'd639) ? 10'd639 : wb_dat_i[9:0];
    wire [9:0] vy_wr = (wb_dat_i[25:16] > 10'd479) ? 10'd479 : wb_dat_i[25:16];

    always @(posedge wb_clk_i) begin

        if (wb_rst_i) begin

            // busy at reset -- see this file's header comment
            ctrl <= 32'h0000_0001;
            video <= VIDEO_MODE_RESET;

            // DESKTOP at reset, on every board, even where `GAME is
            // defined. `GAME says the mode is available, not that it
            // is on: the machine boots into a window manager on a
            // 640x480 desktop and software asks for game mode later.
            // Coming up in a 320x240 viewport would mean the boot
            // logo, the BIOS messages and any early panic all landed
            // mostly off-screen on a board whose only display is a
            // TV -- which is precisely the situation with the least
            // margin for a mode nobody asked for.
            game <= 1'b0;
            wrap <= 1'b0;
            vx <= 10'd0;
            vy <= 10'd0;
            vload <= 1'b0;

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

                    // GAME and VIEW both flip vload. One toggle for
                    // two registers is the point, not a shortcut:
                    // gpu_video.v captures the whole payload on the
                    // edge, so "enter game mode" and "set the origin"
                    // written as two consecutive stores are adopted
                    // as one coherent change at one frame boundary
                    // rather than as two changes a frame apart.
                    //
                    // Flipped unconditionally on a write in range,
                    // including a write that changes nothing. A
                    // redundant capture of an unchanged payload costs
                    // nothing and is far easier to reason about than
                    // a "did anything actually change" comparison
                    // that has to stay correct as fields are added.
                    if (wb_adr_i == 32'd3) begin
                        if (wb_sel_i[0]) begin
                            game <= wb_dat_i[0];
                            wrap <= wb_dat_i[1];
                            vload <= ~vload;
                        end
                    end

                    // Lanes 0/1 carry x, lanes 2/3 carry y, so each
                    // coordinate is honoured or not as a unit -- a
                    // halfword store to the low half moves x and
                    // leaves y alone, which is the only byte-lane
                    // behaviour that makes sense for a pair of
                    // 10-bit fields. Software writes whole words.
                    if (wb_adr_i == 32'd4) begin
                        if (wb_sel_i[0] || wb_sel_i[1]) vx <= vx_wr;
                        if (wb_sel_i[2] || wb_sel_i[3]) vy <= vy_wr;
                        vload <= ~vload;
                    end

                end else begin

                    case (wb_adr_i)
                        32'd0: wb_dat_o <= ctrl;
                        32'd1: wb_dat_o <= MAGIC;
                        32'd2: wb_dat_o <= { VIDEO_SIG, 14'b0, video };
                        // reads back game_en, not `game` -- so a
                        // build without game support reports the
                        // enable bit clear no matter what was
                        // written, matching what the hardware is
                        // actually doing. See the assign above.
                        32'd3: wb_dat_o <= { GAME_SIG, 13'b0,
                                             (GAME_AVAIL != 0), wrap, game_en };
                        32'd4: wb_dat_o <= { 6'b0, vy, 6'b0, vx };
                        32'd5: wb_dat_o <= { 15'b0, in_vblank, frame_ctr };
                        default: wb_dat_o <= 32'h0000_0000;
                    endcase

                end

            end

        end

    end

endmodule
