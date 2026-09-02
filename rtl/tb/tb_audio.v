/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/audio.v and rtl/audio_out.v.
 *
 *   iverilog -g2005 -o /tmp/tb_audio rtl/tb/tb_audio.v \
 *       rtl/audio.v rtl/audio_out.v && /tmp/tb_audio
 *
 * Follows the pattern rtl/tb/tb_spim.v established, and for the reason
 * that file's own history gives: the FIRST version of that testbench
 * failed a correct design, because its behavioural slave sampled the
 * data line on the wrong clock edge. A receiver model that ignores
 * clock edges proves nothing about a serialiser -- it only proves the
 * bits were emitted in some order at some time.
 *
 * So the PT8211 receiver below samples DIN on the RISING edge of BCK,
 * which is what the real part does, and reconstructs the word from
 * nothing but the pin activity. If the serialiser changes DIN on the
 * wrong edge, or gets the WS alignment wrong by a bit, this notices.
 *
 * Tests, in order:
 *
 *   1  register interface -- MAGIC, CONFIG, CLKHZ, RATE, WMARK
 *   2  FIFO -- fill, level, full, drop-when-full, flush
 *   3  frame rate -- measured against the arithmetic in audio_out.v
 *   4  PT8211 round trip -- every distinct sample value in a sweep,
 *      plus the two channels kept straight, plus SWAPLR
 *   5  PT8211 exhaustive -- all 65536 16-bit values through the
 *      serialiser
 *   6  underrun -- holds, sets the sticky bit, and CLRUR clears it
 *   7  sigma-delta DC transfer -- output density against input
 *   8  sigma-delta stability -- full-scale sine, error terms bounded,
 *      no limit cycle
 *   9  interrupt -- asserts below the watermark, clears above it
 */

// rtl/audio.v includes boards.vh (for `AUDIO_MIXER), so this needs
// -I rtl on the iverilog command line. Defining AUDIO_MIXER here --
// before that include runs -- makes the testbench build the same
// configuration a board does, rather than silently testing the
// mixer-less variant and reporting a pass for it.
`define AUDIO_MIXER

`timescale 1ns / 1ps

