/*
 * Zeitlos SOC GPU
 * Copyright (c) 2021 Lone Dynamics Corporation. All rights reserved.
 *
 * Video timing generator for 640x480@60Hz (VESA DMT standard timing --
 * pixel clock 25.175MHz nominally, driven here at 25.2MHz (pll1.v's
 * CLKOS, sharing a VCO with the TMDS bit clock for an exact 5:1 ratio
 * -- see pll1.v's own comment). 25.2 vs true 25.175 is 0.099% high,
 * landing almost exactly on 60.000Hz rather than the traditional
 * 59.94Hz -- and well within any display's tolerance either way.
 *
 * 60Hz over 75Hz: 640x480@60Hz is about the most universally-
 * recognized display timing that exists -- the original 1987 VGA
 * standard, and one of the mandatory baseline timings in the CEA-861
 * spec HDMI TVs are built around, unlike 75Hz VESA modes which are
 * much more of a "computer monitor" convention plenty of TVs simply
 * don't list support for.
 *
 * Native resolution -- GPU_PIXEL_DOUBLE (rendering at half this and
 * doubling both axes on scanout) is gone. That scheme is what let a
 * 512x384 framebuffer fill a 1024x768 signal; it's not needed here
 * since 640x480 is now both the framebuffer's own resolution and the
 * display timing's h_disp/v_disp, in native 1:1 correspondence -- x/y
 * below are used directly as VRAM row/column indices, no hx/hy
 * halving.
 *
 * -- GAME MODE --
 *
 * Game mode brings pixel doubling back, but for a completely
 * different reason and with a completely different shape, so it is
 * worth being explicit that this is NOT GPU_PIXEL_DOUBLE returning.
 *
 * That scheme shrank the framebuffer to match a doubled signal. This
 * one leaves the 640x480 framebuffer entirely alone and instead moves
 * a 320x240 CAMERA over it, doubling what the camera sees to fill the
 * same 640x480 signal. The video timing does not change at all -- the
 * monitor sees the identical 640x480@60Hz mode either way, which is
 * the whole point on a TV.
 *
 * The consequences of that framing are what make this cheap:
 *
 *   - VRAM is unchanged. So are rtl/mem/vram.v, rtl/arbiter_vram.v,
 *     rtl/gpu/gpu_raster.v and rtl/gpu/gpu_blit.v. The rasterizer and
 *     the blitter go on drawing into a 640x480 1bpp surface and
 *     neither of them has any idea a camera exists. A game's back
 *     buffer is just a region of that surface the camera is not
 *     currently pointed at.
 *
 *   - No new BRAM, and no extra VRAM bandwidth. The line buffer below
 *     already holds one FULL framebuffer row (640 bits, 20 words) per
 *     physical scanline. The viewport's x offset is a different index
 *     into a buffer that was already being fetched; its y offset is a
 *     different row number. In game mode each framebuffer row is
 *     fetched twice, once per physical row -- exactly what
 *     GPU_PIXEL_DOUBLE did, so the refill machinery below needed no
 *     change whatsoever.
 *
 *   - A page flip is a register write. There is no such thing as a
 *     "page" in this file: the origin is an arbitrary (x,y), and
 *     640x480 happens to hold four non-overlapping 320x240 pages.
 *     Which of them software calls front and back is entirely a
 *     software convention, and one it can change per game.
 *
 * The two knobs, both adopted at a frame boundary (see below):
 *
 *   game_en    0 = desktop, 1:1, the viewport ignored entirely.
 *              1 = 320x240 viewport, doubled on both axes.
 *
 *   game_wrap  0 = CLAMP. The origin is limited to x <= 320,
 *              y <= 240, so the viewport can never hang off the edge
 *              of the framebuffer. This is what the desktop wants:
 *              scrolling to look at the dock and then finding the
 *              right-hand edge of the screen wrapped around to the
 *              left would be disorienting rather than useful.
 *
 *              1 = WRAP. The framebuffer is a torus: column 639 is
 *              followed by column 0, and row 479 by row 0. This is
 *              what a scrolling game wants -- it makes the 640x480
 *              surface an infinitely scrollable world where only the
 *              leading edge has to be redrawn as it comes around,
 *              rather than a bounded 2x2-screen playfield.
 *
 *              It costs one comparator on a counter that already
 *              exists plus one conditional subtract, both in the
 *              25.2MHz pixel domain, which is where the slack is.
 *
 * -- what changed in the pixel path, and what deliberately did not --
 *
 * The framebuffer bit for the current pixel is bit x[4:0] of the word
 * the line is showing (cur0, below), and x counts framebuffer columns
 * 0..639. x is a LOADABLE COUNTER rather than a subtraction of hc,
 * loaded with the viewport origin at the start of each line and
 * advanced every pixel (desktop) or every other pixel (game).
 *
 * That structure is not an accident and should not be "simplified"
 * back into an expression: putting an adder in front of the pixel
 * select -- view_x + (x >> 1), the obvious way to write this -- would
 * add that adder's delay to the pixel path for no functional gain. A
 * counter puts the arithmetic behind a register.
 *
 * The row itself used to be a 640-bit register, hline, and the pixel
 * hline[x]: a 640:1 multiplexer, the critical path in this clock
 * domain, and 640 flops refilled from the wishbone domain. It is now
 * a 20-word row of the plane line buffer (a block RAM the colour
 * planes already used), read a word at a time into cur0 the same way
 * planes 1..3 are, so the pixel is a 32:1 mux of a register.
 *
 * The x and y OUTPUTS now carry framebuffer coordinates rather than
 * screen coordinates. In desktop mode those are the same thing, so
 * nothing changes. In game mode it means the hardware cursor (the
 * bottom of this file, docs/cursor.md) needs nothing of its own: it
 * compares against framebuffer coordinates, so it draws the pointer at
 * the right place in the framebuffer -- and because consecutive screen
 * pixels map to the same framebuffer coordinate, the sprite comes out
 * pixel-doubled along with everything else, for free.
 *
 * -- one off-by-one fixed on the way through --
 *
 * The old `if (hc > h_disp_start) x <= hc - h_disp_start` produced
 * x = 0 for the first TWO visible pixels of every line and never
 * produced x = 639 at all: the whole display was shifted one pixel
 * right and the last column was dropped. On a 1:1 desktop that is
 * invisible (a one pixel shift, into overscan) and it went unnoticed
 * for exactly that reason.
 *
 * It does not stay invisible under 2x doubling -- it becomes a
 * three-pixel-wide first column against two everywhere else, a
 * visible seam at the left edge of a scrolling playfield. The
 * counter's load boundary below fixes it (load while hc <
 * h_disp_start, so x holds the origin on the first visible cycle and
 * ends the line on 639). Desktop output therefore moves one pixel
 * LEFT relative to every previous bitstream and gains its rightmost
 * column back. Nothing in software depends on the old behaviour, but
 * it is a real change to what appears on the glass, and it is called
 * out here rather than buried.
 */

