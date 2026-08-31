`timescale 1ns / 1ps

// Self-checking testbench for rtl/gpu/gpu_video.v's composite output.
//
// This one MEASURES rather than inspecting. A composite signal is
// wrong in exactly one way that matters -- a receiver will not lock to
// it -- and whether it locks is a question about microseconds and
// voltages, not about which branch of a mux fired. So the testbench
// times the real waveform with $realtime and compares against the
// broadcast standards.
//
// Checked here:
//
//   1. LINE PERIOD, measured sync-edge to sync-edge, within 0.1% of
//      63.5555us (NTSC) or 64.0us (PAL). Receivers tolerate far more
//      than this; 0.1% is tight enough to catch an off-by-one in the
//      counter chain, which is the error actually likely.
//
//   2. SYNC PULSE WIDTH, within 0.2us of 4.7us. Too short and a
//      receiver may not separate it from noise; too long and it eats
//      the back porch.
//
//   3. FIELD PERIOD and LINE COUNT -- 262 lines at ~60Hz (NTSC) or
//      312 at ~50Hz (PAL). The line count is what a receiver uses to
//      find vertical centre, so a wrong count is a rolling picture.
//
//   4. THE BROAD PULSES. During vertical sync the level must be at
//      sync tip for most of each line and return high briefly -- the
//      serration. This is the one that is easy to get subtly wrong
//      and that no static reading of the code reveals: a missing
//      serration frees the horizontal oscillator for three lines and
//      the picture tears at the top.
//
//   5. LEVELS. Only three distinct DAC values may ever appear, and
//      sync must be the lowest of them. A fourth value means a branch
//      is producing something that is not sync, blank or white.
//
//   6. ACTIVE PIXEL WIDTH -- each source pixel occupies exactly 4
//      pixel clocks, so 320 of them span the active window.
//
// Run (NTSC):
//   sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
//       rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
//   iverilog -g2005 -DGPU_COMPOSITE -o /tmp/tb_comp.out \
//       rtl/gpu/bench/tb_composite.v /tmp/gpu_video_fix.v
//   vvp /tmp/tb_comp.out
//
// For PAL, add -DTB_PAL.

module tb_composite;

`ifdef TB_PAL
	localparam H_FP = 57, H_PW = 118, H_BP = 158, H_ACT = 1280;
	localparam V_FP = 25, V_PW = 3,   V_BP = 44,  V_ACT = 240;
	localparam real LINE_US = 64.0;
	localparam integer LINES = 312;
	localparam real FIELD_HZ = 50.0;
`define STD "PAL 288p"
`else
	localparam H_FP = 65, H_PW = 118, H_BP = 139, H_ACT = 1280;
	localparam V_FP = 3,  V_PW = 3,   V_BP = 16,  V_ACT = 240;
	localparam real LINE_US = 63.5555;
	localparam integer LINES = 262;
	localparam real FIELD_HZ = 59.94;
