`timescale 1ns / 1ps

// Self-checking testbench for composite COLOUR in rtl/gpu/gpu_video.v
// (docs/composite.md, "Colour").
//
// Like tb_composite.v this one MEASURES. The question is not which
// branch fired but whether a receiver would decode the right colour, so
// the bench is a small software receiver: it takes the DAC samples of a
// line, correlates them against an ideal subcarrier at the standard's
// nominal frequency, and reads off the phase and amplitude of the burst
// and of the picture -- which is what a TV's decoder does.
//
// Checked here:
//
//   1. Colour off: only the three monochrome levels anywhere in a
//      frame, and no burst. The desktop is the black-and-white signal it
//      always was.
//   2. Colour on, a solid field in each of eight colours: the picture's
//      average level is the colour's luma, its chroma PHASE relative to
//      the burst is the colour's hue, and its amplitude relative to the
//      burst is the colour's saturation -- within what a 4-bit DAC can
//      resolve.
//   3. The subcarrier frequency: the burst phase, measured against an
//      ideal oscillator over a hundred lines, does not drift.
//   4. PAL only: V inverts on alternate lines and the burst moves
//      between 135 and 225 degrees with it, and both lines decode to the
//      same colour.
//   5. CVBS mono: colour on, but no burst and no chroma -- a flat line
//      at the colour's luma.
//   6. The planes on composite timing: a 4-colour layout whose plane 1
//      is all ones decodes to palette entry 2's colour.
//   7. Codes stay within 2..15 in the picture: never down at sync level.
//
// Running it -- the usual trailing-comma patch (rtl/gpu/bench/README.md):
//
//   sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
//       rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
//   iverilog -g2005 -DGPU_COMPOSITE -o /tmp/tb_cvbs.out \
//       rtl/gpu/bench/tb_cvbs_color.v /tmp/gpu_video_fix.v
//   vvp /tmp/tb_cvbs.out
//
// and PAL with -DGPU_COMPOSITE_PAL as well. A few minutes each.

module tb_cvbs_color;

`ifdef GPU_COMPOSITE_PAL
	localparam H_DISP = 1280, H_FP = 57, H_PW = 118, H_BP = 158;
	localparam V_DISP = 240,  V_FP = 25, V_PW = 3,   V_BP = 44;
	localparam real FSC = 4433618.75;
	localparam BURST_DLY = 141, BURST_LEN = 57;
	localparam IS_PAL = 1;
