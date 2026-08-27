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
	parameter [10:0] v_frame = 525

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

	output red,
	output green,
	output blue,
	output hsync,
	output vsync,

	output [3:0] dvi_p,

	output [3:0] dac,

	output reg [9:0] x,
	output reg [9:0] y,
	output is_visible,

	output reg [14:0] gb_adr_o,
	input [31:0] gb_dat_i,

);

	reg [10:0] hc;
	reg [10:0] vc;

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

	wire pix = hline[x] || pixel;

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

	assign red   = (video_mode_active == GPU_MODE_GREEN) ? 1'b0 : pset;
	assign green = pset;
	assign blue  = (video_mode_active == GPU_MODE_AMBER ||
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
	wire [7:0] ddmi_red =
		(video_mode_active == GPU_MODE_AMBER) ?
			{pset, pset, 1'b0, pset, 1'b0, 1'b0, pset, 1'b0} :
		(video_mode_active == GPU_MODE_GREEN) ? 8'b0 :
			{pset, 7'b0};

	wire [7:0] ddmi_green =
		(video_mode_active == GPU_MODE_AMBER) ?
			{pset, 1'b0, 1'b0, pset, pset, pset, 1'b0, pset} :
			{pset, 7'b0};

	wire [7:0] ddmi_blue =
		(video_mode_active == GPU_MODE_AMBER ||
		 video_mode_active == GPU_MODE_GREEN) ? 8'b0 :
			{pset, 7'b0};

	ODDRX1F ddr0_clock (.D0(out_tmds_clk   [0] ), .D1(out_tmds_clk   [1] ), .Q(dvi_p[3]), .SCLK(bclk), .RST(0));
	ODDRX1F ddr0_red   (.D0(out_tmds_red   [0] ), .D1(out_tmds_red   [1] ), .Q(dvi_p[2]), .SCLK(bclk), .RST(0));
	ODDRX1F ddr0_green (.D0(out_tmds_green [0] ), .D1(out_tmds_green [1] ), .Q(dvi_p[1]), .SCLK(bclk), .RST(0));
	ODDRX1F ddr0_blue  (.D0(out_tmds_blue  [0] ), .D1(out_tmds_blue  [1] ), .Q(dvi_p[0]), .SCLK(bclk), .RST(0));

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

	// TODO

`endif

	// video timing

	parameter [10:0] h_disp_start = h_front_porch + h_pulse_width + h_back_porch;
	parameter [10:0] h_disp_stop = h_disp_start + h_disp;

	parameter [10:0] v_disp_start = v_front_porch + v_pulse_width + v_back_porch;
	parameter [10:0] v_disp_stop = v_disp_start + v_disp;

	assign is_visible = (hc >= h_disp_start && vc >= v_disp_start &&
		hc < h_disp_stop && vc < v_disp_stop);

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
	always @(posedge pclk) begin
		video_mode_sync0 <= video_mode;
		video_mode_sync1 <= video_mode_sync0;
		if (!resetn) begin
			video_mode_active <= GPU_MODE_WHITE;
		end else if (hc == h_disp_stop - 1 && vc == v_disp_stop - 1) begin
			video_mode_active <= video_mode_sync1;
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

	always @(posedge clk) begin
		refill <= refill_synced;
		y_refill_sync0 <= y_refill;
		y_refill_sync1 <= y_refill_sync0;
		if (!resetn) begin
			refill_sync <= 0;
		end else begin
			refill_sync <= {refill_sync[0], refill_toggle}; 
		end
	end

	always @(posedge pclk) begin

		if (!resetn) begin
			hc <= 0;
			vc <= 0;
			refill_toggle <= 0;
		end else if (hc == h_disp_stop - 1) begin
			refill_toggle <= ~refill_toggle;
			hc <= 0;
			if (vc == v_disp_stop - 1) begin
				vc <= 0;
			end else begin
				vc <= vc + 1;
				if (vc > v_disp_start) begin
					y <= vc - v_disp_start;
					y_refill <= vc - v_disp_start;
				end else begin
					y <= 0;
					y_refill <= 0;
				end
			end
		end else begin
			hc <= hc + 1;
			if (hc > h_disp_start) x <= hc - h_disp_start; else x <= 0;
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

	always @(posedge clk) begin
		if (refill) begin
			refill_words <= 21;
			gb_adr_o <= row_base + 19;
		end else if (refill_words > 0) begin
			if (refill_words != 21)
				hline <= { hline, gb_dat_i };
			if (refill_words > 2)
				gb_adr_o <= row_base + (refill_words - 3);
			refill_words <= refill_words - 1;
		end
	end

endmodule