`define STD "NTSC 240p"
`endif

	// 25.2MHz -- the SAME clock the VGA path uses. Composite needs no
	// new PLL output, which is most of why it is cheap. Half period
	// 19.8413ns.
	localparam real PCLK_HALF = 19.8413;

	reg clk = 0;
	reg pclk = 0;
	reg bclk = 0;
	reg resetn = 0;

	reg [1:0] video_mode = 2'd0;
	reg [31:0] gb_dat_i = 32'hffff_ffff;   // every pixel set -> white

	reg view_load = 0;
	reg game_en = 0, game_wrap = 0;
	reg [9:0] view_x = 0, view_y = 0;

	wire red, green, blue, hsync, vsync, is_visible;
	wire [9:0] x, y;
	wire [14:0] gb_adr_o;
	wire [3:0] dvi_p;
	wire [3:0] dac;
	wire [15:0] frame_ctr;
	wire in_vblank;

	integer errors = 0;

	always #10 clk = ~clk;
	always #PCLK_HALF pclk = ~pclk;
	always #4 bclk = ~bclk;

	gpu_video #(
		.h_disp(H_ACT), .h_front_porch(H_FP),
		.h_pulse_width(H_PW), .h_back_porch(H_BP),
		.h_line(H_FP + H_PW + H_BP + H_ACT),
		.v_disp(V_ACT), .v_front_porch(V_FP),
		.v_pulse_width(V_PW), .v_back_porch(V_BP),
		.v_frame(V_FP + V_PW + V_BP + V_ACT),
		.H_DIV_BASE(3'd4),
		.FIXED_VIEWPORT(1'b1)
	) dut (
		.pixel(1'b0), .clk(clk), .pclk(pclk), .bclk(bclk), .resetn(resetn),
		.video_mode(video_mode),
		.view_load(view_load), .game_en(game_en), .game_wrap(game_wrap),
		.view_x(view_x), .view_y(view_y),
		.frame_ctr(frame_ctr), .in_vblank(in_vblank),
		.red(red), .green(green), .blue(blue),
		.hsync(hsync), .vsync(vsync), .dvi_p(dvi_p), .dac(dac),
		.x(x), .y(y), .is_visible(is_visible),
		.gb_adr_o(gb_adr_o), .gb_dat_i(gb_dat_i)
	);

	task fail;
		input [511:0] what;
		begin
			$display("FAIL: %0s", what);
			errors = errors + 1;
		end
	endtask

	real t_sync_fall, t_prev_fall, t_sync_rise;
	real line_us, pulse_us;
	real t_field_start, field_us;
	integer line_count;
	integer i;
	integer seen_levels;
	reg [15:0] level_seen;
	integer broad_lines, serrations;
	integer active_run, px_runs, bad_runs;
	reg [3:0] prev_dac;

	initial begin

		level_seen = 0;
		broad_lines = 0;
		serrations = 0;
		bad_runs = 0;
		px_runs = 0;

		resetn = 0;
		repeat (8) @(posedge pclk);
		resetn = 1;

		// let one whole field go by so the counters are in steady state
		repeat (LINES * 400) @(posedge pclk);

		// ---- 1 & 2: line period and sync width ----
		//
		// Measured on ORDINARY lines. Waits until well clear of the
		// vertical interval first, since broad pulses have a
		// deliberately different shape.
		while (dut.vc < V_FP + V_PW + V_BP + 20) @(posedge pclk);

		@(negedge dac[2]);            // sync tip: every bit goes to 0
		t_prev_fall = $realtime;
		@(posedge dac[0]);            // back up to blanking
		t_sync_rise = $realtime;
		@(negedge dac[2]);
		t_sync_fall = $realtime;

		line_us = (t_sync_fall - t_prev_fall) / 1000.0;
		pulse_us = (t_sync_rise - t_prev_fall) / 1000.0;

		$display("%s: line %.4f us (want %.4f), sync %.3f us (want 4.700)",
			`STD, line_us, LINE_US, pulse_us);

		if (line_us < LINE_US * 0.999 || line_us > LINE_US * 1.001)
			fail("line period out of tolerance");

		if (pulse_us < 4.5 || pulse_us > 4.9)
			fail("sync pulse width out of tolerance");

		// ---- 3: field period and line count ----

		while (!(dut.vc == 0 && dut.hc == 0)) @(posedge pclk);
		t_field_start = $realtime;
		// starts at 1: we are standing ON the first line's hc==0 right
		// now, and the loop below only sees the ones after it.
		line_count = 1;
		@(posedge pclk);
		while (!(dut.vc == 0 && dut.hc == 0)) begin
			if (dut.hc == 0) line_count = line_count + 1;
			@(posedge pclk);
		end
		field_us = ($realtime - t_field_start) / 1000.0;

		$display("%s: field %.3f us = %.3f Hz over %0d lines (want %0d)",
			`STD, field_us, 1000000.0 / field_us, line_count, LINES);

		if (line_count != LINES) fail("wrong line count");
		if ((1000000.0 / field_us) < FIELD_HZ * 0.99 ||
			(1000000.0 / field_us) > FIELD_HZ * 1.02)
			fail("field rate out of tolerance");

		// ---- 4: broad pulses during vertical sync ----
		//
		// Each vsync line must sit at sync tip for most of its length
		// and return to blanking briefly at the end. Counting both
		// halves catches a missing serration, which frees the
		// receiver's horizontal oscillator and tears the top of the
		// picture -- and which reading the code does not reveal.

		while (dut.vc != 0) @(posedge pclk);
		while (dut.vc < V_FP) @(posedge pclk);

		for (i = 0; i < V_PW; i = i + 1) begin : one_broad_line
			integer low_clks, high_clks;
			low_clks = 0;
			high_clks = 0;
			while (dut.hc != 0) @(posedge pclk);
			while (dut.hc != H_FP + H_PW + H_BP + H_ACT - 1) begin
				if (dac == 4'd0) low_clks = low_clks + 1;
				else high_clks = high_clks + 1;
				@(posedge pclk);
			end
			if (low_clks > high_clks) broad_lines = broad_lines + 1;
			if (high_clks > 0) serrations = serrations + 1;
			@(posedge pclk);
		end

		$display("%s: %0d/%0d broad lines, %0d with serration",
			`STD, broad_lines, V_PW, serrations);

		if (broad_lines != V_PW) fail("vertical sync is not broad pulses");
		if (serrations != V_PW) fail("broad pulses have no serration");

		// ---- 5 & 6: levels, and pixel width ----
		//
		// Sweeps a whole line recording which DAC values appear and how
		// long each run of a constant value lasts inside active video.
		// With every framebuffer bit set the picture is solid white, so
		// runs there should be the whole active window -- the pixel
		// width check instead uses the transition out of blanking.

		while (dut.vc != V_FP + V_PW + V_BP + 50) @(posedge pclk);
		while (dut.hc != 0) @(posedge pclk);

		prev_dac = dac;
		active_run = 0;
		for (i = 0; i < H_FP + H_PW + H_BP + H_ACT; i = i + 1) begin
			level_seen[dac] = 1'b1;
			@(posedge pclk);
		end

		seen_levels = 0;
		for (i = 0; i < 16; i = i + 1)
			if (level_seen[i]) seen_levels = seen_levels + 1;

		$display("%s: %0d distinct DAC levels used", `STD, seen_levels);

		if (seen_levels != 3) fail("expected exactly 3 DAC levels");
		if (!level_seen[0]) fail("sync tip (0) never driven");
		if (!level_seen[5]) fail("blanking level (5) never driven");
		if (!level_seen[15]) fail("white level (15) never driven");

		// pixel width: x must advance exactly once every 4 pclk inside
		// active video
		while (dut.hc != H_FP + H_PW + H_BP + 8) @(posedge pclk);
		begin : pixel_width
			integer x0, held;
			x0 = x;
			held = 0;
			while (x == x0) begin
				held = held + 1;
				@(posedge pclk);
			end
			$display("%s: source pixel held for %0d pixel clocks", `STD, held);
			if (held != 4) fail("source pixel is not 4 pixel clocks wide");
		end

		if (errors == 0) $display("RESULT: PASS");
		else $display("RESULT: FAIL (%0d errors)", errors);

		$finish;

	end

endmodule