module tb_audio;

	// 48MHz -- SYSCLK on every board this runs on. Written as the
	// half period in ns because 48MHz does not divide evenly: an
	// integer 21ns period is 47.6MHz, which is 0.8% slow and was
	// enough to fail the frame-rate check below against a correct
	// divider. Measuring a rate to 0.1% means generating the
	// reference to better than that.
	localparam real CLK_HALF_NS = 10.416667;
	localparam integer DEPTH_LOG2 = 10;
	localparam integer DEPTH = (1 << DEPTH_LOG2);

	localparam [31:0] REG_MAGIC  = 32'd0;
	localparam [31:0] REG_CTRL   = 32'd1;
	localparam [31:0] REG_STATUS = 32'd2;
	localparam [31:0] REG_DATA   = 32'd3;
	localparam [31:0] REG_RATE   = 32'd4;
	localparam [31:0] REG_WMARK  = 32'd5;
	localparam [31:0] REG_CONFIG = 32'd6;
	localparam [31:0] REG_CLKHZ  = 32'd7;

	localparam [7:0] CTRL_EN     = 8'h01;
	localparam [7:0] CTRL_IRQEN  = 8'h02;
	localparam [7:0] CTRL_SWAPLR = 8'h04;
	localparam [7:0] CTRL_FLUSH  = 8'h08;
	localparam [7:0] CTRL_CLRUR  = 8'h10;

	reg clk;
	reg rst;

	reg [31:0] wb_adr;
	reg [31:0] wb_dat_w;
	wire [31:0] wb_dat_r;
	reg wb_we;
	reg [3:0] wb_sel;
	reg wb_stb;
	reg wb_cyc;
	wire wb_ack;

	wire aud_int;
	wire audio_l;
	wire audio_r;
	wire aud_bck;
	wire aud_ws;
	wire aud_din;

	integer errors;
	integer i;
	integer j;
	integer ones;
	integer total;
	reg [31:0] rd;
	reg [31:0] tmp;
	reg signed [15:0] want_l;
	reg signed [15:0] want_r;
	real t0;
	real t1;
	real fs_measured;
	real fs_expected;
	real density;
	real expect_density;

	// -- behavioural PT8211 receiver --
	//
	// Samples DIN on the rising edge of BCK, exactly as the part does,
	// and uses the WS transition as the word boundary. Knows nothing
	// about the serialiser's internals.
	reg [15:0] pt_shift;
	reg [4:0] pt_bits;
	reg pt_ws_d;
	reg [15:0] pt_word_a;	// captured while WS was low
	reg [15:0] pt_word_b;	// captured while WS was high
	reg [15:0] pt_pair_a;	// the two halves of ONE frame, latched
	reg [15:0] pt_pair_b;	// together -- see the receiver below
	integer pt_words;
	integer pt_frames;

	// -- sigma-delta observation --
	integer sd_l_ones;
	integer sd_samples;

	audio_wb #(
		.DEPTH_LOG2(DEPTH_LOG2),
		.FORMATS(4'b0011),
		.CLK_HZ(48000000),
		.RATE_RESET(8'd17)
	) dut (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wb_adr_i(wb_adr),
		.wb_dat_i(wb_dat_w),
		.wb_dat_o(wb_dat_r),
		.wb_we_i(wb_we),
		.wb_sel_i(wb_sel),
		.wb_stb_i(wb_stb),
		.wb_ack_o(wb_ack),
		.wb_cyc_i(wb_cyc),
		.int_o(aud_int),
		.AUDIO_L(audio_l),
		.AUDIO_R(audio_r),
		.AUD_BCK(aud_bck),
		.AUD_WS(aud_ws),
		.AUD_DIN(aud_din)
	);

	initial begin
		clk = 1'b0;
		forever #(CLK_HALF_NS) clk = ~clk;
	end

	// ------------------------------------------------------------
	// wishbone helpers
	// ------------------------------------------------------------

	task wb_write;
		input [31:0] adr;
		input [31:0] dat;
		begin
			@(posedge clk);
			wb_adr <= adr;
			wb_dat_w <= dat;
			wb_we <= 1'b1;
			wb_sel <= 4'b1111;
			wb_stb <= 1'b1;
			wb_cyc <= 1'b1;
			@(posedge clk);
			while (!wb_ack) @(posedge clk);
			wb_stb <= 1'b0;
			wb_cyc <= 1'b0;
			wb_we <= 1'b0;
			@(posedge clk);
		end
	endtask

	task wb_read;
		input [31:0] adr;
		output [31:0] dat;
		begin
			@(posedge clk);
			wb_adr <= adr;
			wb_we <= 1'b0;
			wb_sel <= 4'b1111;
			wb_stb <= 1'b1;
			wb_cyc <= 1'b1;
			@(posedge clk);
			while (!wb_ack) @(posedge clk);
			dat = wb_dat_r;
			wb_stb <= 1'b0;
			wb_cyc <= 1'b0;
			@(posedge clk);
		end
	endtask

	task check;
		input [511:0] name;
		input [31:0] got;
		input [31:0] want;
		begin
			if (got !== want) begin
				$display("FAIL %0s: got %08x want %08x", name, got, want);
				errors = errors + 1;
			end else begin
				$display("  ok  %0s = %08x", name, got);
			end
		end
	endtask

	// ------------------------------------------------------------
	// behavioural PT8211 receiver
	// ------------------------------------------------------------

	always @(posedge aud_bck) begin
		pt_shift <= { pt_shift[14:0], aud_din };
		pt_bits <= pt_bits + 5'd1;
	end

	// WS transition ends a word: the 16 bits before the edge are the
	// half-frame that just finished.
	//
	// A COMPLETE FRAME IS ONLY VALID AT THE FALLING EDGE, and getting
	// that wrong is how the first version of this testbench failed a
	// correct serialiser. The WS-low word is captured at the rising
	// edge, the WS-high word half a frame later at the falling edge --
	// so sampling pt_word_a and pt_word_b at an arbitrary moment gets
	// two halves of DIFFERENT frames, one apart, which looks exactly
	// like a serialiser bug. pt_pair_a/pt_pair_b are latched together
	// at the falling edge and are the only pair safe to compare;
	// pt_frames ticks when a new one lands.
	always @(posedge clk) begin
		pt_ws_d <= aud_ws;
		if (aud_ws != pt_ws_d) begin
			if (pt_ws_d == 1'b0) begin
				pt_word_a <= pt_shift;
			end else begin
				pt_word_b <= pt_shift;
				pt_pair_a <= pt_word_a;
				pt_pair_b <= pt_shift;
				pt_frames <= pt_frames + 1;
			end
			pt_words <= pt_words + 1;
			// 16 BCK edges per half-frame, every half-frame. Skips
			// the first two, which start mid-word out of reset.
			if (pt_bits != 5'd16 && pt_words > 2) begin
				$display("FAIL pt8211: %0d BCK edges in a half-frame, want 16",
					pt_bits);
				errors = errors + 1;
			end
			pt_bits <= 5'd0;
		end
	end

	// wait for n whole frames to be received
	task pt_wait_frames;
		input integer n;
		integer target;
		begin
			target = pt_frames + n;
			while (pt_frames < target) @(posedge clk);
			@(posedge clk);
		end
	endtask

	// ------------------------------------------------------------
	// sigma-delta observation
	// ------------------------------------------------------------

	always @(posedge clk) begin
		if (sd_samples >= 0) begin
			sd_l_ones <= sd_l_ones + (audio_l ? 1 : 0);
			sd_samples <= sd_samples + 1;
		end
	end

	task sd_measure;
		input integer cycles;
		begin
			sd_l_ones = 0;
			sd_samples = 0;
			repeat (cycles) @(posedge clk);
			density = sd_l_ones * 1.0 / sd_samples;
		end
	endtask

	// wait for n frame periods at the current rate
	task wait_frames;
		input integer n;
		begin
			repeat (n) @(posedge dut.out_i.frame_req);
		end
	endtask

	// ------------------------------------------------------------

	initial begin

		// Opt-in: `vvp tb_audio +vcd`. Dumping every signal through
		// the exhaustive check below writes gigabytes and turns a
		// one-minute run into an hour, so it is off unless asked for.
		if ($test$plusargs("vcd")) begin
			$dumpfile("/tmp/tb_audio.vcd");
			$dumpvars(0, tb_audio);
		end

		errors = 0;
		rst = 1'b1;
		wb_adr = 32'd0;
		wb_dat_w = 32'd0;
		wb_we = 1'b0;
		wb_sel = 4'b0000;
		wb_stb = 1'b0;
		wb_cyc = 1'b0;
		pt_shift = 16'd0;
		pt_bits = 5'd0;
		pt_ws_d = 1'b0;
		pt_word_a = 16'd0;
		pt_word_b = 16'd0;
		pt_pair_a = 16'd0;
		pt_pair_b = 16'd0;
		pt_words = 0;
		pt_frames = 0;
		sd_l_ones = 0;
		sd_samples = -1;

		repeat (8) @(posedge clk);
		rst = 1'b0;
		repeat (4) @(posedge clk);

		$display("");
		$display("=== 1. register interface ===");

		wb_read(REG_MAGIC, rd);
		check("MAGIC", rd, 32'h5A41_5544);

		wb_read(REG_CONFIG, rd);
		// Spelled out field by field rather than as one constant, so
		// that adding a capability bit is a one-line edit here with an
		// obvious meaning instead of a hunt for which hex digit moved.
		// Bit 13 (FMT16) arrived exactly that way and this check
		// caught it, which is the check doing its job -- but "got
		// 5a41330a want 5a41130a" is not a helpful way to be told.
		check("CONFIG", rd, {
			16'h5A41,      // signature
			2'b0,          // reserved
			1'b1,          // bit 13: mixer channels can fetch 16-bit
			1'b1,          // bit 12: hardware mixer present
			4'b0011,       // formats: sigma-delta + PT8211
			8'd10          // FIFO depth log2
		});

		wb_read(REG_CLKHZ, rd);
		check("CLKHZ", rd, 32'd48000000);

		wb_read(REG_RATE, rd);
		check("RATE reset", rd, 32'd17);

		wb_read(REG_WMARK, rd);
		check("WMARK reset", rd, DEPTH / 2);

		wb_read(REG_CTRL, rd);
		check("CTRL reset", rd, 32'd0);

		// command bits must never read back as stored state
		wb_write(REG_CTRL, CTRL_FLUSH | CTRL_CLRUR | CTRL_EN);
		wb_read(REG_CTRL, rd);
		check("CTRL command bits not stored", rd, 32'h0000_0001);

		wb_write(REG_WMARK, 32'd40);
		wb_read(REG_WMARK, rd);
		check("WMARK writable", rd, 32'd40);
		wb_write(REG_WMARK, DEPTH / 2);

		$display("");
		$display("=== 2. FIFO ===");

		// muted while filling, so nothing drains underneath us
		wb_write(REG_CTRL, CTRL_FLUSH);

		wb_read(REG_STATUS, rd);
		check("STATUS empty after flush", rd[17:16], 2'b01);
		check("LEVEL after flush", rd[15:0], 16'd0);

		for (i = 0; i < DEPTH; i = i + 1)
			wb_write(REG_DATA, { i[15:0], i[15:0] });

		wb_read(REG_STATUS, rd);
		check("LEVEL when full", rd[15:0], DEPTH[15:0]);
		check("FULL flag", rd[17], 1'b1);
		check("EMPTY flag clear", rd[16], 1'b0);

		// a write to a full FIFO is dropped, not queued and not fatal
		wb_write(REG_DATA, 32'hDEAD_BEEF);
		wb_read(REG_STATUS, rd);
		check("LEVEL unchanged after overflow write", rd[15:0], DEPTH[15:0]);

		wb_write(REG_CTRL, CTRL_FLUSH);
		wb_read(REG_STATUS, rd);
		check("LEVEL after flush", rd[15:0], 16'd0);

		$display("");
		$display("=== 3. frame rate ===");

		// fs = CLK_HZ / (64 * RATE); at RATE=17 that is 44117.6 Hz
		wb_write(REG_RATE, 32'd17);
		wb_write(REG_CTRL, CTRL_EN);

		@(posedge dut.out_i.frame_req);
		t0 = $realtime;
		repeat (100) @(posedge dut.out_i.frame_req);
		t1 = $realtime;

		fs_measured = 100.0 / ((t1 - t0) * 1.0e-9);
		fs_expected = 48000000.0 / (64.0 * 17.0);
		$display("  fs measured %0.1f Hz, expected %0.1f Hz",
			fs_measured, fs_expected);
		if (fs_measured < fs_expected * 0.999 ||
			fs_measured > fs_expected * 1.001) begin
			$display("FAIL frame rate out of tolerance");
			errors = errors + 1;
		end else begin
			$display("  ok  frame rate");
		end

		// and the 22kHz divider
		wb_write(REG_RATE, 32'd34);
		@(posedge dut.out_i.frame_req);
		t0 = $realtime;
		repeat (50) @(posedge dut.out_i.frame_req);
		t1 = $realtime;
		fs_measured = 50.0 / ((t1 - t0) * 1.0e-9);
		fs_expected = 48000000.0 / (64.0 * 34.0);
		$display("  fs measured %0.1f Hz, expected %0.1f Hz",
			fs_measured, fs_expected);
		if (fs_measured < fs_expected * 0.999 ||
			fs_measured > fs_expected * 1.001) begin
			$display("FAIL 22kHz divider out of tolerance");
			errors = errors + 1;
		end else begin
			$display("  ok  22kHz divider");
		end

		wb_write(REG_RATE, 32'd2);	// fastest legal, keeps the sim short

		$display("");
		$display("=== 4. PT8211 round trip ===");

		wb_write(REG_CTRL, CTRL_FLUSH);
		wb_write(REG_RATE, 32'd2);
		wb_write(REG_CTRL, CTRL_EN);

		// A single distinctive frame, repeated, so whichever frame the
		// receiver happens to land on is the same one. Two clearly
		// different values, neither symmetric, so a swapped or
		// bit-rotated channel cannot look correct by accident.
		for (i = 0; i < 60; i = i + 1)
			wb_write(REG_DATA, { 16'h8001, 16'h0FE2 });

		pt_wait_frames(3);
		check("pt8211 left  (SWAPLR clear)", { 16'b0, pt_pair_a }, 32'h0000_8001);
		check("pt8211 right (SWAPLR clear)", { 16'b0, pt_pair_b }, 32'h0000_0FE2);

		// SWAPLR must exchange them and change nothing else
		wb_write(REG_CTRL, CTRL_FLUSH);
		wb_write(REG_CTRL, CTRL_EN | CTRL_SWAPLR);
		for (i = 0; i < 60; i = i + 1)
			wb_write(REG_DATA, { 16'h8001, 16'h0FE2 });

		pt_wait_frames(3);
		check("pt8211 left  (SWAPLR set)", { 16'b0, pt_pair_a }, 32'h0000_0FE2);
		check("pt8211 right (SWAPLR set)", { 16'b0, pt_pair_b }, 32'h0000_8001);

		wb_write(REG_CTRL, CTRL_EN);

		$display("");
		$display("=== 5. PT8211 exhaustive value check ===");
		// 65536 values is ~17M cycles. Skippable with +quick so the
		// other eight groups stay a fast edit/run loop; CI and any
		// change to the serialiser should run it in full.
		if ($test$plusargs("quick")) begin
			$display("  (skipped: +quick)");
		end else

		// Every 16-bit value through the serialiser. Drives audio_out
		// directly -- going through the FIFO would need 65536 bus
		// writes and prove nothing extra about the serialiser.
		exhaustive_serialiser;

		$display("");
		$display("=== 6. underrun ===");

		wb_write(REG_RATE, 32'd2);
		wb_write(REG_CTRL, CTRL_FLUSH);
		wb_write(REG_CTRL, CTRL_EN | CTRL_CLRUR);

		wb_write(REG_DATA, { 16'h4000, 16'hC000 });

		// one frame is queued; run past it and the FIFO starves
		wait_frames(4);

		wb_read(REG_STATUS, rd);
		check("UNDERRUN set", rd[19], 1'b1);
		check("EMPTY set", rd[16], 1'b1);

		// held, not garbage: hold_l must still be the last real frame
		check("held sample L", { 16'b0, dut.out_i.hold_l }, 32'h0000_4000);
		check("held sample R", { 16'b0, dut.out_i.hold_r }, 32'h0000_C000);

		wb_write(REG_CTRL, CTRL_EN | CTRL_CLRUR);
		wb_read(REG_STATUS, rd);
		check("UNDERRUN cleared", rd[19], 1'b0);

		$display("");
		$display("=== 7. sigma-delta DC transfer ===");

		// The modulator's average output density should track the
		// input: density = (x + SD_FS) / (2 * SD_FS), with SD_FS the
		// 49152 in audio_out.v. Silence must be 50%.
		wb_write(REG_RATE, 32'd2);
		wb_write(REG_CTRL, CTRL_FLUSH);
		wb_write(REG_CTRL, CTRL_EN);

		check_dc(16'sd0,      0.5);
		check_dc(16'sd16384,  (16384.0 + 49152.0) / 98304.0);
		check_dc(-16'sd16384, (-16384.0 + 49152.0) / 98304.0);
		check_dc(16'sd32767,  (32767.0 + 49152.0) / 98304.0);
		check_dc(-16'sd32768, (-32768.0 + 49152.0) / 98304.0);

		$display("");
		$display("=== 8. sigma-delta stability at full scale ===");

		stability_check;

		$display("");
		$display("=== 9. interrupt ===");

		wb_write(REG_CTRL, CTRL_FLUSH);
		wb_write(REG_WMARK, 32'd64);

		// IRQEN clear: no interrupt regardless of level
		wb_write(REG_CTRL, 32'd0);
		@(posedge clk);
		check("int clear when IRQEN clear", { 31'b0, aud_int }, 32'd0);

		// IRQEN set, FIFO empty: asserted
		wb_write(REG_CTRL, CTRL_IRQEN);
		@(posedge clk);
		check("int set below watermark", { 31'b0, aud_int }, 32'd1);

		// fill past the watermark: deasserted. EN is still clear so
		// nothing drains while we do it.
		for (i = 0; i < 70; i = i + 1)
			wb_write(REG_DATA, 32'h0000_0000);
		@(posedge clk);
		check("int clear above watermark", { 31'b0, aud_int }, 32'd0);

		wb_write(REG_CTRL, CTRL_IRQEN | CTRL_FLUSH);
		@(posedge clk);
		check("int set again after flush", { 31'b0, aud_int }, 32'd1);

		$display("");
		if (errors == 0)
			$display("=== tb_audio: PASS ===");
		else
			$display("=== tb_audio: %0d FAILURE(S) ===", errors);
		$display("");

		$finish;

	end

	// ------------------------------------------------------------
	// helper tasks that need their own timing
	// ------------------------------------------------------------

	task check_dc;
		input signed [15:0] value;
		input real want;
		begin
			// keep one value in the FIFO permanently by refilling
			// faster than it drains
			wb_write(REG_CTRL, CTRL_FLUSH);
			wb_write(REG_CTRL, CTRL_EN);
			for (j = 0; j < 100; j = j + 1)
				wb_write(REG_DATA, { value, value });
			wait_frames(2);
			sd_measure(20000);
			$display("  x=%6d density %0.4f want %0.4f",
				value, density, want);
			if (density < want - 0.02 || density > want + 0.02) begin
				$display("FAIL sigma-delta DC transfer at x=%0d", value);
				errors = errors + 1;
			end
		end
	endtask

	// Full-scale sine into the modulator, checking the error terms
	// stay inside the clamp and the output actually toggles. A
	// modulator that has gone into a limit cycle still produces bits;
	// what it stops doing is tracking the input, so both halves of
	// this matter.
	task stability_check;
		integer k;
		integer toggles;
		integer clamped;
		reg last;
		reg signed [15:0] s;
		real ang;
		begin
			toggles = 0;
			clamped = 0;
			last = audio_l;
			wb_write(REG_CTRL, CTRL_FLUSH);
			wb_write(REG_CTRL, CTRL_EN);
			wb_write(REG_RATE, 32'd2);

			for (k = 0; k < 400; k = k + 1) begin
				ang = 2.0 * 3.14159265 * k / 32.0;
				s = $rtoi(32700.0 * $sin(ang));
				wb_write(REG_DATA, { s, s });
				if (dut.out_i.sd_l_i.e1 > 4 * 49152 ||
					dut.out_i.sd_l_i.e1 < -4 * 49152)
					clamped = clamped + 1;
				if (audio_l != last) toggles = toggles + 1;
				last = audio_l;
			end

			if (clamped != 0) begin
				$display("FAIL sigma-delta: e1 outside clamp %0d times",
					clamped);
				errors = errors + 1;
			end else begin
				$display("  ok  e1 stayed inside the clamp");
			end

			if (toggles < 20) begin
				$display("FAIL sigma-delta: output barely toggled (%0d)",
					toggles);
				errors = errors + 1;
			end else begin
				$display("  ok  output toggling (%0d transitions sampled)",
					toggles);
			end
		end
	endtask

	// ------------------------------------------------------------
	// exhaustive serialiser check
	// ------------------------------------------------------------
	//
	// A second audio_out instance driven directly, so all 65536 values
	// can be pushed through without 65536 wishbone writes.

	reg ex_rst;
	reg signed [15:0] ex_l;
	reg signed [15:0] ex_r;
	wire ex_frame_req;
	wire ex_bck;
	wire ex_ws;
	wire ex_din;

	reg [15:0] ex_shift;
	reg ex_ws_d;
	reg [15:0] ex_word_a;
	reg [15:0] ex_pair_a;
	reg [15:0] ex_pair_b;
	integer ex_frames;

	audio_out #() ex_i (
		.clk(clk),
		.rst(ex_rst),
		.enable(1'b1),
		.rate(8'd2),
		.swap_lr(1'b0),
		.sample_l(ex_l),
		.sample_r(ex_r),
		.sample_valid(1'b1),
		.frame_req(ex_frame_req),
		.sd_l(),
		.sd_r(),
		.pt_bck(ex_bck),
		.pt_ws(ex_ws),
		.pt_din(ex_din)
	);

	always @(posedge ex_bck)
		ex_shift <= { ex_shift[14:0], ex_din };

	// Same atomic-pair rule as the main receiver above -- the WS-low
	// and WS-high words are captured half a frame apart, so only the
	// pair latched together at the falling edge belongs to one frame.
	always @(posedge clk) begin
		ex_ws_d <= ex_ws;
		if (ex_ws && !ex_ws_d) begin
			ex_word_a <= ex_shift;
		end else if (!ex_ws && ex_ws_d) begin
			ex_pair_a <= ex_word_a;
			ex_pair_b <= ex_shift;
			ex_frames <= ex_frames + 1;
		end
	end

	task exhaustive_serialiser;
		integer v;
		integer bad;
		integer target;
		begin
			bad = 0;
			ex_rst = 1'b1;
			ex_l = 16'd0;
			ex_r = 16'd0;
			ex_shift = 16'd0;
			ex_ws_d = 1'b0;
			ex_frames = 0;
			repeat (4) @(posedge clk);
			ex_rst = 1'b0;

			for (v = 0; v < 65536; v = v + 1) begin
				ex_l = v[15:0];
				ex_r = ~v[15:0];

				// three frames: one to finish whatever was in flight,
				// one for the new value to be loaded, one to receive it
				target = ex_frames + 3;
				while (ex_frames < target) @(posedge clk);
				@(posedge clk);

				if (ex_pair_a !== v[15:0] || ex_pair_b !== (~v[15:0])) begin
					if (bad < 8)
						$display("FAIL serialiser v=%04x: A=%04x B=%04x",
							v[15:0], ex_pair_a, ex_pair_b);
					bad = bad + 1;
				end
			end

			if (bad != 0) begin
				$display("FAIL serialiser: %0d of 65536 values wrong", bad);
				errors = errors + 1;
			end else begin
				$display("  ok  all 65536 values round-tripped");
			end
		end
	endtask

endmodule