`else
	localparam H_DISP = 1280, H_FP = 65, H_PW = 118, H_BP = 139;
	localparam V_DISP = 240,  V_FP = 3,  V_PW = 3,   V_BP = 16;
	localparam real FSC = 3579545.4545;
	localparam BURST_DLY = 134, BURST_LEN = 63;
	localparam IS_PAL = 0;
`endif
	localparam H_START = H_FP + H_PW + H_BP;
	localparam H_LINE  = H_START + H_DISP;
	localparam V_START = V_FP + V_PW + V_BP;
	localparam V_STOP  = V_START + V_DISP;
	localparam real PCLK = 25200000.0;
	localparam real PI = 3.14159265358979;

	reg clk = 0, pclk = 0, bclk = 0, resetn = 0;
	reg [31:0] gb_dat_i = 0;
	reg view_load = 0;
	reg color_en = 0;
	reg [1:0] color_np = 0;
	reg [5:0] color_off = 0;
	reg pal_we = 0;
	reg [3:0] pal_idx = 0;
	reg [11:0] pal_rgb = 0;
	reg cvbs_mono = 0;

	wire red, green, blue, hsync, vsync, is_visible;
	wire [9:0] x, y;
	wire [14:0] gb_adr_o;
	wire [3:0] dvi_p, dac;
	wire [15:0] frame_ctr;
	wire in_vblank;

	always #10.417 clk  = ~clk;
	always #19.841 pclk = ~pclk;
	always #3.968  bclk = ~bclk;

	gpu_video #(
		.h_disp(H_DISP), .h_front_porch(H_FP), .h_pulse_width(H_PW),
		.h_back_porch(H_BP), .h_line(H_LINE),
		.v_disp(V_DISP), .v_front_porch(V_FP), .v_pulse_width(V_PW),
		.v_back_porch(V_BP), .v_frame(V_STOP),
		.H_DIV_BASE(3'd4), .FIXED_VIEWPORT(1'b1), .COLOR_AVAIL(1'b1)
	) dut (
		.pixel(1'b0), .clk(clk), .pclk(pclk), .bclk(bclk), .resetn(resetn),
		.video_mode(2'd0), .view_load(view_load), .game_en(1'b0),
		.game_wrap(1'b0), .view_x(10'd0), .view_y(10'd0),
		.color_en(color_en), .color_np(color_np), .color_off(color_off),
		.pal_we(pal_we), .pal_idx(pal_idx), .pal_rgb(pal_rgb),
		.cvbs_mono(cvbs_mono),
		.frame_ctr(frame_ctr), .in_vblank(in_vblank),
		.red(red), .green(green), .blue(blue), .hsync(hsync), .vsync(vsync),
		.dvi_p(dvi_p), .dac(dac), .x(x), .y(y), .is_visible(is_visible),
		.gb_adr_o(gb_adr_o), .gb_dat_i(gb_dat_i)
	);

	reg [31:0] vram [0:9599];
	always @(posedge clk) gb_dat_i <= vram[gb_adr_o];

	// absolute sample counter: sample n is taken at time n / PCLK
	integer n_abs = 0;
	always @(posedge pclk) n_abs <= n_abs + 1;

	integer errors = 0;

	// -- one line of samples --
	reg [3:0] ln [0:2047];
	integer ln_n0;          // n_abs of sample 0
	integer ln_alt;         // PAL: the V switch for this line

	task capture;           // the line whose number is vline
		input integer vline;
		integer i;
		begin
			while (!(dut.vc == vline && dut.hc == 0)) @(posedge pclk);
			ln_n0 = n_abs;
			for (i = 0; i < H_LINE; i = i + 1) begin
				@(negedge pclk);
				ln[i] = dac;
				if (i == H_FP + BURST_DLY + 10) ln_alt = dut.s1_alt;
				@(posedge pclk);
			end
		end
	endtask

	// Correlate samples a..b-1 with the ideal subcarrier. The DAC is two
	// samples behind the counters (the output pipeline), which shifts
	// nothing here: the reference phase follows each sample's own time.
	real c_mean, c_amp, c_phase;
	task demod;
		input integer a;
		input integer b;
		integer i;
		real s, si, co, w, t;
		begin
			s = 0; si = 0; co = 0;
			for (i = a; i < b; i = i + 1) s = s + ln[i];
			c_mean = s / (b - a);
			for (i = a; i < b; i = i + 1) begin
				t = (ln_n0 + i) / PCLK;
				w = 2.0 * PI * FSC * t;
				si = si + (ln[i] - c_mean) * $sin(w);
				co = co + (ln[i] - c_mean) * $cos(w);
			end
			c_amp = 2.0 * $sqrt(si * si + co * co) / (b - a);
			c_phase = $atan2(co, si) * 180.0 / PI;
		end
	endtask

	// angle difference folded into -180..180
	function real wrapd;
		input real d;
		begin
			wrapd = d;
			while (wrapd > 180.0) wrapd = wrapd - 360.0;
			while (wrapd < -180.0) wrapd = wrapd + 360.0;
		end
	endfunction

	task set_color;
		input en;
		input [1:0] np;
		input [5:0] off;
		begin
			@(posedge clk);
			color_en <= en;
			color_np <= np;
			color_off <= off;
			view_load <= ~view_load;
		end
	endtask

	task pal_write;
		input [3:0] i;
		input [11:0] rgb;
		begin
			@(posedge clk);
			pal_we <= 1; pal_idx <= i; pal_rgb <= rgb;
			@(posedge clk);
			pal_we <= 0;
		end
	endtask

	task next_frame;
		begin
			repeat (8) @(posedge pclk);
			while (!(dut.vc == V_STOP - 1 && dut.hc == H_LINE - 1)) @(posedge pclk);
			repeat (4) @(posedge pclk);
		end
	endtask

	// -- expected colour, from the RGB444 the palette was given --
	real e_y, e_u, e_v;
	task expect_yuv;
		input [11:0] rgb;
		real r, g, b;
		begin
			r = rgb[11:8] / 15.0; g = rgb[7:4] / 15.0; b = rgb[3:0] / 15.0;
			e_y = 10.0 * (0.299 * r + 0.587 * g + 0.114 * b);  // in DAC codes
			e_u = 0.492 * (10.0 * b - e_y);
			e_v = 0.877 * (10.0 * r - e_y);
		end
	endtask

	// burst at samples (2 behind the window, trimmed)
	localparam BA = H_FP + BURST_DLY + 2 + 4;
	localparam BB = H_FP + BURST_DLY + 2 + BURST_LEN - 4;
	localparam PA = H_START + 60;
	localparam PB = H_START + H_DISP - 60;

	real b_phase, b_amp, rel, want, ampr, wamp;
	integer i, lo, hi, bad;

	task check_colour;
		input [3:0] idx;
		input [11:0] rgb;
		input [8*24-1:0] name;
		integer vline;
		begin
			pal_write(idx, rgb);
			expect_yuv(rgb);
			next_frame();
			vline = V_START + 100;
			capture(vline);
			demod(BA, BB); b_phase = c_phase; b_amp = c_amp;
			demod(PA, PB);
			// luma
			if ((c_mean - (5.0 + e_y)) > 0.6 || (c_mean - (5.0 + e_y)) < -0.6) begin
				$display("FAIL: %0s: level %f, want %f", name, c_mean, 5.0 + e_y);
				errors = errors + 1;
			end
			wamp = $sqrt(e_u * e_u + e_v * e_v);
			if (wamp >= 1.5) begin
				// hue, relative to the burst
				if (IS_PAL && ln_alt)
					want = wrapd($atan2(-e_v, e_u) * 180.0 / PI - 225.0);
				else if (IS_PAL)
					want = wrapd($atan2(e_v, e_u) * 180.0 / PI - 135.0);
				else
					want = wrapd($atan2(e_v, e_u) * 180.0 / PI - 180.0);
				rel = wrapd(c_phase - b_phase);
				if (wrapd(rel - want) > 15.0 || wrapd(rel - want) < -15.0) begin
					$display("FAIL: %0s: hue %f deg from burst, want %f", name, rel, want);
					errors = errors + 1;
				end
				// saturation, relative to the burst (2 codes), unless
				// the waveform would be clamped
				if (5.0 + e_y + wamp <= 15.0 && 5.0 + e_y - wamp >= 2.0) begin
					ampr = (c_amp / b_amp) / (wamp / 2.0);
					if (ampr > 1.3 || ampr < 0.7) begin
						$display("FAIL: %0s: chroma %f x the burst, want %f",
							name, c_amp / b_amp, wamp / 2.0);
						errors = errors + 1;
					end
				end
			end else if (c_amp > 0.5) begin
				$display("FAIL: %0s: grey has chroma %f", name, c_amp);
				errors = errors + 1;
			end
			// within 2..15
			lo = 15; hi = 0;
			for (i = H_START + 4; i < H_START + H_DISP; i = i + 1) begin
				if (ln[i] < lo) lo = ln[i];
				if (ln[i] > hi) hi = ln[i];
			end
			if (lo < 2) begin
				$display("FAIL: %0s: picture code %0d, below 2", name, lo);
				errors = errors + 1;
			end
			$display("     %0s: level %f, burst %f@%f, chroma %f@%f",
				name, c_mean, b_amp, b_phase, c_amp, wrapd(c_phase - b_phase));
		end
	endtask

	real ph0, phmin, phmax, d;
	integer k;

	initial begin
		for (i = 0; i < 9600; i = i + 1) vram[i] = 0;
		resetn = 0;
		repeat (8) @(posedge pclk);
		resetn = 1;

		// ---- 1: colour off: three levels, no burst ----
		next_frame();
		bad = 0;
		for (k = 0; k < 40; k = k + 1) begin
			capture(V_START + 10 + k * 5);
			for (i = 0; i < H_LINE; i = i + 1)
				if (ln[i] != 0 && ln[i] != 5 && ln[i] != 15) bad = bad + 1;
			for (i = BA; i < BB; i = i + 1)
				if (ln[i] != 5) bad = bad + 1;
		end
		if (bad) begin
			$display("FAIL: colour off: %0d samples not monochrome", bad);
			errors = errors + 1;
		end else $display("ok:   colour off is the monochrome signal");

		// ---- 2: colours ----
		set_color(1, 2'd0, 6'd0);
		check_colour(4'd0, 12'h900, "red");
		check_colour(4'd0, 12'h090, "green");
		check_colour(4'd0, 12'h009, "blue");
		check_colour(4'd0, 12'h099, "cyan");
		check_colour(4'd0, 12'h909, "magenta");
		check_colour(4'd0, 12'hb60, "orange");
		check_colour(4'd0, 12'h47c, "light blue");
		check_colour(4'd0, 12'h888, "grey");

		// ---- 3: subcarrier frequency: burst phase over 100 lines ----
		pal_write(4'd0, 12'h000);
		next_frame();
		phmin = 1000; phmax = -1000; ph0 = 0;
		for (k = 0; k < 100; k = k + 1) begin
			capture(V_START + 20 + k);
			demod(BA, BB);
			// PAL: undo the switch, so both lines read the same
			if (IS_PAL) c_phase = c_phase + (ln_alt ? -45.0 : 45.0);
			if (k == 0) ph0 = c_phase;
			d = wrapd(c_phase - ph0);
			if (d < phmin) phmin = d;
			if (d > phmax) phmax = d;
		end
		if (phmax - phmin > 30.0) begin
			$display("FAIL: burst phase wanders %f..%f deg over 100 lines", phmin, phmax);
			errors = errors + 1;
		end else
			$display("ok:   burst phase steady over 100 lines (%f..%f)", phmin, phmax);

		// ---- 4: PAL: both phases of the switch decode alike ----
		if (IS_PAL) begin
			pal_write(4'd0, 12'h909);
			expect_yuv(12'h909);
			next_frame();
			for (k = 0; k < 2; k = k + 1) begin
				capture(V_START + 50 + k);
				demod(BA, BB); b_phase = c_phase;
				demod(PA, PB);
				rel = wrapd(c_phase - b_phase);
				want = ln_alt ? wrapd($atan2(-e_v, e_u) * 180.0 / PI - 225.0)
				              : wrapd($atan2(e_v, e_u) * 180.0 / PI - 135.0);
				if (wrapd(rel - want) > 15.0 || wrapd(rel - want) < -15.0) begin
					$display("FAIL: PAL line alt=%0d: hue %f, want %f", ln_alt, rel, want);
					errors = errors + 1;
				end else
					$display("ok:   PAL line alt=%0d decodes (%f, want %f)", ln_alt, rel, want);
			end
		end

		// ---- 5: CVBS mono: no burst, no chroma, the luma ----
		pal_write(4'd0, 12'h909);
		expect_yuv(12'h909);
		cvbs_mono = 1;
		next_frame();
		capture(V_START + 60);
		lo = 15; hi = 0; bad = 0;
		for (i = BA; i < BB; i = i + 1) if (ln[i] != 5) bad = bad + 1;
		for (i = PA; i < PB; i = i + 1) begin
			if (ln[i] < lo) lo = ln[i];
			if (ln[i] > hi) hi = ln[i];
		end
		if (bad || hi != lo || (lo - (5.0 + e_y)) > 0.6 || (lo - (5.0 + e_y)) < -0.6) begin
			$display("FAIL: mono: burst samples %0d, picture %0d..%0d, want %f flat",
				bad, lo, hi, 5.0 + e_y);
			errors = errors + 1;
		end else $display("ok:   CVBS mono: no burst, flat at %0d", lo);
		cvbs_mono = 0;

		// ---- 6: planes on composite timing ----
		// plane 1 at +320 columns, all ones: index 2 everywhere
		for (i = 0; i < 240; i = i + 1)
			for (k = 10; k < 20; k = k + 1) vram[i * 20 + k] = 32'hFFFFFFFF;
		// entry 0 something else entirely, so the wrong index shows
		pal_write(4'd0, 12'h00f);
		set_color(1, 2'd1, 6'b00_00_01);
		check_colour(4'd2, 12'h0a0, "plane 1 -> entry 2");

		if (errors == 0) $display("RESULT: PASS");
		else $display("RESULT: FAIL (%0d errors)", errors);
		$finish;
	end

endmodule
