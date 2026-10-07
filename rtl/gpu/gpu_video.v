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
 *   - No new BRAM, and no extra VRAM bandwidth. hline below already
 *     holds one FULL framebuffer row (640 bits, 20 words) per
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
 * The framebuffer bit for the current pixel used to be hline[x] where
 * x counted 0..639. It is still hline[x]; x is now a LOADABLE COUNTER
 * rather than a subtraction of hc, loaded with the viewport origin at
 * the start of each line and advanced every pixel (desktop) or every
 * other pixel (game).
 *
 * That structure is not an accident and should not be "simplified"
 * back into an expression. hline[x] is a 640:1 multiplexer and is
 * almost certainly the critical path in this clock domain; putting an
 * adder in front of it -- hline[view_x + (x >> 1)], the obvious way
 * to write this -- would add that adder's delay to the longest path
 * in the design for no functional gain. A counter puts the arithmetic
 * behind a register instead, so the mux input is a register output in
 * game mode exactly as it was in desktop mode, and the path depth is
 * unchanged.
 *
 * The x and y OUTPUTS now carry framebuffer coordinates rather than
 * screen coordinates. In desktop mode those are the same thing, so
 * nothing changes. In game mode it means rtl/gpu/gpu_cursor.v needs
 * no modification at all: it compares against framebuffer
 * coordinates, so it draws the pointer at the right place in the
 * framebuffer -- and because consecutive screen pixels map to the
 * same framebuffer coordinate, the sprite comes out pixel-doubled
 * along with everything else, for free.
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
	parameter COLOR_AVAIL = 1'b0

) (

	input pixel,

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
	// color_np is planes-1 (0..3 = 2, 4, 8, 16 colours). color_off
	// holds a 2-bit {dy, dx} offset for each of planes 1..3, in that
	// order from bit 0: dx adds 320 columns, dy adds 240 rows, both
	// modulo the 640x480 torus. Plane 0 is the viewport itself.
	input color_en,
	input [1:0] color_np,
	input [5:0] color_off,

	// Palette write port, wishbone clock domain (= clk). One-cycle
	// strobe from socctl; 12-bit RGB444 into entry pal_idx.
	input pal_we,
	input [3:0] pal_idx,
	input [11:0] pal_rgb,

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
	wire p0 = hline[x];
	wire pix = p0 ^ pixel;

	// is_visible gates the result, and it MUST: in GPU_PAPER the
	// inactive state is 1, so an ungated invert would drive the VGA
	// DAC high right through the front porch, sync pulse and back
	// porch. That is not a cosmetic problem -- a monitor reads sync
	// amplitude to lock, and a "white" blanking interval is how you
	// get a display that reports no signal at all. The other three
	// modes would tolerate the sloppiness; this one does not, which
	// is exactly why the gate belongs here rather than per-mode.
	wire pset = is_visible &&
		((video_mode_active == GPU_MODE_PAPER) ? ~pix : pix);

	// -- game-mode colour: index, palette, output --
	//
	// See docs/color.md for the whole design; the short version is
	// that the 640x480x1bpp framebuffer is unchanged, and in game mode
	// up to three more 320x240 regions of it are read as extra
	// BITPLANES of the same picture. Plane 0 is the viewport, exactly
	// as in monochrome game mode, and still comes from hline. Planes
	// 1..3 come from the plane line buffer below, one held 32-bit word
	// per plane (cur1..cur3), indexed by the same x[4:0] -- which is
	// valid for every plane because a plane offset of 320 columns is
	// exactly ten words, so all planes cross word boundaries together.
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
	reg [1:0] np_active;
	reg [5:0] off_active;
	reg [31:0] cur1, cur2, cur3;

	// vp_on rather than game_active: on a composite board the
	// viewport is permanent and game_active is never consulted. Moot
	// today (rtl/sysctl.v builds COLOR_AVAIL as 0 on composite, so
	// this is a constant 0 there), but it is the term composite colour
	// will want, so it is written that way now.
	wire color_on = (COLOR_AVAIL != 0) && col_active && vp_on;

	// Which planes take part. Unused planes read as 0, so a 4-colour
	// picture uses palette entries 0..3, an 8-colour one 0..7.
	wire [3:0] plane_mask = { (np_active == 2'd3), np_active[1],
		(np_active != 2'd0), 1'b1 };

	// The cursor inverts the ACTIVE planes only, i.e. index ^ (2^n-1):
	// the same XOR the monochrome path does, generalised. Over entry 0
	// it shows the highest entry in use, and it can never land on an
	// entry the game is not using.
	wire [3:0] cidx = ({ cur3[x[4:0]], cur2[x[4:0]], cur1[x[4:0]], p0 } ^
		{ pixel, pixel, pixel, pixel }) & plane_mask;

	// 16 x RGB444, in distributed RAM: written in the wishbone domain,
	// read asynchronously in this one. A write landing mid-frame can
	// show as one wrong pixel; write the palette in vblank (software
	// does -- docs/color.md) and it cannot.
	//
	// The power-on contents are the EGA/CGA default 16, which makes
	// the 4-colour mode the familiar black/blue/green/cyan until a
	// game sets its own. Reset does not reload them (distributed RAM
	// has no reset); software that cares sets the palette it wants.
	(* ram_style = "distributed" *)
	reg [11:0] pal [0:15];

	initial begin
		pal[0]  = 12'h000;  pal[1]  = 12'h00a;
		pal[2]  = 12'h0a0;  pal[3]  = 12'h0aa;
		pal[4]  = 12'ha00;  pal[5]  = 12'ha0a;
		pal[6]  = 12'ha50;  pal[7]  = 12'haaa;
		pal[8]  = 12'h555;  pal[9]  = 12'h55f;
		pal[10] = 12'h5f5;  pal[11] = 12'h5ff;
		pal[12] = 12'hf55;  pal[13] = 12'hf5f;
		pal[14] = 12'hff5;  pal[15] = 12'hfff;
	end

	always @(posedge clk) begin
		if (pal_we)
			pal[pal_idx] <= pal_rgb;
	end

	wire [11:0] rgb = pal[cidx];

	// blanking gated here for the same reason pset gates it above
	wire [3:0] c_r = is_visible ? rgb[11:8] : 4'd0;
	wire [3:0] c_g = is_visible ? rgb[7:4]  : 4'd0;
	wire [3:0] c_b = is_visible ? rgb[3:0]  : 4'd0;

	// VGA has ONE bit per channel on every board that has VGA at all,
	// so colour there is the top bit of each channel: at most the
	// eight RGB primaries and secondaries. The EGA default palette
	// degrades to the obvious eight (its 0xa levels read as on, its
	// 0x5 levels as off).
	assign red   = color_on ? c_r[3] :
		(video_mode_active == GPU_MODE_GREEN) ? 1'b0 : pset;
	assign green = color_on ? c_g[3] : pset;
	assign blue  = color_on ? c_b[3] :
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
	wire [7:0] ddmi_red = color_on ? { c_r, c_r } :
		(video_mode_active == GPU_MODE_AMBER) ?
			{pset, pset, 1'b0, pset, 1'b0, 1'b0, pset, 1'b0} :
		(video_mode_active == GPU_MODE_GREEN) ? 8'b0 :
			{pset, 7'b0};

	wire [7:0] ddmi_green = color_on ? { c_g, c_g } :
		(video_mode_active == GPU_MODE_AMBER) ?
			{pset, 1'b0, 1'b0, pset, pset, pset, 1'b0, pset} :
			{pset, 7'b0};

	wire [7:0] ddmi_blue = color_on ? { c_b, c_b } :
		(video_mode_active == GPU_MODE_AMBER ||
		 video_mode_active == GPU_MODE_GREEN) ? 8'b0 :
			{pset, 7'b0};

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

	gpu_ddmi #() gpu_ddmi_i
	(
		.pclk(pclk),
		.tmds_clk(bclk),
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
	// they share hline and the refill -- but because the TIMING is
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

	assign dac = csync_low ? DAC_SYNC :
		(is_visible && pset) ? DAC_WHITE : DAC_BLANK;

`else

	// No composite output on this board -- tie the ladder off rather
	// than leaving it floating. Costs nothing; a board without the
	// pins never routes it anywhere.
	assign dac = 4'd0;

`endif

	// video timing

	parameter [10:0] h_disp_start = h_front_porch + h_pulse_width + h_back_porch;
	parameter [10:0] h_disp_stop = h_disp_start + h_disp;

	parameter [10:0] v_disp_start = v_front_porch + v_pulse_width + v_back_porch;
	parameter [10:0] v_disp_stop = v_disp_start + v_disp;

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

	reg game_active, wrap_active;
	reg [9:0] vx_active, vy_active;

	// colour rides the same capture and the same adoption -- see the
	// color_en port. *_active are declared with the pixel path above.
	reg col_cap;
	reg [1:0] np_cap;
	reg [5:0] off_cap;

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

		view_sync0 <= view_load;
		view_sync1 <= view_sync0;
		view_sync2 <= view_sync1;

		if (!resetn) begin
			video_mode_active <= GPU_MODE_WHITE;
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
			np_cap <= 2'd0;
			off_cap <= 6'd0;
			col_active <= 1'b0;
			np_active <= 2'd0;
			off_active <= 6'd0;
		end else begin
			if (view_edge) begin
				game_cap <= game_en;
				wrap_cap <= game_wrap;
				vx_cap <= view_x;
				vy_cap <= view_y;
				col_cap <= color_en;
				np_cap <= color_np;
				off_cap <= color_off;
			end
			if (hc == h_disp_stop - 1 && vc == v_disp_stop - 1) begin
				video_mode_active <= video_mode_sync1;
				game_active <= game_cap;
				wrap_active <= wrap_cap;
				vx_active <= vx_adopt;
				vy_active <= vy_adopt;
				col_active <= col_cap && (COLOR_AVAIL != 0);
				np_active <= np_cap;
				off_active <= off_cap;
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
	// below only adds a word index to it, exactly as the hline refill
	// adds one to row_base. It is assigned on the same edge, under the
	// same condition, as y_refill and refill_toggle, so it crosses with
	// exactly the margin y_refill's crossing already relies on.
	wire [10:0] alt_sum = { 1'b0, fb_row } + 11'd240;
	wire [9:0] fb_alt =
		(alt_sum >= 11'd480) ? (alt_sum - 11'd480) : alt_sum[9:0];
	wire [14:0] alt_base_next = ({ 5'd0, fb_alt } << 4) + ({ 5'd0, fb_alt } << 2);
	reg [14:0] alt_refill;

	// -- horizontal: the loadable pixel index --
	//
	// x is the framebuffer COLUMN, and hline[x] selects the bit. See
	// this file's header for why this is a counter and not
	// `hline[vx + (hc >> 1)]`.
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
		end else if (hc >= h_disp_start) begin
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
	reg [639:0] hline;
	wire [14:0] row_base = (y_refill_sync1 << 4) + (y_refill_sync1 << 2);   // y*20

	// -- colour: the plane fetch --
	//
	// After hline's 20 words, three more rows of 20 -- planes 1, 2
	// and 3 for the SAME physical line -- go into the plane line
	// buffer. Same port, same two-cycle latency, same "issue now,
	// store two edges later" shape as the hline refill directly above
	// it, and the same timing: fetched in the horizontal blanking
	// interval of the line before, so nothing ever reads a word that
	// is being written. 60 words at 48MHz is 1.25us; with the hline
	// refill and the crossing it is done about 45 pixel clocks into a
	// 160-clock blanking interval. The read side does not touch the
	// buffer until 16 clocks before the line starts.
	//
	// Every plane is always fetched in full, whatever color_np says:
	// the VRAM port is dedicated to scanout and otherwise idle in
	// blanking, so skipping words would buy nothing and cost a
	// comparison in the one clock domain that has no slack. dx is not
	// applied here either -- whole rows come in, in natural order, and
	// the read side picks the word. That keeps this side to a mux and
	// an adder per word, the same depth as the hline address above.
	//
	// The per-plane dy bits cross from the pixel domain on two flops.
	// They are frame-constant (adopted at the end of the last visible
	// line), so the only fetch that can sample them changing is the one
	// for the first blanking line of the next frame, which nothing
	// displays.
	//
	// The buffer itself is 3 x 20 words, addressed { plane-1, word }:
	// one DP16KD on ECP5, one RAMB18 on 7-series. Written here, in the
	// wishbone domain; read in the pixel domain below.
	(* ram_style = "block" *)
	reg [31:0] pbuf [0:127];

	reg [14:0] alt_sync0, alt_sync1;
	reg [2:0] dy_sync0, dy_sync1;

	reg pf_run;
	reg [1:0] pf_k;
	reg [4:0] pf_w;
	reg [14:0] pf_b0, pf_b1, pf_b2;
	reg pf_v1, pf_v2;
	reg [6:0] pf_a1, pf_a2;

	wire [14:0] pf_base = (pf_k == 2'd0) ? pf_b0 :
		(pf_k == 2'd1) ? pf_b1 : pf_b2;
	wire pf_issue = (COLOR_AVAIL != 0) && pf_run && !refill &&
		(refill_words == 6'd0);

	always @(posedge clk) begin
		alt_sync0 <= alt_refill;
		alt_sync1 <= alt_sync0;
		dy_sync0 <= { off_active[5], off_active[3], off_active[1] };
		dy_sync1 <= dy_sync0;
	end

	always @(posedge clk) begin
		if (refill) begin
			refill_words <= 21;
			gb_adr_o <= row_base + 19;
			pf_run <= 1'b0;
		end else if (refill_words > 0) begin
			if (refill_words != 21)
				hline <= { hline, gb_dat_i };
			if (refill_words > 2)
				gb_adr_o <= row_base + (refill_words - 3);
			refill_words <= refill_words - 1;
			// hline done: latch the three plane row bases and start.
			// Latched rather than recomputed per word so the issue
			// path below is register -> 3:1 mux -> adder.
			if (refill_words == 6'd1 && COLOR_AVAIL != 0) begin
				pf_run <= 1'b1;
				pf_k <= 2'd0;
				pf_w <= 5'd0;
				pf_b0 <= dy_sync1[0] ? alt_sync1 : row_base;
				pf_b1 <= dy_sync1[1] ? alt_sync1 : row_base;
				pf_b2 <= dy_sync1[2] ? alt_sync1 : row_base;
			end
		end else if (pf_issue) begin
			gb_adr_o <= pf_base + { 10'd0, pf_w };
			if (pf_w == 5'd19) begin
				pf_w <= 5'd0;
				if (pf_k == 2'd2)
					pf_run <= 1'b0;
				else
					pf_k <= pf_k + 2'd1;
			end else begin
				pf_w <= pf_w + 5'd1;
			end
		end

		// store pipeline: the word addressed on this edge arrives on
		// gb_dat_i two edges later, as for hline
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

	always @(posedge clk) begin
		if (pf_v2)
			pbuf[pf_a2] <= gb_dat_i;
	end

	// -- colour: the plane read side, pixel domain --
	//
	// cur1..cur3 hold the word each plane is showing; nxt1..nxt3 the
	// word after it. At a word crossing (the edge on which x moves
	// from bit 31 of a word to bit 0 of the next) cur takes nxt, and a
	// short pass refills nxt from the buffer. A pass is three reads,
	// one per plane, through a registered read port, so it takes six
	// clocks; the next crossing is at least 64 clocks away in game
	// mode (32 source pixels, each doubled) and 32 in desktop mode,
	// where none of this is displayed anyway.
	//
	// The line start is a preload: at 16 clocks before the first
	// visible pixel -- long after the fetch above has finished --
	// one pass loads the viewport's first word into nxt, nxt moves to
	// cur, and a second pass loads the word after it. x already holds
	// the viewport origin at that point (it is loaded throughout
	// blanking), so the origin needs no separate path, and a non-word-
	// aligned origin is just a different x[4:0] into cur.
	//
	// Word arithmetic is modulo 20 throughout -- the framebuffer row
	// is a 20-word torus -- and a plane with dx set reads word
	// (w + 10) mod 20 where plane 0 reads w, which is the 320-column
	// offset. In clamp mode the "next" word after the last visible one
	// is never shown, so the wrap there is harmless rather than wrong.
	reg [31:0] nxt1, nxt2, nxt3;
	reg [31:0] pq;
	reg [6:0] pra;
	reg [2:0] ps;
	reg [4:0] pw;
	reg pre;

	wire [4:0] xw = x[9:5];
	wire [5:0] xw2_sum = { 1'b0, xw } + 6'd2;
	wire [4:0] xw2 = (xw2_sum >= 6'd20) ? (xw2_sum[4:0] - 5'd20) : xw2_sum[4:0];
	wire [5:0] pw1_sum = { 1'b0, pw } + 6'd1;
	wire [4:0] pw1 = (pw1_sum >= 6'd20) ? (pw1_sum[4:0] - 5'd20) : pw1_sum[4:0];
	wire [4:0] pw_dx = (pw >= 5'd10) ? (pw - 5'd10) : (pw + 5'd10);
	wire [4:0] pw_k0 = off_active[0] ? pw_dx : pw;
	wire [4:0] pw_k1 = off_active[2] ? pw_dx : pw;
	wire [4:0] pw_k2 = off_active[4] ? pw_dx : pw;

	wire x_cross = (hc >= h_disp_start) && x_step && (x[4:0] == 5'd31);
	wire pre_go = (hc == h_disp_start - 11'd16);

	always @(posedge pclk) begin
		pq <= pbuf[pra];

		if (!resetn) begin
			ps <= 3'd0;
			pre <= 1'b0;
			pw <= 5'd0;
			pra <= 7'd0;
		end else if (COLOR_AVAIL != 0) begin
			if (pre_go) begin
				ps <= 3'd1;
				pw <= xw;
				pre <= 1'b1;
			end else if (x_cross) begin
				cur1 <= nxt1;
				cur2 <= nxt2;
				cur3 <= nxt3;
				ps <= 3'd1;
				pw <= xw2;
			end else begin
				case (ps)
					3'd1: begin pra <= { 2'd0, pw_k0 }; ps <= 3'd2; end
					3'd2: begin pra <= { 2'd1, pw_k1 }; ps <= 3'd3; end
					3'd3: begin pra <= { 2'd2, pw_k2 }; nxt1 <= pq; ps <= 3'd4; end
					3'd4: begin nxt2 <= pq; ps <= 3'd5; end
					3'd5: begin nxt3 <= pq; ps <= 3'd6; end
					3'd6: begin
						if (pre) begin
							cur1 <= nxt1;
							cur2 <= nxt2;
							cur3 <= nxt3;
							pw <= pw1;
							pre <= 1'b0;
							ps <= 3'd1;
						end else begin
							ps <= 3'd0;
						end
					end
					default: ps <= 3'd0;
				endcase
			end
		end
	end

endmodule