module gpu_video #(

	parameter [10:0] h_disp = 640,
	parameter [10:0] h_front_porch = 16,
	parameter [10:0] h_pulse_width = 96,
	parameter [10:0] h_back_porch = 48,
	parameter [10:0] h_line = 800,
	parameter [10:0] v_disp = 480,
	parameter [10:0] v_front_porch = 10,
	parameter [10:0] v_pulse_width = 2,
	parameter [10:0] v_back_porch = 33,
	parameter [10:0] v_frame = 525,

	// -- scanout scaling --
	//
	// H_DIV_BASE is how many pixel clocks one SOURCE pixel occupies in
	// the base (non-game) mode. 1 for VGA/DDMI, where the framebuffer
	// and the signal are 1:1. 4 for composite, where 320 source pixels
	// are spread across 1280 clocks of active video -- see
	// docs/composite.md for why 320 and not 640.
	//
	// FIXED_VIEWPORT makes the 320x240 viewport permanently active,
	// independent of socctl's game bit. Composite sets it, because on
	// composite there is no other mode available: the luma bandwidth
	// to draw 640 distinct pixels across a 52us active line is 12.6MHz
	// and the channel carries about 4.2 (NTSC) or 5.5 (PAL). A "640
	// wide" composite picture is a blur of the correct average
	// brightness, not a picture.
	//
	// So on a composite board the viewport is not an optional extra
	// mode -- it is the only mode, and CTRL-ALT-ARROW is how the rest
	// of the desktop is reached. That is a design consequence worth
	// stating rather than discovering.
	parameter [2:0] H_DIV_BASE = 3'd1,
	parameter FIXED_VIEWPORT = 1'b0,

	// -- game-mode colour (docs/color.md) --
	//
	// 1 builds the plane fetcher, the plane line buffer (one block
	// RAM) and the palette. 0 -- the default, so every existing
	// instantiation and every existing testbench is unchanged -- makes
	// color_on below a constant 0, and yosys removes the lot: no
	// extra VRAM reads, no block RAM, no palette, and the output
	// muxes fold back to exactly the monochrome path.
	parameter COLOR_AVAIL = 1'b0,

	// The most bitplanes colour shows: 4, 6 or 8 (rtl/boards.vh's
	// `GAME_PLANES; docs/color.md, "More than four planes"). Planes
	// 4..7 are planes 0..3 of the second VRAM sheet, rows 480..959 --
	// 480..719 with 6, where that sheet is half height -- and the
	// palette has 2^GAME_PLANES entries. Ignored without COLOR_AVAIL.
	parameter integer GAME_PLANES = 4,

	// 1 keeps the cursor's shapes and the palette in distributed (LUT)
	// RAM instead of their block RAM (cbuf, below): one DP16KD fewer,
	// for a few dozen LUTs, on a board that is one block short
	// (rtl/boards.vh's `GPU_CURSOR_LUTRAM). Same behaviour, same
	// timing: the reads are registered just as the block RAM's are.
	parameter CBUF_LUTRAM = 1'b0,

	// 1 builds the colour window (docs/color.md, "The colour window"):
	// in desktop mode, one rectangle of the 640x480 desktop shows in
	// colour, its pixel's index the desktop's own bit plus the second
	// sheet's planes (4..7) -- 8 colours with GAME_PLANES 6, 32 with 8.
	// Needs COLOR_AVAIL and more than four planes; ignored otherwise,
	// and on composite (FIXED_VIEWPORT). 0 -- the default, so every
	// existing bench is unchanged -- builds none of it.
	parameter WIN_AVAIL = 1'b0,

	// -- the hardware cursor (docs/cursor.md) --
	//
	// 1 draws the pointer from four 16x16 shapes kept in the spare
	// words of the plane line buffer (pbuf, below). 0 -- the default,
	// so every testbench is unchanged -- draws none, and only `pixel`
	// below can invert a pixel.
	parameter CURSOR_AVAIL = 1'b0

) (

	// An extra invert, XORed in with the cursor's own. Tied to 0 by
	// rtl/sysctl.v; the benches use it to stand in for a cursor.
	input pixel,

	// -- the hardware cursor (CURSOR_AVAIL) --
	//
	// curs_x/y: the pointer, from rtl/usb_hid.v (or the virtual
	// mouse), in framebuffer coordinates. curs_alt (busy) and
	// curs_hide come from socctl's CTRL, curs_slots from its CURSOR
	// register: { busy slot, normal slot }. All are used as they are
	// in the pixel domain, as the old comparator cursor (gpu_cursor.v)
	// used them: each is a
	// human-paced value, and the worst a change mid-line can do is
	// draw one line with the old one.
	//
	// curs_we writes one shape row, curs_wdata, to row curs_waddr
	// ({ slot, row }) of the cursor's own block RAM: a one-clock
	// strobe from socctl, in this module's clk (wishbone) domain.
	input [9:0] curs_x,
	input [9:0] curs_y,
	input curs_alt,
	input curs_hide,
	// curs_scr: in game mode, curs_x/y are SCREEN coordinates (0..639,
	// 0..479 on the doubled picture) rather than framebuffer ones: the
	// cursor is drawn at half of them, where the pointer is on screen
	// with the view at (0, 0) -- the arcade runtime's (socctl CTRL bit
	// 2, docs/socctl.md). A shift is wiring: two 10-bit 2:1 muxes.
	input curs_scr,
	input [3:0] curs_slots,
	input curs_we,
	input [5:0] curs_waddr,
	input [31:0] curs_wdata,

	input clk,
	input pclk,
	input bclk,
	input resetn,

	// Virtual phosphor mode, from rtl/socctl.v's VIDEO register (see
	// that file, and docs/socctl.md). In the WISHBONE clock domain,
	// not pclk -- synchronised below before anything looks at it.
	//
	// 00 white-on-black (default)  01 amber-on-black
	// 10 green-on-black            11 black-on-white ("paper")
	input [1:0] video_mode,

	// DDMI audio: data islands on the DDMI output (socctl's DDMI
	// register, docs/ddmi_audio.md). Wishbone domain, synchronised and
	// adopted at the frame boundary like video_mode. Only looked at in
	// a build with `GPU_DDMI_AUDIO; anything else ignores it.
	input ddmi_en,

	// ... and its sound half (socctl's DDMI, ACRN, ACRCTS), adopted
	// with it; and the frames to send, written on `clk` by rtl/audio.v
	// into the FIFO in rtl/gpu/ddmi_island.v.
	input ddmi_sound,
	input [3:0] ddmi_fs,
	input [19:0] ddmi_acr_n,
	input [19:0] ddmi_acr_cts,
	input ddmi_we,
	input [31:0] ddmi_wdata,

	// Monochrome brightness on DDMI, 0..15, from the same VIDEO
	// register; wishbone domain, synchronised and adopted with
	// video_mode. 7 is the 0x80 ink this output has always used. See
	// ddmi_ink_* below and docs/socctl.md.
	input [3:0] video_bright,

	// -- game mode, from rtl/socctl.v's GAME/VIEW registers --
	//
	// One payload, latched together. view_load is a TOGGLE in the
	// WISHBONE clock domain, flipped by a write to either register;
	// the other four are the data, written on the same edge that
	// flips it. See socctl.v's own comment for why a plain 2-flop
	// synchroniser on 22 bits would not do, and the capture logic
	// below for the timing margin that makes the toggle safe.
	input view_load,
	input game_en,
	input game_wrap,
	input [9:0] view_x,
	input [9:0] view_y,

	// -- game-mode colour, from rtl/socctl.v's COLOR register --
	//
	// Part of the SAME payload as game_en/view_x/view_y: captured on
	// the same view_load edge and adopted at the same frame boundary,
	// so "enter game mode, set the origin, switch colour on" written
	// as three stores lands as one change on one frame.
	//
	// color_np is planes-1 (0..7 = 2 .. 256 colours; socctl never
	// asks for more than GAME_PLANES). color_off holds a 2-bit
	// {dy, dx} offset for each of planes 1..3, in that order from bit
	// 0: dx adds 320 columns, dy adds 240 rows, both modulo the
	// 640x480 torus. Plane 0 is the viewport itself. Planes 4..7 sit
	// in the second sheet where planes 0..3 sit in the first, so they
	// take the same offsets.
	input color_en,
	input [2:0] color_np,
	input [5:0] color_off,

	// -- the colour window, from rtl/socctl.v's COLWIN/COLWINEND --
	//
	// Part of the same payload again: captured on view_load and adopted
	// at the frame boundary. The rectangle is x0 <= x < x1, y0 <= y < y1
	// in framebuffer (= desktop) pixels; software keeps it within
	// 320x240, the most the second sheet's fold can tell apart. Ignored
	// in game mode.
	input win_en,
	input [9:0] win_x0,
	input [9:0] win_y0,
	input [9:0] win_x1,
	input [9:0] win_y1,

	// Palette write port, wishbone clock domain (= clk). One-cycle
	// strobe from socctl; 12-bit RGB444 into entry pal_idx (its bits
	// above GAME_PLANES are ignored).
	input pal_we,
	input [7:0] pal_idx,
	input [11:0] pal_rgb,

	// Composite only: 1 = no colour on the composite output -- the
	// palette's brightness alone, and no colour burst, so the receiver
	// shows black and white. From socctl's CVBS register, wishbone
	// domain; adopted at a frame boundary below. Ignored everywhere
	// else.
	input cvbs_mono,

	// -- scanout status, out to rtl/socctl.v's FRAME register --
	//
	// Both already in the WISHBONE clock domain: the crossing happens
	// below, here, rather than in socctl, for the same reason
	// video_mode's crossing happens here rather than there -- this is
	// the module that knows where a frame boundary is.
	output reg [15:0] frame_ctr,
	output reg in_vblank,

	output red,
	output green,
	output blue,
	output hsync,
	output vsync,

	output [3:0] dvi_p,
`ifdef XC7
	// The negative halves of the TMDS pairs.
	//
	// Only on 7-series, and that is a difference in how the two
	// families express a differential output rather than a difference
	// in what comes out of the connector. ECP5 does it with ONE port
	// constrained as LVCMOS33D: the .lpf names the P ball and the tool
	// drives the adjacent N ball for you, so a second port would have
	// nothing to attach to. Xilinx does it with an explicit OBUFDS
	// having two pins, so the port has to exist.
	//
	// BOTH halves must be constrained in the XDC: nextpnr-xilinx places
	// an unconstrained N port wherever it likes, and then refuses an
	// OBUFDS whose N side landed on a master pin. See
	// boards/sergei_mx1.xdc.
	output [3:0] dvi_n,
`endif

	output [3:0] dac,

	output reg [9:0] x,
	output reg [9:0] y,
	output is_visible,

	output reg [14:0] gb_adr_o,
	input [31:0] gb_dat_i,

);

	reg [10:0] hc;
	reg [10:0] vc;

	// video timing -- here, before the first use, for the same reason
	// as game_active below.
	parameter [10:0] h_disp_start = h_front_porch + h_pulse_width + h_back_porch;
	parameter [10:0] h_disp_stop = h_disp_start + h_disp;

	parameter [10:0] v_disp_start = v_front_porch + v_pulse_width + v_back_porch;
	parameter [10:0] v_disp_stop = v_disp_start + v_disp;

	// -- scanout divisors --
	//
	// h_div: pixel clocks per source pixel. v_half: two physical lines
	// per source row. vp_on: the viewport origin applies at all.
	//
	// On composite these are fixed by the parameters above and
	// game_active is not consulted, so yosys folds the whole
	// game_active path out of a composite build -- the viewport is
	// unconditional there. On VGA/DDMI they follow game_active exactly
	// as before.
	// Declared here, before its first use, rather than with the rest
	// of the viewport state below: yosys accepts a use before the
	// declaration, the language (and iverilog, which the benches in
	// rtl/gpu/bench run on) does not.
	reg game_active, wrap_active;

	wire [2:0] h_div = FIXED_VIEWPORT ? H_DIV_BASE :
		(game_active ? 3'd2 : 3'd1);
	wire v_half = FIXED_VIEWPORT ? 1'b0 : game_active;
	wire vp_on  = FIXED_VIEWPORT ? 1'b1 : game_active;

	// Counts 0..h_div-1. Two bits, because h_div is only ever 1, 2 or
	// 4 -- a general divider here would be a multiplier's worth of
	// logic in the one clock domain that has a 640:1 mux in it
	// already.
	reg [1:0] x_phase;

	// flipped once per frame, in the pclk domain, and crossed into
	// the wishbone domain to drive frame_ctr. vblank_p is the raw
	// level; in_vblank is its synchronised copy.
	reg vblank_toggle;

	// -- virtual phosphor modes --
	//
	// These were `ifdef GPU_AMBER / `ifdef GPU_GREEN: chosen at
	// synthesis, changeable only by re-flashing the gateware. They are
	// now a 2-bit register in rtl/socctl.v that software can write at
	// any time. The COLOUR VALUES below are unchanged from those
	// `ifdefs -- only the selection moved.
	//
	// GPU_PAPER is the one genuinely new mode, and it is not a colour
	// at all: it is white-on-black with the pixel sense inverted, so
	// it reuses the white path exactly rather than defining a second
	// white that could drift out of step with the first.
	localparam [1:0] GPU_MODE_WHITE = 2'd0;
	localparam [1:0] GPU_MODE_AMBER = 2'd1;
	localparam [1:0] GPU_MODE_GREEN = 2'd2;
	localparam [1:0] GPU_MODE_PAPER = 2'd3;

	reg [1:0] video_mode_sync0;
	reg [1:0] video_mode_sync1;
	reg [1:0] video_mode_active;
	reg [3:0] bright_sync0, bright_sync1;
	reg [3:0] bright_active;
	reg ddmi_en_sync0, ddmi_en_sync1, ddmi_en_active;
	// The sound half's settings change only when software sets a rate,
	// and are adopted at a frame boundary after two flops, like the
	// rest. A multi-bit value caught mid-change could be torn for one
	// frame -- one ACR packet with a wrong N, which a sink rides out
	// (it averages CTS anyway) -- and is then right from the next.
	reg ddmi_snd_sync0, ddmi_snd_sync1, ddmi_snd_active;
	reg [3:0] ddmi_fs_sync0, ddmi_fs_sync1, ddmi_fs_active;
	reg [19:0] ddmi_n_sync0, ddmi_n_sync1, ddmi_n_active;
	reg [19:0] ddmi_cts_sync0, ddmi_cts_sync1, ddmi_cts_active;

	// XOR, not OR.
	//
	// OR made the cursor always white, which is invisible the moment
	// it crosses anything white -- and wm now inverts the focused
	// window's titlebar, so that is a wide bar the pointer disappears
	// into rather than a rare case.
	//
	// XOR is the classic answer and the reason cursor bitmaps have
	// looked the way they do since the 1980s: over black it reads
	// white exactly as before, over white it reads black, and over a
	// dithered fill it reads as the inverse of whatever it is on.
	// There is no background the pointer can vanish into.
	//
	// Free in gates -- an XOR and an OR are the same one LUT at the
	// same depth -- so this costs nothing in the 25.2MHz pixel domain
	// this line sits in.
	// The word each plane is showing -- see the plane read side at
	// the bottom of this file. Planes 4..7 exist only with GAME_PLANES
	// 6 or 8; in a smaller build nothing loads them, plane_mask below
	// keeps them out of the index, and synthesis removes them.
	reg [31:0] cur0, cur1, cur2, cur3, cur4, cur5, cur6, cur7;

	// How many planes this build fetches and shows: GAME_PLANES with
	// colour, and 4 without -- the plane fetch and read side then run
	// exactly as they always have (planes 1..3 read and dropped).
	localparam integer NP = (COLOR_AVAIL == 0) ? 4 :
		(GAME_PLANES >= 8) ? 8 : (GAME_PLANES >= 6) ? 6 : 4;

	// -- the colour window (WIN_AVAIL; docs/color.md) --
	//
	// In desktop mode, inside one rectangle, the pixel is shown through
	// the palette rather than the phosphor's ink, its index the
	// desktop's bit (plane 0, bit 0) and the second sheet's planes
	// (bits 4..7) -- planes 1..3 are the desktop's other quarters and
	// are masked out. The planes are read exactly as game mode reads
	// them with the 16-colour layout's offsets (plane 1 dx, 2 dy, 3 dx
	// and dy) and the view at (0, 0): plane 4 at the pixel's own
	// position in the second sheet (its row mod 240 with six planes),
	// plane 5 320 columns on, planes 6 and 7 the same 240 rows on, all
	// modulo the sheet. So a window up to 320x240 has a place of its own
	// for each of its pixels in every plane wherever it sits on the
	// desktop, and the fetch and the read side needed no change -- they
	// already run in desktop mode, every line.
	//
	// What it adds: the rectangle, a registered test of it in this
	// domain, the mask, and the output choosing palette or ink per pixel
	// instead of per frame. The palette's read is registered, so while
	// the window is on x leads the raster by a clock, as it does for
	// game-mode colour (x_lead), and the monochrome pixel is delayed by
	// one register to meet it (pixm).
	localparam WIN_ON = (WIN_AVAIL != 0) && (COLOR_AVAIL != 0) &&
		(NP > 4) && (FIXED_VIEWPORT == 0);
	reg win_active;
	reg [9:0] wx0_active, wy0_active, wx1_active, wy1_active;
	wire x_lead;

	// The cursor's pixel, from the read side at the bottom of this
	// file: 1 = invert, the XOR described above, which is all the
	// pointer ever does. `pixel` adds an invert of its own (the
	// benches'). Opaque pixels (ink, background) were measured and
	// left out: in context they cost about 200 LUTs and 3 MHz on this
	// clock more than invert alone, which costs less than the old
	// cursor did (docs/cursor.md, "Cost").
	reg c_d;
	wire c_inv = pixel | c_d;

	wire p0 = cur0[x[4:0]];
	wire pix = p0 ^ c_inv;

	// With x a clock ahead (x_lead), the monochrome pixel is the one
	// worked out a clock ago. Only ever so in desktop mode with the
	// colour window on; in game-mode colour the monochrome path is not
	// shown. The cursor comes out where it always did: it is registered
	// from x too, so both moved by the same clock.
	reg pix_q;
	always @(posedge pclk)
		pix_q <= pix;
	wire pixm = x_lead ? pix_q : pix;

	// is_visible gates the result, and it MUST: in GPU_PAPER the
	// inactive state is 1, so an ungated invert would drive the VGA
	// DAC high right through the front porch, sync pulse and back
	// porch. That is not a cosmetic problem -- a monitor reads sync
	// amplitude to lock, and a "white" blanking interval is how you
	// get a display that reports no signal at all. The other three
	// modes would tolerate the sloppiness; this one does not, which
	// is exactly why the gate belongs here rather than per-mode.
	wire pset = is_visible &&
		((video_mode_active == GPU_MODE_PAPER) ? ~pixm : pixm);

	// -- game-mode colour: index, palette, output --
	//
	// See docs/color.md for the whole design; the short version is
	// that the 640x480x1bpp framebuffer is unchanged, and in game mode
	// up to three more 320x240 regions of it are read as extra
	// BITPLANES of the same picture. Plane 0 is the viewport, exactly
	// as in monochrome game mode, and comes from cur0 like the desktop.
	// Planes 1..3 come from the same line buffer below, one held 32-bit
	// word per plane (cur1..cur3), indexed by the same x[4:0] -- which is
	// valid for every plane because a plane offset of 320 columns is
	// exactly ten words, so all planes cross word boundaries together.
	// With GAME_PLANES 6 or 8, planes 4..7 are the same again in the
	// second VRAM sheet (cur4..cur7), which holds them where the first
	// holds planes 0..3.
	//
	// COLOUR REPLACES THE PHOSPHOR MODE while it is on. The virtual
	// phosphor (white/amber/green/paper) is a choice of ONE ink for a
	// 1bpp picture; with a palette there is no single ink for it to
	// choose, so it is not consulted at all. It is not changed or
	// lost either: video_mode_active keeps tracking socctl's VIDEO
	// register, and the moment colour goes off -- including by
	// leaving game mode, which turns colour off in socctl -- the
	// monochrome path below is selected again in whatever phosphor
	// mode the desktop was in. Monochrome game mode (colour off)
	// keeps honouring the phosphor exactly as it always has.
	reg col_active;
	reg [2:0] np_active;
	reg [5:0] off_active;

	// vp_on rather than game_active: on a composite board the
	// viewport is permanent and game_active is never consulted. Moot
	// today (rtl/sysctl.v builds COLOR_AVAIL as 0 on composite, so
	// this is a constant 0 there), but it is the term composite colour
	// will want, so it is written that way now.
	wire color_on = (COLOR_AVAIL != 0) && col_active && vp_on;

	// Which planes take part: those below np_active + 1 that this build
	// has. Unused planes read as 0, so a 4-colour picture uses palette
	// entries 0..3, a 64-colour one 0..63.
	// In the colour window: the desktop's bit and every plane of the
	// second sheet this build has, whatever np_active says.
	localparam [7:0] WIN_MASK = { (NP > 7), (NP > 6), (NP > 5), (NP > 4),
		4'b0001 };
	wire [7:0] plane_mask = win_active ? WIN_MASK : {
		(NP > 7) && (np_active >= 3'd7), (NP > 6) && (np_active >= 3'd6),
		(NP > 5) && (np_active >= 3'd5), (NP > 4) && (np_active >= 3'd4),
		(np_active >= 3'd3), (np_active >= 3'd2), (np_active >= 3'd1), 1'b1 };

	// The plane offsets in use: the 16-colour layout's in the window.
	localparam [5:0] WIN_OFF = 6'b11_10_01;
	wire [5:0] off_use = win_active ? WIN_OFF : off_active;

	// The cursor inverts the ACTIVE planes only, i.e. index ^ (2^n-1):
	// the same XOR the monochrome path does, generalised. Over entry 0
	// it shows the highest entry in use, and it can never land on an
	// entry the game is not using.
	wire [7:0] cidx = ({ cur7[x[4:0]], cur6[x[4:0]], cur5[x[4:0]], cur4[x[4:0]],
		cur3[x[4:0]], cur2[x[4:0]], cur1[x[4:0]], p0 } ^ {8{c_inv}}) & plane_mask;

	// -- the palette: 2^NP entries in the cursor's block RAM --
	//
	// 16, 64 or 256 x RGB444 (on composite, x Y/U/V), in words
	// 256..511 of cbuf, the block RAM the cursor shapes live in
	// (bottom of this file): the cursor uses 64 of its 512 words, so a
	// palette of any size costs no block of its own, and no LUT RAM --
	// the 16-entry palette used to be distributed RAM and a 16:1 mux
	// in this clock's path. Written in the wishbone domain through
	// cbuf's write port; read here through its registered read port,
	// addressed by cidx. A write landing mid-frame can show as one
	// wrong pixel; write the palette in vblank (software does --
	// docs/color.md) and it cannot.
	//
	// THE READ IS REGISTERED, so the colour of a pixel arrives one
	// pixel clock after its index. On VGA/DDMI that clock is taken back
	// at the other end: while colour is on, x starts one clock early
	// (x_lead, below), so the index runs one clock ahead of the raster
	// and the palette's output lands exactly on it -- blanking, sync
	// and the picture keep the timing they always had, and nothing else
	// moves. In game mode every source pixel lasts two clocks, so
	// starting a clock early is just the first half of the first pixel.
	// In desktop mode with the colour window on, a pixel is one clock,
	// so the monochrome pixel outside the window is taken a clock late
	// to match (pixm, above). On composite the read is the first of the
	// two pipeline stages that path already has.
	//
	// cb_q is cbuf's registered read port, assigned at the bottom of
	// this file and declared here, ahead of its first use. Once a line,
	// in horizontal blanking, it carries the cursor's row instead.
	//
	// The power-on contents are the EGA/CGA default 16, repeated
	// through every entry (entry i is EGA colour i mod 16), so a 64- or
	// 256-colour picture shows as its low four planes until a game
	// sets its own palette. Reset does not reload them; software that
	// cares sets the palette it wants.
	reg [31:0] cb_q;
	wire [11:0] rgb = cb_q[11:0];

`ifdef GPU_COMPOSITE
	localparam PAL_LEAD = 1'b0;
`else
	localparam PAL_LEAD = 1'b1;
`endif
	assign x_lead = PAL_LEAD && (color_on || win_active);

	// Inside the colour window: x against the rectangle, registered on
	// the same edge as the palette's read of the same pixel, so win_px
	// says which of cb_q and the ink is the pixel on screen. The row
	// test changes only at the end of a line.
	reg win_row, win_px;
	wire win_col = (x >= wx0_active) && (x < wx1_active);
	always @(posedge pclk) begin
		win_row <= win_active && (y >= wy0_active) && (y < wy1_active);
		win_px <= win_row && win_col;
	end
	wire pal_out = color_on || win_px;

	// blanking gated here for the same reason pset gates it above
	wire [3:0] c_r = is_visible ? rgb[11:8] : 4'd0;
	wire [3:0] c_g = is_visible ? rgb[7:4]  : 4'd0;
	wire [3:0] c_b = is_visible ? rgb[3:0]  : 4'd0;

	// VGA has ONE bit per channel on every board that has VGA at all,
	// so colour there is the top bit of each channel: at most the
	// eight RGB primaries and secondaries. The EGA default palette
	// degrades to the obvious eight (its 0xa levels read as on, its
	// 0x5 levels as off).
	assign red   = pal_out ? c_r[3] :
		(video_mode_active == GPU_MODE_GREEN) ? 1'b0 : pset;
	assign green = pal_out ? c_g[3] : pset;
	assign blue  = pal_out ? c_b[3] :
		(video_mode_active == GPU_MODE_AMBER ||
		 video_mode_active == GPU_MODE_GREEN) ? 1'b0 : pset;

`ifdef GPU_DDMI

	wire [1:0] out_tmds_red;
	wire [1:0] out_tmds_green;
	wire [1:0] out_tmds_blue;
	wire [1:0] out_tmds_clk;

	// Same three colours the `ifdefs above used to select between,
	// bit-for-bit -- the amber weightings in particular are the
	// original ones and are deliberately not "tidied up" into
	// something rounder. White/green stay at 0x80 rather than 0xff for
	// the same reason: that is what this path has always driven, and
	// changing it would silently alter the look of every existing
	// board while nominally only adding a feature.
	//
	// GPU_PAPER needs no entry of its own: pset is already inverted
	// for it above, so the white arms below produce black glyphs on a
	// 0x80 field, which is precisely the same white, swapped.
	// In colour each 4-bit channel is replicated to 8 bits (0xa ->
	// 0xaa, 0xf -> 0xff), so the palette spans the full range rather
	// than the 0x80 ceiling the monochrome inks keep for
	// compatibility. A game wanting the desktop's white sets 0x888.
	// -- monochrome brightness (VIDEO bits 11:8) --
	//
	// The ink a lit pixel gets, per channel, worked out ONCE per frame
	// from the phosphor mode and the brightness level -- both adopted at
	// the frame boundary -- and registered. The pixel path is then just
	// "ink if pset", as it always was; the arithmetic below is off it
	// entirely, so sixteen levels cost the same as four would.
	//
	// White is W = 16 (L + 1), clamped to 255: L = 7 is 0x80, exactly
	// the ink this output has always used, and is the reset value, so
	// nothing changes until someone asks. Amber keeps today's colour at
	// every level -- R = 0xD2/0x80 W and G = 0x9D/0x80 W, both as
	// shift-adds that give 0xD2 and 0x9D exactly at L = 7 -- clamped at
	// 255, where it drifts towards yellow at the top. Green is (0, W, 0);
	// paper's white is white's.
	//
	// Colour mode does not use these: its palette already spans the
	// full range (docs/color.md).
	// As three 16-entry tables (one LUT4 per output bit) rather than the
	// adders that define them -- generated from exactly those formulas:
	//   W = min(255, 16 (L + 1))
	//   amber R = min(255, W + W/2 + W/8 + W/64)          (0xD2 at L = 7)
	//   amber G = min(255, W + W/8 + W/16 + W/32 + W/128)  (0x9D at L = 7)
	reg [7:0] bw, amber_r8, amber_g8;

	always @(*) begin
		case (bright_active)
			4'd0: bw = 8'h10;
			4'd1: bw = 8'h20;
			4'd2: bw = 8'h30;
			4'd3: bw = 8'h40;
			4'd4: bw = 8'h50;
			4'd5: bw = 8'h60;
			4'd6: bw = 8'h70;
			4'd7: bw = 8'h80;
			4'd8: bw = 8'h90;
			4'd9: bw = 8'hA0;
			4'd10: bw = 8'hB0;
			4'd11: bw = 8'hC0;
			4'd12: bw = 8'hD0;
			4'd13: bw = 8'hE0;
			4'd14: bw = 8'hF0;
			4'd15: bw = 8'hFF;
		endcase
	end

	always @(*) begin
		case (bright_active)
			4'd0: amber_r8 = 8'h1A;
			4'd1: amber_r8 = 8'h34;
			4'd2: amber_r8 = 8'h4E;
			4'd3: amber_r8 = 8'h69;
			4'd4: amber_r8 = 8'h83;
			4'd5: amber_r8 = 8'h9D;
			4'd6: amber_r8 = 8'hB7;
			4'd7: amber_r8 = 8'hD2;
			4'd8: amber_r8 = 8'hEC;
			4'd9: amber_r8 = 8'hFF;
			4'd10: amber_r8 = 8'hFF;
			4'd11: amber_r8 = 8'hFF;
			4'd12: amber_r8 = 8'hFF;
			4'd13: amber_r8 = 8'hFF;
			4'd14: amber_r8 = 8'hFF;
			4'd15: amber_r8 = 8'hFF;
		endcase
	end

	always @(*) begin
		case (bright_active)
			4'd0: amber_g8 = 8'h13;
			4'd1: amber_g8 = 8'h27;
			4'd2: amber_g8 = 8'h3A;
			4'd3: amber_g8 = 8'h4E;
			4'd4: amber_g8 = 8'h61;
			4'd5: amber_g8 = 8'h75;
			4'd6: amber_g8 = 8'h88;
			4'd7: amber_g8 = 8'h9D;
			4'd8: amber_g8 = 8'hB0;
			4'd9: amber_g8 = 8'hC4;
			4'd10: amber_g8 = 8'hD7;
			4'd11: amber_g8 = 8'hEB;
			4'd12: amber_g8 = 8'hFE;
			4'd13: amber_g8 = 8'hFF;
			4'd14: amber_g8 = 8'hFF;
			4'd15: amber_g8 = 8'hFF;
		endcase
	end

	reg [7:0] ddmi_ink_r, ddmi_ink_g, ddmi_ink_b;

	always @(posedge pclk) begin
		ddmi_ink_r <= (video_mode_active == GPU_MODE_AMBER) ? amber_r8 :
			(video_mode_active == GPU_MODE_GREEN) ? 8'd0 : bw;
		ddmi_ink_g <= (video_mode_active == GPU_MODE_AMBER) ? amber_g8 : bw;
		ddmi_ink_b <= (video_mode_active == GPU_MODE_AMBER ||
			video_mode_active == GPU_MODE_GREEN) ? 8'd0 : bw;
	end

	wire [7:0] ddmi_red   = pal_out ? { c_r, c_r } : (pset ? ddmi_ink_r : 8'd0);
	wire [7:0] ddmi_green = pal_out ? { c_g, c_g } : (pset ? ddmi_ink_g : 8'd0);
	wire [7:0] ddmi_blue  = pal_out ? { c_b, c_b } : (pset ? ddmi_ink_b : 8'd0);

	// -- Serialiser output stage --
	//
	// Ten TMDS bits per pixel leave at 2 per bclk. The 10:1 shifting is
	// done in fabric above (see gpu_ddmi.v's DDR_ENABLED), so all this
	// stage has to do is put two bits per clock onto a pin, and bclk is
	// exactly 5 * pclk for that reason.
	//
	// DELIBERATELY NOT OSERDESE2 on 7-series. That primitive would do
	// the full 10:1 itself and free the fabric shifter -- but it is
	// cascaded master/slave for widths above 8, it is the part of
	// nextpnr-xilinx's support that has moved most recently, and none of
	// that buys anything here: the shifter already exists, already
	// works on two other families, and costs a handful of registers. A
	// 2:1 DDR output is the smallest thing that can work and the same
	// shape as what ECP5 does.
`ifdef XC7
	// D1 goes out on the rising edge and D2 on the falling one, which
	// is ODDRX1F's D0 and D1 respectively. SAME_EDGE means both are
	// presented on the rising edge of bclk, so the fabric feeding this
	// is a plain single-edge design.
	wire [3:0] ddr_q;

	ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC"))
		ddr0_clock (.Q(ddr_q[3]), .C(bclk), .CE(1'b1),
			.D1(out_tmds_clk[0]),   .D2(out_tmds_clk[1]),   .R(1'b0), .S(1'b0));
	ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC"))
		ddr0_red   (.Q(ddr_q[2]), .C(bclk), .CE(1'b1),
			.D1(out_tmds_red[0]),   .D2(out_tmds_red[1]),   .R(1'b0), .S(1'b0));
	ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC"))
		ddr0_green (.Q(ddr_q[1]), .C(bclk), .CE(1'b1),
			.D1(out_tmds_green[0]), .D2(out_tmds_green[1]), .R(1'b0), .S(1'b0));
	ODDR #(.DDR_CLK_EDGE("SAME_EDGE"), .INIT(1'b0), .SRTYPE("SYNC"))
		ddr0_blue  (.Q(ddr_q[0]), .C(bclk), .CE(1'b1),
			.D1(out_tmds_blue[0]),  .D2(out_tmds_blue[1]),  .R(1'b0), .S(1'b0));

	// One OBUFDS per lane. The IOSTANDARD (TMDS_33) comes from the XDC;
	// this only says the output is differential.
	OBUFDS obufds_clock (.I(ddr_q[3]), .O(dvi_p[3]), .OB(dvi_n[3]));
	OBUFDS obufds_red   (.I(ddr_q[2]), .O(dvi_p[2]), .OB(dvi_n[2]));
	OBUFDS obufds_green (.I(ddr_q[1]), .O(dvi_p[1]), .OB(dvi_n[1]));
	OBUFDS obufds_blue  (.I(ddr_q[0]), .O(dvi_p[0]), .OB(dvi_n[0]));
`else
	ODDRX1F ddr0_clock (.D0(out_tmds_clk   [0] ), .D1(out_tmds_clk   [1] ), .Q(dvi_p[3]), .SCLK(bclk), .RST(0));
	ODDRX1F ddr0_red   (.D0(out_tmds_red   [0] ), .D1(out_tmds_red   [1] ), .Q(dvi_p[2]), .SCLK(bclk), .RST(0));
	ODDRX1F ddr0_green (.D0(out_tmds_green [0] ), .D1(out_tmds_green [1] ), .Q(dvi_p[1]), .SCLK(bclk), .RST(0));
	ODDRX1F ddr0_blue  (.D0(out_tmds_blue  [0] ), .D1(out_tmds_blue  [1] ), .Q(dvi_p[0]), .SCLK(bclk), .RST(0));
`endif

`ifdef GPU_DDMI_AUDIO
	gpu_ddmi #(.ISLANDS(1)) gpu_ddmi_i
`else
	gpu_ddmi #(.ISLANDS(0)) gpu_ddmi_i
`endif
	(
		.pclk(pclk),
		.tmds_clk(bclk),
		.hc(hc),
		.vc(vc),
		.islands_en(ddmi_en_active),
		.sound_en(ddmi_snd_active),
		.fs_code(ddmi_fs_active),
		.acr_n(ddmi_n_active),
		.acr_cts(ddmi_cts_active),
		.aud_clk(clk),
		.aud_we(ddmi_we),
		.aud_wdata(ddmi_wdata),
		.in_vga_red(ddmi_red),
		.in_vga_green(ddmi_green),
		.in_vga_blue(ddmi_blue),
		.in_vga_blank(!is_visible),
		.in_vga_vsync(vsync),
		.in_vga_hsync(hsync),
		.out_tmds_red(out_tmds_red),
		.out_tmds_green(out_tmds_green),
		.out_tmds_blue(out_tmds_blue),
		.out_tmds_clk(out_tmds_clk)
	);

`endif

`ifdef GPU_COMPOSITE

	// -- monochrome composite video (CVBS) --
	//
	// One resistor ladder on `dac`, one 75R output, one RCA socket.
	// See docs/composite.md for the timing derivation, the ladder
	// values and why this is 320 pixels wide and not 640.
	//
	// Mutually exclusive with GPU_VGA and GPU_DDMI at build time. Not
	// because the pixel pipeline could not feed all three -- it could,
	// they share the line buffer and the refill -- but because the TIMING is
	// different. A 15.7kHz line rate and a 31.5kHz line rate cannot
	// come out of one set of counters, and running two sets means two
	// scanline buffers and an arbiter on vram's single graphics port.
	// That is a real feature; it is not this one.
	//
	// -- levels --
	//
	// A 1Vpp composite signal into 75R has three levels that matter
	// for a monochrome picture:
	//
	//     sync tip   0.000V     the bottom of every sync pulse
	//     blanking   0.300V     everything not sync and not picture
	//     white      1.000V     a set pixel
	//
	// With a 4-bit ladder spanning 0..1V, 0.3V is 4.5 steps. DAC_BLANK
	// is 5 (0.333V) rather than 4 (0.267V) because erring HIGH keeps
	// the sync amplitude at 0.667V rather than 0.733V -- still well
	// inside the +-6% every receiver allows, and the direction that
	// loses picture contrast rather than sync lock. A display that
	// cannot lock shows nothing at all; one with 4% less contrast
	// looks fine.
	//
	// Black and blanking are the same value here, i.e. 0 IRE setup.
	// That is exactly right for PAL and for NTSC-J, and 7.5 IRE low
	// for original NTSC-M -- which shows up as blacks that are very
	// slightly darker than the receiver expects, on a 1bpp display
	// whose "black" is the absence of a pixel anyway. Not worth a
	// fourth level and a per-standard difference.
	localparam [3:0] DAC_SYNC  = 4'd0;
	localparam [3:0] DAC_BLANK = 4'd5;
	localparam [3:0] DAC_WHITE = 4'd15;

	// -- composite sync --
	//
	// Ordinary lines carry the horizontal pulse. During vertical sync
	// the pulse is INVERTED into a broad pulse: sync sits low for the
	// whole line except a short serration at the end.
	//
	// This is the simple version -- no equalizing pulses before and
	// after the vertical block, and no half-line offsets, because
	// there is no interlace to offset. Both are there in a broadcast
	// signal to keep an interlaced receiver's vertical oscillator
	// phased correctly across the half-line difference between fields.
	// This is progressive 240p/288p: every field is identical, there
	// is no half-line, and there is nothing for them to correct. Every
	// consumer TV, capture card and upscaler locks to this; it is what
	// game consoles emitted for twenty years.
	wire in_vsync_lines = (vc >= v_front_porch) &&
		(vc < v_front_porch + v_pulse_width);

	wire h_pulse = (hc >= h_front_porch) &&
		(hc < h_front_porch + h_pulse_width);

	// the serration: sync returns high for one h_pulse_width at the
	// end of each broad-pulse line, which is what keeps the
	// receiver's horizontal oscillator running through the vertical
	// interval instead of free-running for three lines.
	// h_disp_stop, not h_line: the counters wrap at h_disp_stop-1 (see
	// the hc block below), so h_disp_stop IS the line length here and
	// h_line is unused by the timing. Using h_line would put the
	// serration in the wrong place on any board whose two disagree.
	wire broad_pulse = (hc < (h_disp_stop - h_pulse_width));

	wire csync_low = in_vsync_lines ? broad_pulse : h_pulse;

	// The monochrome output, exactly as it has always been: three
	// levels, nothing else. Used as it stands on a build without
	// colour, and while colour is off on one with it.
	wire [3:0] dac_mono = csync_low ? DAC_SYNC :
		(is_visible && pset) ? DAC_WHITE : DAC_BLANK;

	// -- composite COLOUR (docs/composite.md, "Colour") --
	//
	// A colour subcarrier synthesised in the pixel clock domain -- a
	// 32-bit phase accumulator advanced every 25.2MHz clock -- and each
	// sample's DAC code worked out as
	//
	//     blank + Y + U sin(phase) + V cos(phase)
	//
	// from the pixel's palette entry, with a colour burst in the back
	// porch for the receiver to lock its own oscillator to. No new PLL
	// output and no new clock domain: the DAC is still driven from
	// pclk, about seven samples per subcarrier cycle, and the TV's
	// chroma filter is the reconstruction filter.
	//
	// The subcarrier is free-running, not locked to the line: a
	// receiver locks to the burst on each line, so what matters is
	// that burst and picture come from the same oscillator, and they
	// do. For the same reason the phase can be coarse -- 32 steps,
	// 11.25 degrees -- because any systematic offset is in the burst as
	// well and cancels.
	//
	// THE DAC IS 4 BITS: one code is 10 IRE. Burst is +-20 IRE, i.e.
	// +-2 codes, and saturated colours swing about +-6. That is coarse
	// -- think Apple II or Atari 8-bit composite, not broadcast -- but
	// it is colour, from four resistors. Codes are clamped to 2..15:
	// never down at sync level (0) where a receiver would see a pulse,
	// and never past the ladder's top.
	//
	// Y, U and V for each palette entry are worked out when the entry
	// is WRITTEN (wishbone domain, a handful of shift-adds, rare), and
	// kept in the palette instead of its RGB. The pixel path
	// then needs only two small multiplies, against a 32-entry sine
	// table.
	//
	// Scales, all in sixteenths of a DAC code:
	//   Y = (0.299R + 0.587G + 0.114B) * 160/15 ~ (51R + 100G + 19B) / 16
	//   U = 0.492 (B*160/15 - Y)               ~ (84B - 8Y) / 16
	//   V = 0.877 (R*160/15 - Y)               ~ (150R - 14Y) / 16
	// R, G, B the palette's 0..15 channels, so white is Y = 159, i.e.
	// 10 codes above blank: code 15, the same white the monochrome
	// output has always used.
	//
	// NTSC encodes U and V directly (the same signal as I and Q, rotated
	// 33 degrees), burst at 180 degrees. PAL inverts V on alternate
	// lines -- the "PAL switch" -- with the burst at 135 and 225
	// degrees to tell the receiver which; that is a sign flip and a
	// different burst offset, nothing more.
	//
	// Colour shows only while game-mode colour is on (color_on), and
	// only then is there a burst. With no burst a receiver's colour
	// killer turns its decoder off, so the desktop and monochrome game
	// mode stay a pure black-and-white signal, three levels, exactly
	// as before. cvbs_mono (socctl's CVBS register) turns the chroma
	// and burst off while leaving colour on: the palette's BRIGHTNESS,
	// in greys, for a receiver or a cable that does not cope.
`ifdef GPU_COMPOSITE_PAL
	localparam [31:0] SC_INC    = 32'd755644743;   // 4433618.75Hz / 25.2MHz * 2^32
	localparam [10:0] BURST_DLY = 11'd141;         // 5.6us after the sync edge
	localparam [10:0] BURST_LEN = 11'd57;          // 10 cycles
	localparam IS_PAL = 1'b1;
`else
	localparam [31:0] SC_INC    = 32'd610080582;   // 315/88MHz / 25.2MHz * 2^32
	localparam [10:0] BURST_DLY = 11'd134;         // 5.3us after the sync edge
	localparam [10:0] BURST_LEN = 11'd63;          // 9 cycles
	localparam IS_PAL = 1'b0;
`endif

	reg [31:0] sc_acc;
	reg pal_alt;                    // PAL: this line's V is inverted
	reg mono_sync0, mono_sync1, mono_active;

	wire burst_win = !in_vsync_lines &&
		(hc >= h_front_porch + BURST_DLY) &&
		(hc < h_front_porch + BURST_DLY + BURST_LEN);

	// round(16 sin(2 pi k / 32))
	reg signed [5:0] sin_tab [0:31];

	initial begin
		sin_tab[0] = 6'sd0;
		sin_tab[1] = 6'sd3;
		sin_tab[2] = 6'sd6;
		sin_tab[3] = 6'sd9;
		sin_tab[4] = 6'sd11;
		sin_tab[5] = 6'sd13;
		sin_tab[6] = 6'sd15;
		sin_tab[7] = 6'sd16;
		sin_tab[8] = 6'sd16;
		sin_tab[9] = 6'sd16;
		sin_tab[10] = 6'sd15;
		sin_tab[11] = 6'sd13;
		sin_tab[12] = 6'sd11;
		sin_tab[13] = 6'sd9;
		sin_tab[14] = 6'sd6;
		sin_tab[15] = 6'sd3;
		sin_tab[16] = 6'sd0;
		sin_tab[17] = -6'sd3;
		sin_tab[18] = -6'sd6;
		sin_tab[19] = -6'sd9;
		sin_tab[20] = -6'sd11;
		sin_tab[21] = -6'sd13;
		sin_tab[22] = -6'sd15;
		sin_tab[23] = -6'sd16;
		sin_tab[24] = -6'sd16;
		sin_tab[25] = -6'sd16;
		sin_tab[26] = -6'sd15;
		sin_tab[27] = -6'sd13;
		sin_tab[28] = -6'sd11;
		sin_tab[29] = -6'sd9;
		sin_tab[30] = -6'sd6;
		sin_tab[31] = -6'sd3;
	end

	// RGB444 -> Y/U/V at write time. Shift-adds, not `*`: a constant
	// multiply is something yosys may hand to a DSP block, and this is
	// a write path that runs a few times a frame at most.
	wire signed [13:0] cr = { 10'd0, pal_rgb[11:8] };
	wire signed [13:0] cg = { 10'd0, pal_rgb[7:4] };
	wire signed [13:0] cb = { 10'd0, pal_rgb[3:0] };
	wire signed [13:0] cy_sum =
		(cr <<< 5) + (cr <<< 4) + (cr <<< 1) + cr +       // 51 R
		(cg <<< 6) + (cg <<< 5) + (cg <<< 2) +            // 100 G
		(cb <<< 4) + (cb <<< 1) + cb;                     // 19 B
	wire signed [13:0] cy = cy_sum >>> 4;
	wire signed [13:0] cu_sum = (cb <<< 6) + (cb <<< 4) + (cb <<< 2) - (cy <<< 3);
	wire signed [13:0] cv_sum = (cr <<< 7) + (cr <<< 4) + (cr <<< 2) + (cr <<< 1) -
		(cy <<< 4) + (cy <<< 1);
	wire signed [13:0] cu = cu_sum >>> 4;
	wire signed [13:0] cv = cv_sum >>> 4;

	// What a PALETTE write puts in cbuf on a composite build: the
	// entry's Y/U/V, not its RGB (composite has no other use for it).
	wire [31:0] pal_wword = { 6'd0, cy[7:0], cu[8:0], cv[8:0] };

	// -- the pipeline: two registers, the same for every signal --
	//
	// Stage 1 looks the pixel's Y/U/V up and samples the phase and
	// every timing flag; stage 2 works out the code and registers it
	// onto the pins. Sync, blanking, burst and picture all go through
	// both stages, so the whole signal is two pixel clocks (80ns) late
	// and nothing moves relative to anything else.
	// Stage 1's Y/U/V is the palette read itself: cbuf's registered
	// read port, addressed by cidx on the same edge as the rest of
	// stage 1 is registered.
	wire [7:0] s1_y = cb_q[25:18];
	wire signed [8:0] s1_u = cb_q[17:9];
	wire signed [8:0] s1_v = cb_q[8:0];
	reg [4:0] s1_p;
	reg s1_vis, s1_sync, s1_burst, s1_pset, s1_alt;
	reg [3:0] dac_r;

	wire luma_on = color_on;
	wire chroma_on = color_on && !mono_active;

	wire signed [5:0] s_sin = sin_tab[s1_p];
	wire signed [5:0] s_cos = sin_tab[s1_p + 5'd8];
	wire signed [8:0] s_v = (IS_PAL && s1_alt) ? -s1_v : s1_v;
	wire signed [15:0] chroma_n = (s1_u * s_sin) + (s_v * s_cos);
	wire signed [12:0] chroma = chroma_n >>> 4;
	wire signed [13:0] val = 14'sd88 + $signed({ 6'd0, s1_y }) +
		(chroma_on ? chroma : 13'sd0);              // 88: blank 80, +8 to round
	wire signed [9:0] q = val >>> 4;
	wire [3:0] code = (q < 10'sd2) ? 4'd2 : (q > 10'sd15) ? 4'd15 : q[3:0];

	// burst: +-2 codes around blank, at 180 degrees (NTSC) or 135/225
	// (PAL, alternating with the V switch)
	wire [4:0] b_off = !IS_PAL ? 5'd16 : (s1_alt ? 5'd20 : 5'd12);
	wire signed [5:0] b_sin = sin_tab[s1_p + b_off];
	wire signed [9:0] b_val = 10'sd88 + (b_sin <<< 1);
	wire [3:0] b_code = b_val[7:4];

	always @(posedge pclk) begin
		s1_p <= sc_acc[31:27];
		s1_vis <= is_visible;
		s1_sync <= csync_low;
		s1_burst <= burst_win;
		s1_pset <= pset;
		s1_alt <= pal_alt;

		dac_r <= s1_sync ? DAC_SYNC :
			s1_vis ? (luma_on ? code : (s1_pset ? DAC_WHITE : DAC_BLANK)) :
			(s1_burst && chroma_on) ? b_code : DAC_BLANK;

		mono_sync0 <= cvbs_mono;
		mono_sync1 <= mono_sync0;

		if (!resetn) begin
			sc_acc <= 32'd0;
			pal_alt <= 1'b0;
			mono_active <= 1'b0;
		end else begin
			sc_acc <= sc_acc + SC_INC;
			if (hc == h_disp_stop - 1) pal_alt <= ~pal_alt;
			if (hc == h_disp_stop - 1 && vc == v_disp_stop - 1)
				mono_active <= mono_sync1;
		end
	end

	// Without colour the monochrome path is the output, untouched, and
	// yosys removes everything above.
	assign dac = (COLOR_AVAIL != 0) ? dac_r : dac_mono;

`else

	// No composite output on this board -- tie the ladder off rather
	// than leaving it floating. Costs nothing; a board without the
	// pins never routes it anywhere.
	assign dac = 4'd0;

	// What a PALETTE write puts in cbuf: the entry's RGB444.
	wire [31:0] pal_wword = { 20'd0, pal_rgb };

`endif

	// video timing: h_disp_start and friends are at the top of the
	// module body, ahead of their first use.

	assign is_visible = (hc >= h_disp_start && vc >= v_disp_start &&
		hc < h_disp_stop && vc < v_disp_stop);

	// VERTICAL blanking only -- deliberately not !is_visible, which is
	// also true during every horizontal blanking interval and would
	// therefore be asserted for a few microseconds of every line. The
	// question software is asking through socctl.v's FRAME register is
	// "is it safe to redraw", and the answer is only yes between
	// frames.
	//
	// One term, not two. The obvious way to write this is
	// `vc < v_disp_start || vc >= v_disp_stop`, but the second half is
	// dead code here: vc wraps to 0 on the cycle it reaches
	// v_disp_stop - 1, so it never reaches v_disp_stop at all. The
	// blanking interval lives at the START of this counter's range
	// (vc 0..44 is the front porch, sync pulse and back porch), not
	// at the end. Written out in full it would look correct, would
	// synthesise to the same thing after constant propagation, and
	// would quietly mislead the next person to read it.
	wire vblank_p = (vc < v_disp_start);

	assign hsync = (hc < h_front_porch) ||
		(hc >= h_front_porch + h_pulse_width);
	assign vsync = (vc < v_front_porch) ||
		(vc >= v_front_porch + v_pulse_width);

	// video_mode crosses from the wishbone clock into pclk. Two flops
	// handle metastability, but two flops alone are NOT enough for a
	// multi-bit value: the two bits can resolve on different cycles,
	// so a write of 01 -> 10 can be observed as 00 or 11 in between.
	// For one pixel clock that would be invisible; the reason to care
	// is that a mid-frame change means the top of the screen is drawn
	// in one mode and the bottom in another, which on a mode that
	// inverts (GPU_PAPER) is a very visible tear.
	//
	// So the synchronised value is only ADOPTED at the end of a frame,
	// on the same cycle the counters wrap. Every frame is therefore
	// drawn entirely in one mode, and a mode change takes effect at
	// the next frame boundary -- at most 16.7ms after the store
	// retires, which is below the threshold at which a person could
	// tell it from instant.
	//
	// Resets to white rather than to video_mode, which means a board
	// whose default is amber or green shows white for at most one
	// frame at power-on. Sampling the input directly at reset would
	// avoid that, but it is the one read that genuinely cannot be
	// synchronised (there is no clock yet), and a single frame during
	// a window when the monitor has not locked anyway is not worth an
	// unsynchronised cross-domain read.
	// -- game mode configuration: capture, then adopt --
	//
	// Two stages, and they solve two different problems.
	//
	// CAPTURE (view_load -> *_cap) is the clock domain crossing.
	// socctl.v flips view_load on the same wb_clk edge that updates
	// the data, so the data is guaranteed stable by the time the
	// toggle's edge is visible here. Three synchroniser flops rather
	// than the usual two, with the edge detected between the second
	// and third: that puts the capture a full pixel clock later than
	// the minimum, so the payload has been stable for at least two
	// pclk (~80ns, ~4 wb_clk) before it is sampled. Two flops would
	// very probably be fine; the extra one costs three LUTs and
	// removes the need to reason about how close together two stores
	// to these registers can possibly land.
	//
	// ADOPT (*_cap -> *_active) is the tearing fix, and it is the
	// same trick, for the same reason, as video_mode_active above:
	// the captured value is only taken up on the cycle the counters
	// wrap, so an entire frame is always drawn with one viewport.
	// Without it a mid-frame origin change would show the top of the
	// screen from one position and the bottom from another -- which
	// is exactly the tear a page flip exists to avoid.
	reg view_sync0, view_sync1, view_sync2;
	wire view_edge = view_sync2 ^ view_sync1;

	reg game_cap, wrap_cap;
	reg [9:0] vx_cap, vy_cap;

	reg [9:0] vx_active, vy_active;

	// colour rides the same capture and the same adoption -- see the
	// color_en port. *_active are declared with the pixel path above.
	reg col_cap;
	reg [2:0] np_cap;
	reg [5:0] off_cap;
	reg win_cap;
	reg [9:0] wx0_cap, wy0_cap, wx1_cap, wy1_cap;

	// The clamp that keeps the viewport on screen, applied once per
	// frame at adoption rather than continuously. Deliberately NOT in
	// socctl.v: it depends on the wrap bit, so doing it on the write
	// path would make the stored origin depend on the order the two
	// registers were written in. Here there is one rule in one place
	// -- whatever is adopted is what gets scanned, and it is always
	// in range.
	//
	// 320 and 240 (not 319/239) are correct: the viewport is 320
	// wide, so an origin of exactly 320 puts its right edge on column
	// 639, the last one there is.
	//
	// In wrap mode there is nothing to clamp -- going off the edge is
	// the feature -- and socctl.v has already limited the origin to
	// 0..639/0..479, which is what bounds the arithmetic below.
	wire vp_cap = FIXED_VIEWPORT ? 1'b1 : game_cap;

	wire [9:0] vx_adopt = (!vp_cap) ? 10'd0 :
		(!wrap_cap && vx_cap > 10'd320) ? 10'd320 : vx_cap;
	wire [9:0] vy_adopt = (!vp_cap) ? 10'd0 :
		(!wrap_cap && vy_cap > 10'd240) ? 10'd240 : vy_cap;

	always @(posedge pclk) begin
		video_mode_sync0 <= video_mode;
		video_mode_sync1 <= video_mode_sync0;
		bright_sync0 <= video_bright;
		bright_sync1 <= bright_sync0;
		ddmi_en_sync0 <= ddmi_en;
		ddmi_en_sync1 <= ddmi_en_sync0;
		ddmi_snd_sync0 <= ddmi_sound;
		ddmi_snd_sync1 <= ddmi_snd_sync0;
		ddmi_fs_sync0 <= ddmi_fs;
		ddmi_fs_sync1 <= ddmi_fs_sync0;
		ddmi_n_sync0 <= ddmi_acr_n;
		ddmi_n_sync1 <= ddmi_n_sync0;
		ddmi_cts_sync0 <= ddmi_acr_cts;
		ddmi_cts_sync1 <= ddmi_cts_sync0;

		view_sync0 <= view_load;
		view_sync1 <= view_sync0;
		view_sync2 <= view_sync1;

		if (!resetn) begin
			video_mode_active <= GPU_MODE_WHITE;
			bright_active <= 4'd7;
			ddmi_en_active <= 1'b0;
			ddmi_snd_active <= 1'b0;
			ddmi_fs_active <= 4'd0;
			ddmi_n_active <= 20'd0;
			ddmi_cts_active <= 20'd0;
			view_sync0 <= 1'b0;
			view_sync1 <= 1'b0;
			view_sync2 <= 1'b0;
			game_cap <= 1'b0;
			wrap_cap <= 1'b0;
			vx_cap <= 10'd0;
			vy_cap <= 10'd0;
			game_active <= 1'b0;
			wrap_active <= 1'b0;
			vx_active <= 10'd0;
			vy_active <= 10'd0;
			col_cap <= 1'b0;
			np_cap <= 3'd0;
			off_cap <= 6'd0;
			col_active <= 1'b0;
			np_active <= 3'd0;
			off_active <= 6'd0;
			win_cap <= 1'b0;
			win_active <= 1'b0;
		end else begin
			if (view_edge) begin
				game_cap <= game_en;
				wrap_cap <= game_wrap;
				vx_cap <= view_x;
				vy_cap <= view_y;
				col_cap <= color_en;
				np_cap <= color_np;
				off_cap <= color_off;
				win_cap <= win_en;
				wx0_cap <= win_x0;
				wy0_cap <= win_y0;
				wx1_cap <= win_x1;
				wy1_cap <= win_y1;
			end
			if (hc == h_disp_stop - 1 && vc == v_disp_stop - 1) begin
				video_mode_active <= video_mode_sync1;
				bright_active <= bright_sync1;
				ddmi_en_active <= ddmi_en_sync1;
				ddmi_snd_active <= ddmi_snd_sync1;
				ddmi_fs_active <= ddmi_fs_sync1;
				ddmi_n_active <= ddmi_n_sync1;
				ddmi_cts_active <= ddmi_cts_sync1;
				game_active <= game_cap;
				wrap_active <= wrap_cap;
				vx_active <= vx_adopt;
				vy_active <= vy_adopt;
				col_active <= col_cap && (COLOR_AVAIL != 0);
				np_active <= np_cap;
				off_active <= off_cap;
				// the window only in desktop mode: game mode wins
				win_active <= WIN_ON && win_cap && !vp_cap;
				wx0_active <= wx0_cap;
				wy0_active <= wy0_cap;
				wx1_active <= wx1_cap;
				wy1_active <= wy1_cap;
			end
		end
	end

	reg refill;
	reg refill_toggle;
	reg [1:0] refill_sync;
	wire refill_synced = refill_sync[1] ^ refill_sync[0];

	// row_base (below) used to read `y` directly, combinationally --
	// but y lives in the pclk domain and row_base is computed inside
	// this clk-domain always block, a genuinely different clock. That
	// unsynchronized cross-domain read is a real CDC hazard: a torn
	// read of y mid-transition produces a wrong row address. y_refill
	// (set alongside refill_toggle itself, same pclk edge, same
	// condition, so it's guaranteed to hold the correct upcoming row
	// by the time refill_toggle flips) is synchronized into the clk
	// domain here with the exact same 2-flop-then-use-once-stable
	// timing already trusted for refill_toggle -> refill_sync ->
	// refill above -- by the time refill fires, y_refill_sync1 has
	// already been stable for the same margin refill_toggle's own
	// crossing relies on, and stays stable for the whole scanline
	// (~840 pclk cycles) after that, so no extra latching is needed.
	reg [9:0] y_refill;
	reg [9:0] y_refill_sync0, y_refill_sync1;

	// -- frame counter and vblank, pclk -> clk --
	//
	// Same one-bit-toggle crossing as refill_toggle directly above,
	// and chosen over synchronising a counter for the same reason
	// socctl.v's view_load is a toggle: a 16-bit value crossing on
	// two flops can be sampled torn, and a torn frame number handed
	// to a game waiting for vsync is a hang or a dropped frame rather
	// than a cosmetic glitch. Crossing one bit and doing the counting
	// on THIS side removes the question entirely -- there is no
	// multi-bit crossing left to get wrong.
	//
	// 16 bits wraps every ~18 minutes at 60Hz. Software compares for
	// inequality (and unsigned-subtracts for elapsed frames), so the
	// wrap is not a special case; it is wide enough that a naive
	// "wait until ctr > target" would also almost always work, which
	// is worth having when somebody writes that by accident.
	//
	// in_vblank is a single bit whose only consumer is a status read,
	// so two flops is genuinely all it needs.
	reg vblank_sync0, vblank_sync1;
	reg [1:0] vbt_sync;
	wire vbt_edge = vbt_sync[1] ^ vbt_sync[0];

	always @(posedge clk) begin
		refill <= refill_synced;
		y_refill_sync0 <= y_refill;
		y_refill_sync1 <= y_refill_sync0;
		vblank_sync0 <= vblank_p;
		vblank_sync1 <= vblank_sync0;
		in_vblank <= vblank_sync1;
		if (!resetn) begin
			refill_sync <= 0;
			vbt_sync <= 0;
			frame_ctr <= 16'd0;
			in_vblank <= 1'b0;
		end else begin
			refill_sync <= {refill_sync[0], refill_toggle}; 
			vbt_sync <= {vbt_sync[0], vblank_toggle};
			if (vbt_edge) frame_ctr <= frame_ctr + 16'd1;
		end
	end

	// -- vertical: which framebuffer row this physical row shows --
	//
	// scan_row is the physical row, 0..479. fb_row is the framebuffer
	// row it maps to.
	//
	// scan_row is computed from vc + 1, NOT from vc, and that is the
	// vertical half of the off-by-one described in this file's header.
	// y is assigned at the END of a line and therefore holds that
	// value for the line AFTER the one vc currently names -- so the
	// old `vc - v_disp_start` displayed row 0 on the first two visible
	// lines and never displayed row 479 at all, exactly mirroring what
	// the horizontal path did. The whole picture was one row low and
	// one column right, which on a desktop is invisible and under 2x
	// doubling is a three-pixel seam on two edges.
	//
	// In desktop mode they are the same and yosys will prune the rest
	// of this away entirely on a board without `GAME (game_active is
	// then a constant 0 all the way back to socctl's GAME_AVAIL
	// parameter).
	//
	// In game mode the physical row is halved -- so each framebuffer
	// row is shown on two consecutive physical rows -- and offset by
	// the viewport origin.
	//
	// The wrap needs one conditional subtract and no more, and it is
	// worth showing why rather than trusting it. In wrap mode
	// socctl.v has limited vy_active to 479 and the halved row is at
	// most 239, so the sum is at most 718: strictly less than 960, so
	// subtracting 480 once always lands back in range. In clamp mode
	// vy_active is at most 240 and the sum is at most 479, so the
	// subtract never fires at all.
	//
	// All of this is combinational into a register in the 25.2MHz
	// domain -- an 11-bit add, a compare and a subtract, three or so
	// LUT levels against a 39.7ns period. The 48MHz side is untouched
	// and still just does row*20 into gb_adr_o.
	wire [10:0] vc_next = vc + 11'd1;
	wire [9:0] scan_row =
		(vc_next >= v_disp_start && vc_next < v_disp_stop) ?
			(vc_next - v_disp_start) : 10'd0;
	wire [9:0] half_row = v_half ? { 1'b0, scan_row[9:1] } : scan_row;
	wire [10:0] row_sum = vp_on ?
		({ 1'b0, vy_active } + { 1'b0, half_row }) : { 1'b0, scan_row };
	wire [9:0] fb_row =
		(row_sum >= 11'd480) ? (row_sum - 11'd480) : row_sum[9:0];

	// -- colour: the "other half" row, for planes offset by 240 --
	//
	// A plane with dy set is read 240 rows further down the torus than
	// plane 0, i.e. from (fb_row + 240) mod 480. Every plane's row is
	// therefore one of only two values, fb_row or this one, so only
	// this one has to be computed and carried across to the fetch.
	//
	// It goes across ALREADY MULTIPLIED by 20 (a VRAM word address).
	// That arithmetic is done here, in the 25.2MHz domain that has the
	// slack, rather than on the 48MHz side, which does not; the fetch
	// below only adds a word index to it, exactly as the row refill
	// adds one to row_base. It is assigned on the same edge, under the
	// same condition, as y_refill and refill_toggle, so it crosses with
	// exactly the margin y_refill's crossing already relies on.
	wire [10:0] alt_sum = { 1'b0, fb_row } + 11'd240;
	wire [9:0] fb_alt =
		(alt_sum >= 11'd480) ? (alt_sum - 11'd480) : alt_sum[9:0];
	wire [14:0] alt_base_next = ({ 5'd0, fb_alt } << 4) + ({ 5'd0, fb_alt } << 2);
	reg [14:0] alt_refill;

	// -- colour: the second sheet's two row bases (GAME_PLANES 6/8) --
	//
	// Planes 4..7 sit in the second sheet, VRAM words 9600 up, where
	// planes 0..3 sit in the first: plane 4+j on the same row as plane
	// j, 480 rows further on. With 6 planes the second sheet is only
	// 240 rows high and its row is taken mod 240 -- the same row for a
	// plane with dy and one without, so a 6-plane layout keeps planes
	// 0 and 1 apart by dx (docs/color.md).
	//
	// Worked out from y_refill and alt_refill, the registers above,
	// every pixel clock: those change only at the end of a line, so
	// these follow one clock later and are then stable for the whole
	// line, and the fetch does not sample them until the row refill is
	// over (21 wishbone clocks after the crossing) -- the same margin
	// alt_refill's own crossing has, less one pixel clock. The adds
	// stay in this domain, as alt_refill's do.
	wire [9:0] s1_row = ((NP == 6) && (y_refill >= 10'd240)) ?
		(y_refill - 10'd240) : y_refill;
	wire [14:0] s1r_next = ({ 5'd0, s1_row } << 4) + ({ 5'd0, s1_row } << 2) +
		15'd9600;
	wire [14:0] s1a_next = (NP == 6) ? s1r_next : (alt_refill + 15'd9600);
	reg [14:0] s1r_refill, s1a_refill;

	always @(posedge pclk) begin
		s1r_refill <= s1r_next;
		s1a_refill <= s1a_next;
	end

	// -- horizontal: the loadable pixel index --
	//
	// x is the framebuffer COLUMN: x[9:5] the word, x[4:0] the bit in
	// cur0. See this file's header for why this is a counter and not
	// `vx + (hc >> 1)`.
	//
	// The wrap is a comparator against 639 on a value that is about
	// to be incremented anyway. It can only fire in game mode with
	// wrap on: in clamp mode the origin is at most 320 and 320 pixel
	// steps take x to at most 639, and in desktop mode the origin is
	// 0 and 640 steps take it to 639.
	//
	// x_phase is the doubling phase. Cleared with the load below, so
	// every line starts on the same phase and the two halves of a
	// doubled pixel never straddle a line boundary.
	wire [9:0] x_next =
		(vp_on && wrap_active && x == 10'd639) ? 10'd0 : (x + 10'd1);

	// true on the last clock of a source pixel
	wire x_step = (x_phase == (h_div - 3'd1));

	// x runs from the first visible clock -- or, while colour is on
	// (VGA/DDMI), from the clock before it, so the palette's registered
	// read lands on the raster: see x_lead with the palette above.
	wire x_run = (hc >= h_disp_start) ||
		(x_lead && (hc == h_disp_start - 11'd1));

	always @(posedge pclk) begin

		// The pixel index runs on its own, outside the hc/vc chain
		// below, because that chain deliberately does nothing on the
		// last cycle of a line (it is busy wrapping) and x has to
		// keep advancing through it to reach column 639.
		//
		// Load while hc < h_disp_start, so the assignment made on the
		// last blanking cycle is the one x holds on the first visible
		// cycle. That is the off-by-one fix described in this file's
		// header: the origin is now shown on the first visible pixel
		// rather than on the first two.
		if (!resetn) begin
			x <= 10'd0;
			x_phase <= 2'd0;
		end else if (x_run) begin
			if (x_step) begin
				x_phase <= 2'd0;
				x <= x_next;
			end else begin
				x_phase <= x_phase + 2'd1;
			end
		end else begin
			x <= vx_active;
			x_phase <= 2'd0;
		end

		if (!resetn) begin
			hc <= 0;
			vc <= 0;
			refill_toggle <= 0;
			vblank_toggle <= 0;
			y <= 0;
			y_refill <= 0;
			alt_refill <= 15'd0;
		end else if (hc == h_disp_stop - 1) begin
			refill_toggle <= ~refill_toggle;
			hc <= 0;
			if (vc == v_disp_stop - 1) begin
				vc <= 0;
				// end of the last visible line: one frame has been
				// scanned out. Same edge the viewport and the colour
				// mode are adopted on, which is not a coincidence --
				// a game that waits for this counter to change and
				// then flips knows the flip lands on the next
				// boundary, a whole frame away.
				vblank_toggle <= ~vblank_toggle;
			end else begin
				vc <= vc + 1;
				y <= fb_row;
				y_refill <= fb_row;
				alt_refill <= alt_base_next;
			end
		end else begin
			hc <= hc + 1;
		end

	end

	reg [5:0] refill_words;

	// one scanline's worth of pixels, refilled from VRAM once per
	// physical row (native 1:1 now -- no more re-reading the same
	// VRAM row across two physical rows the way GPU_PIXEL_DOUBLE did).
	// 640 pixels = 20 words; row stride in gb_adr_o below is
	// therefore *20, not a power-of-2 shift (unlike the old 1024-wide
	// non-doubled path's y<<5, which happened to work only because
	// 1024/32=32 is itself a power of 2) -- computed as (y<<4)+(y<<2)
	// to avoid inferring an actual multiplier for what's just y*20.
	// Uses y_refill_sync1 (see above), not y directly -- the CDC fix.
	//
	// The row goes into the line buffer (pbuf, below) at words 0..19,
	// row 0 -- plane p is row p -- not into a 640-bit register: see
	// this file's header.
	wire [14:0] row_base = (y_refill_sync1 << 4) + (y_refill_sync1 << 2);   // y*20

	// -- colour: the plane fetch --
	//
	// After the row's 20 words, NP-1 more rows of 20 -- planes 1..3,
	// and 4..7 with GAME_PLANES 6 or 8, for the SAME physical line -- go
	// into the line buffer too. Same port, same two-cycle latency, same
	// "issue now, store two edges later" shape as the row refill
	// directly above it, and the same timing: fetched in the horizontal
	// blanking interval of the line before, so nothing ever reads a
	// word that is being written. 60 words at 48MHz is 1.25us; with the
	// row refill and the crossing it is done about 45 pixel clocks into
	// a 160-clock blanking interval. Eight planes, 140 words, take about
	// 90, and the read side does not touch the buffer until 24 clocks
	// before the line starts (136).
	//
	// Every plane is always fetched in full, whatever color_np says:
	// the VRAM port is dedicated to scanout and otherwise idle in
	// blanking, so skipping words would buy nothing and cost a
	// comparison in the one clock domain that has no slack. dx is not
	// applied here either -- whole rows come in, in natural order, and
	// the read side picks the word. That keeps this side to a mux and
	// an adder per word, the same depth as the row address above.
	//
	// The per-plane dy bits cross from the pixel domain on two flops.
	// They are frame-constant (adopted at the end of the last visible
	// line), so the only fetch that can sample them changing is the one
	// for the first blanking line of the next frame, which nothing
	// displays.
	//
	// The buffer itself is up to 8 x 20 words, addressed { row, word }:
	// row p is plane p, row 0 the framebuffer row. 256 words of a
	// 512-word block: one DP16KD on ECP5, one RAMB18 on 7-series,
	// colour or not, at any plane count. Written here, in the wishbone
	// domain; read in the pixel domain below.
	(* ram_style = "block" *)
	reg [31:0] pbuf [0:255];

	// The hardware cursor's shapes (docs/cursor.md): a block RAM of
	// its own, one 32-bit word a row (bits 15:0, bit c = column c, 1 =
	// invert; bits 31:16 are not used yet), written by
	// socctl in this (wishbone) domain and read in the pixel domain
	// below. Slots of 16 rows: 0 and 1 are today's X and Z from
	// configuration on -- the pointer shows before any software runs
	// -- and 2 and 3 a 9x9 X and Z until wm loads the user's. Its own
	// block rather than the spare words of pbuf above (sharing pbuf
	// would need its write port for the CPU too, a 32-bit mux in front
	// of it, which measured 15 MHz off the wishbone clock, or its read
	// port to write, true dual port, which is two blocks for a 32-bit
	// width). Words 0..63 are the four slots; words 256..511 are the
	// game-mode palette (with the pixel path, above): one block for
	// both, written through one port by socctl, read through the other
	// here -- by the cursor for two clocks a line in horizontal
	// blanking, by the palette at every other clock.
	// The palette's power-on entries: EGA's sixteen (z_color_palette_ega
	// in sw/common/zcolor.c, KEEP IN SYNC), as RGB444 -- or, on a
	// composite build, as the Y/U/V words a write of that RGB stores.
	function [31:0] pal_init;
		input [3:0] i;
		begin
			case (i)
`ifdef GPU_COMPOSITE
				4'd0: pal_init = 32'h00000000;	// Y 0, U 0, V 0
				4'd1: pal_init = 32'h002c5ff6;	// Y 11, U 47, V -10
				4'd2: pal_init = 32'h00fbc3c9;	// Y 62, U -31, V -55
				4'd3: pal_init = 32'h01281fbf;	// Y 74, U 15, V -65
				4'd4: pal_init = 32'h007fe042;	// Y 31, U -16, V 66
				4'd5: pal_init = 32'h00ac3e38;	// Y 43, U 31, V 56
				4'd6: pal_init = 32'h00ffc026;	// Y 63, U -32, V 38
				4'd7: pal_init = 32'h01abfe01;	// Y 106, U -1, V 1
				4'd8: pal_init = 32'h00d7fe00;	// Y 53, U -1, V 0
				4'd9: pal_init = 32'h01045df6;	// Y 65, U 46, V -10
				4'd10: pal_init = 32'h01cfc1ca;	// Y 115, U -32, V -54
				4'd11: pal_init = 32'h01fc1fbf;	// Y 127, U 15, V -65
				4'd12: pal_init = 32'h0157de42;	// Y 85, U -17, V 66
				4'd13: pal_init = 32'h01803c38;	// Y 96, U 30, V 56
				4'd14: pal_init = 32'h024fa00c;	// Y 147, U -48, V 12
				4'd15: pal_init = 32'h027ffe01;	// Y 159, U -1, V 1
`else
				4'd0: pal_init = 32'h00000000;
				4'd1: pal_init = 32'h0000000a;
				4'd2: pal_init = 32'h000000a0;
				4'd3: pal_init = 32'h000000aa;
				4'd4: pal_init = 32'h00000a00;
				4'd5: pal_init = 32'h00000a0a;
				4'd6: pal_init = 32'h00000a50;
				4'd7: pal_init = 32'h00000aaa;
				4'd8: pal_init = 32'h00000555;
				4'd9: pal_init = 32'h0000055f;
				4'd10: pal_init = 32'h000005f5;
				4'd11: pal_init = 32'h000005ff;
				4'd12: pal_init = 32'h00000f55;
				4'd13: pal_init = 32'h00000f5f;
				4'd14: pal_init = 32'h00000ff5;
				4'd15: pal_init = 32'h00000fff;
`endif
				default: pal_init = 32'd0;
			endcase
		end
	endfunction

	(* ram_style = "block" *)
	reg [31:0] cbuf [0:511];

	// -- the same, in distributed RAM (CBUF_LUTRAM) --
	//
	// curl is the four slots (64 rows of 16 bits), pall the palette:
	// 2^NP entries of RGB444, or of Y/U/V on composite. Read
	// asynchronously and registered into cb_q, so everything after it
	// sees exactly what the block RAM gives. Without CBUF_LUTRAM nothing
	// writes or reads them and synthesis removes them; with it, cbuf
	// goes instead.
	localparam integer PAL_N = (NP == 8) ? 256 : (NP == 6) ? 64 : 16;
`ifdef GPU_COMPOSITE
	localparam integer PALW = 26;
`else
	localparam integer PALW = 12;
`endif
	(* ram_style = "distributed" *)
	reg [15:0] curl [0:63];
	(* ram_style = "distributed" *)
	reg [PALW-1:0] pall [0:PAL_N-1];

	integer cb_i;
	initial begin
		for (cb_i = 0; cb_i < 512; cb_i = cb_i + 1)
			cbuf[cb_i] = 32'd0;
		for (cb_i = 0; cb_i < 64; cb_i = cb_i + 1)
			curl[cb_i] = 16'd0;
		// -- cursor shapes, generated --
		// slot 0: 5x5 X (normal)
		cbuf[6] = 32'h00000440; curl[6] = 16'h0440;	// ......#...#.....
		cbuf[7] = 32'h00000280; curl[7] = 16'h0280;	// .......#.#......
		cbuf[8] = 32'h00000100; curl[8] = 16'h0100;	// ........#.......
		cbuf[9] = 32'h00000280; curl[9] = 16'h0280;	// .......#.#......
		cbuf[10] = 32'h00000440; curl[10] = 16'h0440;	// ......#...#.....
		// slot 1: 5x5 Z (busy)
		cbuf[22] = 32'h000007c0; curl[22] = 16'h07c0;	// ......#####.....
		cbuf[23] = 32'h00000200; curl[23] = 16'h0200;	// .........#......
		cbuf[24] = 32'h00000100; curl[24] = 16'h0100;	// ........#.......
		cbuf[25] = 32'h00000080; curl[25] = 16'h0080;	// .......#........
		cbuf[26] = 32'h000007c0; curl[26] = 16'h07c0;	// ......#####.....
		// slot 2: 9x9 X
		cbuf[36] = 32'h00001010; curl[36] = 16'h1010;	// ....#.......#...
		cbuf[37] = 32'h00000820; curl[37] = 16'h0820;	// .....#.....#....
		cbuf[38] = 32'h00000440; curl[38] = 16'h0440;	// ......#...#.....
		cbuf[39] = 32'h00000280; curl[39] = 16'h0280;	// .......#.#......
		cbuf[40] = 32'h00000100; curl[40] = 16'h0100;	// ........#.......
		cbuf[41] = 32'h00000280; curl[41] = 16'h0280;	// .......#.#......
		cbuf[42] = 32'h00000440; curl[42] = 16'h0440;	// ......#...#.....
		cbuf[43] = 32'h00000820; curl[43] = 16'h0820;	// .....#.....#....
		cbuf[44] = 32'h00001010; curl[44] = 16'h1010;	// ....#.......#...
		// slot 3: 9x9 Z
		cbuf[52] = 32'h00001ff0; curl[52] = 16'h1ff0;	// ....#########...
		cbuf[53] = 32'h00000800; curl[53] = 16'h0800;	// ...........#....
		cbuf[54] = 32'h00000400; curl[54] = 16'h0400;	// ..........#.....
		cbuf[55] = 32'h00000200; curl[55] = 16'h0200;	// .........#......
		cbuf[56] = 32'h00000100; curl[56] = 16'h0100;	// ........#.......
		cbuf[57] = 32'h00000080; curl[57] = 16'h0080;	// .......#........
		cbuf[58] = 32'h00000040; curl[58] = 16'h0040;	// ......#.........
		cbuf[59] = 32'h00000020; curl[59] = 16'h0020;	// .....#..........
		cbuf[60] = 32'h00001ff0; curl[60] = 16'h1ff0;	// ....#########...
		// -- end of cursor shapes --
		// the palette: EGA's sixteen, repeated through all 256 entries
		for (cb_i = 0; cb_i < 256; cb_i = cb_i + 1)
			cbuf[256 + cb_i] = pal_init(cb_i % 16);
		for (cb_i = 0; cb_i < PAL_N; cb_i = cb_i + 1)
			pall[cb_i] = pal_init(cb_i % 16);
	end

	reg [14:0] alt_sync0, alt_sync1;
	reg [14:0] s1r_sync0, s1r_sync1, s1a_sync0, s1a_sync1;
	reg [2:0] dy_sync0, dy_sync1;

	// The fetch walks planes 1..NP-1: pf_k is the plane, pf_w its word.
	// Every plane's row base is latched when the row refill ends --
	// pf_base for plane 1, pf_b2..pf_b7 for the rest -- and pf_base
	// takes the next one, registered, as each plane ends, so the issue
	// path is register -> adder for any number of planes, and the
	// choice of the next base is a mux between registers, off it.
	reg pf_run;
	reg [2:0] pf_k;
	reg [4:0] pf_w;
	reg [14:0] pf_base;
	reg [14:0] pf_b2, pf_b3, pf_b4, pf_b5, pf_b6, pf_b7;
	reg pf_v1, pf_v2;
	reg [7:0] pf_a1, pf_a2;

	reg pf_ld;

	// Planes this build does not have select pf_b3, so a 4-plane build
	// keeps the 2:1 choice it needs and no more.
	reg [14:0] pf_bnext;
	always @(*) begin
		case (pf_k)
			3'd1: pf_bnext = pf_b2;
			3'd2: pf_bnext = pf_b3;
			3'd3: pf_bnext = (NP > 4) ? pf_b4 : pf_b3;
			3'd4: pf_bnext = (NP > 4) ? pf_b5 : pf_b3;
			3'd5: pf_bnext = (NP > 6) ? pf_b6 : pf_b3;
			default: pf_bnext = (NP > 6) ? pf_b7 : pf_b3;
		endcase
	end

	// pf_run is only ever set as the row refill ends and is cleared
	// when the next one starts, so refill_words is 0 whenever it is
	// set: no compare on the counter here.
	wire pf_issue = (COLOR_AVAIL != 0) && pf_run && !refill;

	always @(posedge clk) begin
		alt_sync0 <= alt_refill;
		alt_sync1 <= alt_sync0;
		s1r_sync0 <= s1r_refill;
		s1r_sync1 <= s1r_sync0;
		s1a_sync0 <= s1a_refill;
		s1a_sync1 <= s1a_sync0;
		dy_sync0 <= { off_use[5], off_use[3], off_use[1] };
		dy_sync1 <= dy_sync0;
	end

	always @(posedge clk) begin
		if (refill) begin
			refill_words <= 21;
			gb_adr_o <= row_base + 19;
			pf_run <= 1'b0;
		end else if (refill_words > 0) begin
			if (refill_words > 2)
				gb_adr_o <= row_base + (refill_words - 3);
			refill_words <= refill_words - 1;
			// row done: plane 1's row base, and start (the other
			// planes' bases are latched on the same edge, below)
			if (refill_words == 6'd1 && COLOR_AVAIL != 0) begin
				pf_run <= 1'b1;
				pf_k <= 3'd1;
				pf_w <= 5'd0;
				pf_base <= dy_sync1[0] ? alt_sync1 : row_base;
			end
		end else if (pf_issue) begin
			gb_adr_o <= pf_base + { 10'd0, pf_w };
			if (pf_w == 5'd19) begin
				pf_w <= 5'd0;
				if (pf_k == NP - 1) begin
					pf_run <= 1'b0;
				end else begin
					pf_k <= pf_k + 3'd1;
					pf_base <= pf_bnext;
				end
			end else begin
				pf_w <= pf_w + 5'd1;
			end
		end

		// store pipeline: the word addressed on this edge arrives on
		// gb_dat_i two edges later, as for the row
		pf_v1 <= pf_issue;
		pf_a1 <= { pf_k, pf_w };
		pf_v2 <= pf_v1;
		pf_a2 <= pf_a1;

		if (!resetn) begin
			pf_run <= 1'b0;
			pf_v1 <= 1'b0;
			pf_v2 <= 1'b0;
		end
	end

	// Planes 2..NP-1's row bases, latched on the edge the row refill
	// ends (refill_words 1, as pf_base above), with a registered strobe
	// of their own: a simple enable on these registers, rather than a
	// branch of the sequencer above fanning out to all of them. A row
	// refill starting on that same edge only means they are latched
	// for a fetch that will not run, which is harmless.
	always @(posedge clk) begin
		pf_ld <= (COLOR_AVAIL != 0) && !refill && (refill_words == 6'd2);
		if (pf_ld) begin
			pf_b2 <= dy_sync1[1] ? alt_sync1 : row_base;
			pf_b3 <= dy_sync1[2] ? alt_sync1 : row_base;
			if (NP > 4) begin
				pf_b4 <= s1r_sync1;
				pf_b5 <= dy_sync1[0] ? s1a_sync1 : s1r_sync1;
			end
			if (NP > 6) begin
				pf_b6 <= dy_sync1[1] ? s1a_sync1 : s1r_sync1;
				pf_b7 <= dy_sync1[2] ? s1a_sync1 : s1r_sync1;
			end
		end
	end

	// The row's words arrive while refill_words counts 20..1, word
	// 19 first: the word on gb_dat_i is word refill_words - 1 (issued
	// two edges earlier, at refill_words + 1). The plane stores start
	// two edges after the row's last, so the two never meet.
	wire row_we = !refill && (refill_words != 6'd0) && (refill_words != 6'd21);
	wire [4:0] row_w = refill_words[4:0] - 5'd1;

	always @(posedge clk) begin
		if (pf_v2)
			pbuf[pf_a2] <= gb_dat_i;
		else if (row_we)
			pbuf[{ 3'd0, row_w }] <= gb_dat_i;
	end

	// cbuf's write port: a cursor row from socctl's CURDATA, or a
	// palette entry from its PALETTE -- one-clock strobes from separate
	// stores, so never on the same clock; the palette wins if they
	// were. An entry's bits above the palette's size are ignored.
	localparam [7:0] PAL_MASK = (NP == 8) ? 8'hff : (NP == 6) ? 8'h3f : 8'h0f;
	wire cb_pal = (COLOR_AVAIL != 0) && pal_we;
	wire cb_we = cb_pal || ((CURSOR_AVAIL != 0) && curs_we);
	wire [8:0] cb_wa = cb_pal ? { 1'b1, pal_idx & PAL_MASK } : { 3'd0, curs_waddr };
	wire [31:0] cb_wd = cb_pal ? pal_wword : curs_wdata;

	always @(posedge clk) begin
		if (cb_we && CBUF_LUTRAM == 0)
			cbuf[cb_wa] <= cb_wd;
		if (cb_we && CBUF_LUTRAM != 0 && cb_pal)
			pall[pal_idx & PAL_MASK] <= cb_wd[PALW-1:0];
		if (cb_we && CBUF_LUTRAM != 0 && !cb_pal)
			curl[curs_waddr] <= cb_wd[15:0];
	end

	// -- colour: the plane read side, pixel domain --
	//
	// cur0..cur7 hold the word each plane is showing; nxt0..nxt7 the
	// word after it. At a word crossing (the edge on which x moves
	// from bit 31 of a word to bit 0 of the next) cur takes nxt, and a
	// short pass refills nxt from the buffer. A pass is one read per
	// plane through a registered read port: NP + 3 clocks, seven for
	// four planes and eleven for eight; the next crossing is at least
	// 64 clocks away in game mode (32 source pixels, each doubled) and
	// 32 in desktop mode. Plane 0 is the framebuffer row, so this runs
	// on every board and in every mode; without colour, planes 1..3
	// are read and dropped (nothing uses nxt1..nxt3, and synthesis
	// removes them).
	//
	// The line start is a preload: at 24 clocks before the first
	// visible pixel (two passes take 22 at eight planes) -- long after
	// the fetch above has finished -- one pass loads the viewport's
	// first word into nxt, nxt moves to cur, and a second pass loads
	// the word after it. x already holds the viewport origin at that
	// point (it is loaded throughout blanking), so the origin needs no
	// separate path, and a non-word-aligned origin is just a different
	// x[4:0] into cur. The first pass is over by 12 clocks in, before
	// x starts, a clock early or not.
	//
	// Word arithmetic is modulo 20 throughout -- the framebuffer row
	// is a 20-word torus -- and a plane with dx set reads word
	// (w + 10) mod 20 where plane 0 reads w, which is the 320-column
	// offset. Plane 4+j takes plane j's dx. In clamp mode the "next"
	// word after the last visible one is never shown, so the wrap there
	// is harmless rather than wrong.
	reg [31:0] nxt0, nxt1, nxt2, nxt3, nxt4, nxt5, nxt6, nxt7;
	reg [31:0] pq;
	reg [7:0] pra;
	reg [3:0] ps;
	reg [4:0] pw;
	reg pre;

	// the pass's last step, on which cur takes nxt during a preload
	localparam [3:0] PS_END = NP + 3;

	wire [4:0] xw = x[9:5];
	wire [5:0] xw2_sum = { 1'b0, xw } + 6'd2;
	wire [4:0] xw2 = (xw2_sum >= 6'd20) ? (xw2_sum[4:0] - 5'd20) : xw2_sum[4:0];
	wire [5:0] pw1_sum = { 1'b0, pw } + 6'd1;
	wire [4:0] pw1 = (pw1_sum >= 6'd20) ? (pw1_sum[4:0] - 5'd20) : pw1_sum[4:0];
	wire [4:0] pw_dx = (pw >= 5'd10) ? (pw - 5'd10) : (pw + 5'd10);
	wire [4:0] pw_k1 = off_use[0] ? pw_dx : pw;
	wire [4:0] pw_k2 = off_use[2] ? pw_dx : pw;
	wire [4:0] pw_k3 = off_use[4] ? pw_dx : pw;

	wire x_cross = x_run && x_step && (x[4:0] == 5'd31);
	wire pre_go = (hc == h_disp_start - 11'd24);

	// -- the hardware cursor: read side, pixel domain (docs/cursor.md) --
	//
	// Once a line, in horizontal blanking (CF_HC), the shape row this
	// line shows is chosen: the slot (normal or busy) and the row,
	// from dy = y - curs_y: the cell's centre (8, 8) is drawn at the
	// pointer, so the line shows a row if -8 <= dy <= 7 (dy[10:3] all
	// zeros or all ones), and that row is dy + 8, which is dy[3:0]
	// with bit 3 flipped -- no adder for the 8.
	//
	// The row is read through cbuf's read port on the clock after
	// CF_HC (c_rd) and held in c_row from the clock after that (c_lat)
	// for the rest of the line: every other clock the port is the
	// palette's. A build without colour has no palette, so there the
	// port's address stays on the row and its registered read is the
	// row, as it always was.
	//
	// Per pixel the same with dx = x - curs_x (curs_x latched with the
	// row): the row's bit at column dx + 8, registered -- one clock
	// behind x, exactly as gpu_cursor.v's `pixel` was. A column
	// or row outside 0..15 draws nothing, so the pointer never wraps
	// to the opposite edge, as before.
	localparam [10:0] CF_HC = 11'd64;

	reg [5:0] c_a;
	reg [9:0] c_px;
	// The pointer in framebuffer coordinates: curs_x/y as they are, or,
	// with curs_scr in game mode, half of them (the view at (0, 0)).
	wire c_half = curs_scr && game_active;
	wire [9:0] c_fx = c_half ? { 1'b0, curs_x[9:1] } : curs_x;
	wire [9:0] c_fy = c_half ? { 1'b0, curs_y[9:1] } : curs_y;
	reg c_rv;
	reg c_rd, c_lat;
	reg [15:0] c_row;

	wire c_on = (CURSOR_AVAIL != 0);
	wire [10:0] c_dy = { 1'b0, y } - { 1'b0, c_fy };
	wire [10:0] c_dx = { 1'b0, x } - { 1'b0, c_px };
	wire c_cin = c_rv && ((c_dx[10:3] == 8'h00) || (c_dx[10:3] == 8'hff));
	wire [15:0] c_rowv = (COLOR_AVAIL != 0) ? c_row : cb_q[15:0];

	// cbuf's read address: the cursor's row on its one clock, the
	// pixel's palette entry (words 256 up) on every other
	wire [8:0] cb_ra = ((COLOR_AVAIL == 0) || c_rd) ? { 3'd0, c_a } :
		{ 1'b1, cidx };

	// The index whose colour cb_q holds -- the pixel on screen, while
	// colour is on. For the benches only; nothing reads it, and
	// synthesis removes it.
	reg [7:0] cidx_o;

	// CBUF_LUTRAM's read: the same address, the same register
	wire [31:0] cb_lq = cb_ra[8] ? { {(32 - PALW){1'b0}}, pall[cb_ra[7:0] & PAL_MASK] } :
		{ 16'd0, curl[cb_ra[5:0]] };

	always @(posedge pclk) begin
		if (CBUF_LUTRAM == 0)
			cb_q <= cbuf[cb_ra];
		else
			cb_q <= cb_lq;
		cidx_o <= cidx;
		c_d <= c_on && c_cin && c_rowv[{ ~c_dx[3], c_dx[2:0] }];
		c_rd <= c_on && (hc == CF_HC);
		c_lat <= c_rd;
		if (c_lat)
			c_row <= cb_q[15:0];
		if (hc == CF_HC) begin
			c_a <= { (curs_alt ? curs_slots[3:2] : curs_slots[1:0]), ~c_dy[3], c_dy[2:0] };
			c_rv <= c_on && !curs_hide &&
				((c_dy[10:3] == 8'h00) || (c_dy[10:3] == 8'hff));
			c_px <= c_fx;
		end
	end

	always @(posedge pclk) begin
		pq <= pbuf[pra];

		if (!resetn) begin
			ps <= 4'd0;
			pre <= 1'b0;
			pw <= 5'd0;
			pra <= 8'd0;
		end else begin
			if (pre_go) begin
				ps <= 4'd1;
				pw <= xw;
				pre <= 1'b1;
			end else if (x_cross) begin
				cur0 <= nxt0;
				cur1 <= nxt1;
				cur2 <= nxt2;
				cur3 <= nxt3;
				cur4 <= nxt4;
				cur5 <= nxt5;
				cur6 <= nxt6;
				cur7 <= nxt7;
				ps <= 4'd1;
				pw <= xw2;
			end else if (ps == PS_END) begin
				if (pre) begin
					cur0 <= nxt0;
					cur1 <= nxt1;
					cur2 <= nxt2;
					cur3 <= nxt3;
					cur4 <= nxt4;
					cur5 <= nxt5;
					cur6 <= nxt6;
					cur7 <= nxt7;
					pw <= pw1;
					pre <= 1'b0;
					ps <= 4'd1;
				end else begin
					ps <= 4'd0;
				end
			end else if (ps != 4'd0) begin
				// pra is registered and so is pq: the word for the
				// address set on one edge is in pq two edges later.
				// Plane p is addressed on step p + 1 and taken on
				// step p + 3; steps past this build's planes read
				// rows nothing uses.
				// Addresses for planes this build does not have are
				// never set (NP > p), so the address mux is only as
				// wide as the planes.
				ps <= ps + 4'd1;
				case (ps)
					4'd1: pra <= { 3'd0, pw };
					4'd2: pra <= { 3'd1, pw_k1 };
					4'd3: begin pra <= { 3'd2, pw_k2 }; nxt0 <= pq; end
					4'd4: begin pra <= { 3'd3, pw_k3 }; nxt1 <= pq; end
					4'd5: begin if (NP > 4) pra <= { 3'd4, pw };    nxt2 <= pq; end
					4'd6: begin if (NP > 5) pra <= { 3'd5, pw_k1 }; nxt3 <= pq; end
					4'd7: begin if (NP > 6) pra <= { 3'd6, pw_k2 }; nxt4 <= pq; end
					4'd8: begin if (NP > 7) pra <= { 3'd7, pw_k3 }; nxt5 <= pq; end
					4'd9: nxt6 <= pq;
					4'd10: nxt7 <= pq;
					default: ;
				endcase
			end
		end
	end

endmodule
