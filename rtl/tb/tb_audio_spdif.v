/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/audio_spdif.v.
 *
 *   iverilog -g2005 -o /tmp/tb_spdif rtl/tb/tb_audio_spdif.v \
 *       rtl/audio_spdif.v
 *   vvp /tmp/tb_spdif
 *
 * The model below is a RECEIVER, not a bit comparator: it measures
 * half-cell widths off the line, finds preambles by their biphase
 * violation, decodes biphase-mark back into bits, and reconstructs the
 * sample, parity and channel status. It knows nothing about the
 * transmitter's internals.
 *
 * That is the only kind of check worth writing here. The first version
 * of this encoder had its half-cell index off by one -- it emitted
 * cell hc when the value it assigned was held for cell hc+1 -- and
 * every bit was still "there", just half a cell out of position. A
 * comparator against an expected bit vector would have passed it. A
 * receiver does not: the bit-cell boundaries land in the wrong place
 * and nothing decodes, which is exactly what a real DAC would have
 * done with it.
 *
 * Same lesson as rtl/tb/tb_spim.v's slave model, and tb_audio.v's
 * PT8211 receiver.
 */

`timescale 1ns / 1ps

module tb_audio_spdif;

	localparam real CLK_HALF_NS = 10.416667;   // 48MHz
	parameter integer RATE = 16;               // 46875 Hz, half-cell = 8

	reg clk = 0;
	reg rst = 1;
	reg enable = 1;
	reg frame_req = 0;
	reg signed [15:0] sample_l = 0;
	reg signed [15:0] sample_r = 0;
	reg [3:0] fs_code = 4'b0100;               // "48 kHz"
	wire spdif;

	integer errors = 0;
	integer i;

	always #(CLK_HALF_NS) clk = ~clk;

	audio_spdif dut (
		.clk(clk), .rst(rst), .enable(enable),
		.rate(RATE[7:0]), .frame_req(frame_req),
		.sample_l(sample_l), .sample_r(sample_r),
		.fs_code(fs_code), .spdif(spdif)
	);

	// ------------------------------------------------------------
	// frame generator: one frame_req every 64*RATE cycles
	// ------------------------------------------------------------
	integer fcount = 0;

	initial begin
		@(negedge rst);
		forever begin
			// EXACTLY 64*RATE cycles per frame, pulse included. The
			// first version added two more for the pulse itself, so
			// the generator ran 1026 cycles against the transmitter's
			// 1024 and the two slowly drifted apart -- which showed up
			// as samples being latched mid-subframe.
			repeat ((64 * RATE) - 1) @(posedge clk);
			frame_req <= 1'b1;
			@(posedge clk);
			frame_req <= 1'b0;
			fcount = fcount + 1;
		end
	end

	// ------------------------------------------------------------
	// receiver
	// ------------------------------------------------------------
	//
	// Samples the line once per half-cell, phase-locked by counting
	// clocks -- a real receiver PLLs, but the point here is to decode
	// from the WIRE rather than from the transmitter's state.

	reg [7:0] rx_hist;          // last 8 half-cell levels, MSB oldest
	reg [7:0] rx_cnt;
	reg rx_armed;

	integer rx_bitpos;          // slot within the subframe, 4..31
	reg [27:0] rx_bits;         // slots 4..31 as decoded
	reg rx_half;                // which half of the bit cell we are in
	reg rx_prev;
	reg rx_insub;

	// what the receiver has recovered
	reg [15:0] rx_audio;
	reg rx_parity_ok;
	reg [1:0] rx_pre;           // 1=Z, 2=X, 3=Y
	integer rx_subframes;
	integer rx_z, rx_x, rx_y;
	integer rx_parity_bad;

	reg [15:0] got_l, got_r;
	reg got_l_v, got_r_v;
	reg [191:0] rx_cs;
	integer rx_csidx;

	task rx_reset;
		begin
			rx_hist = 0; rx_cnt = 0; rx_armed = 0;
			rx_bitpos = 0; rx_bits = 0; rx_half = 0; rx_prev = 0;
			rx_insub = 0; rx_subframes = 0;
			rx_z = 0; rx_x = 0; rx_y = 0; rx_parity_bad = 0;
			got_l_v = 0; got_r_v = 0; rx_csidx = 0; rx_cs = 0;
		end
	endtask

	// half-cell strobe: RATE/2 clocks, aligned by the first edge after
	// reset and then free-running, exactly like the transmitter's own
	// divider
	// The receiver must recover the half-cell clock the same way a real
	// one does -- from the line -- because at an odd RATE the
	// transmitter's half-cells are not all the same length. A fixed
	// divider here would only work for even rates and would hide
	// exactly the bug this file exists to catch.
	reg [15:0] rx_acc;
	wire hc_stb = (rx_acc >= (64 * RATE));

	always @(posedge clk) begin
		if (rst) begin
			rx_acc <= 0;
		end else begin
			rx_acc <= hc_stb ? (rx_acc - (64 * RATE) + 128) : (rx_acc + 128);

			if (hc_stb) begin
				rx_hist <= { rx_hist[6:0], spdif };

				// A preamble is three half-cells at one level then a
				// transition -- a run of 3 is impossible under
				// biphase-mark, where the longest run is 2. That
				// violation is how a receiver finds frame boundaries,
				// and detecting it here is the point.
				if (rx_hist[2:0] == 3'b000 || rx_hist[2:0] == 3'b111) begin
					// candidate: 8 half-cells ago started a preamble
					rx_armed <= 1'b1;
				end
			end
		end
	end

	// Decode by pattern-matching the 8 half-cells of a preamble against
	// the three legal shapes, in either polarity.
	function [1:0] pre_kind;
		input [7:0] p;
		begin
			if (p == 8'b11101000 || p == 8'b00010111) pre_kind = 2'd1; // Z
			else if (p == 8'b11100010 || p == 8'b00011101) pre_kind = 2'd2; // X
			else if (p == 8'b11100100 || p == 8'b00011011) pre_kind = 2'd3; // Y
			else pre_kind = 2'd0;
		end
	endfunction

	// ------------------------------------------------------------
	// The decoder proper: walks the line one half-cell at a time.
	// ------------------------------------------------------------
	integer state;              // 0 = hunting, 1 = in preamble, 2 = data
	integer hcn;
	reg [7:0] prebuf;
	reg [1:0] kind;
	integer hunt;
	reg locked;
	reg cs_armed;
	reg lastlev;
	integer bitn;
	reg [27:0] payload;
	reg curbit;

	initial begin
		rx_reset;
		state = 0; hcn = 0; prebuf = 0; bitn = 0; payload = 0;
		lastlev = 0; hunt = 0; locked = 0; cs_armed = 0;

		@(negedge rst);

		forever begin
			@(posedge clk);
			if (hc_stb) begin

				case (state)

				0: begin   // hunting for a preamble
					prebuf = { prebuf[6:0], spdif };
					hunt = hunt + 1;

					// Once locked, a preamble starts exactly 8
					// half-cells after a subframe ends, so only test
					// there. Testing every shift lets a pattern with
					// leading zeros match early against the zeros
					// prebuf was cleared to -- 00010111 (an inverted
					// Z) matches after five shifts of 10111 -- and the
					// decoder desyncs mid-subframe. That was a bug in
					// this model, not in the transmitter.
					kind = (locked && hunt < 8) ? 2'd0 : pre_kind(prebuf);

					if (kind != 2'd0) begin
						locked = 1;
						hunt = 0;
						state = 2;
						bitn = 0;
						payload = 0;
						lastlev = spdif;
						if (kind == 1) rx_z = rx_z + 1;
						if (kind == 2) rx_x = rx_x + 1;
						if (kind == 3) rx_y = rx_y + 1;
						rx_pre = kind;
					end
				end

				2: begin   // 28 bits of biphase-mark, 2 half-cells each
					if (bitn[0] == 0) begin
						// first half: must transition
						if (spdif == lastlev) begin
							$display("FAIL biphase: no transition at cell boundary (bit %0d)",
								bitn >> 1);
							errors = errors + 1;
						end
						lastlev = spdif;
					end else begin
						// second half: transition means 1
						curbit = (spdif != lastlev);
						payload[bitn >> 1] = curbit;
						lastlev = spdif;
					end
					bitn = bitn + 1;

					if (bitn == 56) begin
						// subframe complete
						rx_subframes = rx_subframes + 1;

						if (^payload !== 1'b0) begin
							rx_parity_bad = rx_parity_bad + 1;
						end

						// audio is slots 8..27, top 16 of the 20-bit
						// field -> payload bits 8..23
						if (rx_pre == 3) begin
							got_r = payload[23:8]; got_r_v = 1;
						end else begin
							got_l = payload[23:8]; got_l_v = 1;
							// A Z preamble is block frame 0. Only
							// start recording channel status there --
							// capturing from an arbitrary point fills
							// the array with bits from the wrong frame
							// indices, and a later block only
							// overwrites the ones it reaches.
							if (rx_pre == 1) begin
								rx_csidx = 0;
								cs_armed = 1;
							end
						end

						// channel status bit is slot 30 = payload[26]
						if (rx_pre != 3 && cs_armed) begin
							if (rx_csidx < 192) rx_cs[rx_csidx] = payload[26];
							rx_csidx = rx_csidx + 1;
						end

						state = 0;
						prebuf = 0;
						hunt = 0;
					end
				end

				default: state = 0;

				endcase
			end
		end
	end

	task check;
		input [511:0] name;
		input integer got;
		input integer want;
		begin
			if (got !== want) begin
				$display("FAIL %0s: got %0d want %0d", name, got, want);
				errors = errors + 1;
			end else begin
				$display("  ok  %0s = %0d", name, got);
			end
		end
	endtask

	task play;
		input signed [15:0] l;
		input signed [15:0] r;
		input integer frames;
		begin
			sample_l = l; sample_r = r;
			repeat (frames) @(posedge frame_req);
			repeat (64 * RATE) @(posedge clk);
		end
	endtask

	initial begin
		if ($test$plusargs("vcd")) begin
			$dumpfile("/tmp/tb_audio_spdif.vcd");
			$dumpvars(0, tb_audio_spdif);
		end

		repeat (8) @(posedge clk);
		rst = 0;

		$display("");
		$display("=== 1. framing ===");

		play(16'sh1234, 16'sh5678, 12);

		$display("  preambles seen: Z=%0d X=%0d Y=%0d, %0d subframes",
			rx_z, rx_x, rx_y, rx_subframes);
		if (rx_subframes < 16) begin
			$display("FAIL receiver never locked -- no subframes decoded");
			errors = errors + 1;
		end else begin
			$display("  ok  receiver locked");
		end
		check("parity errors", rx_parity_bad, 0);
		// Off by one is just the window ending mid-frame.
		if ((rx_x + rx_z) - rx_y > 1 || rx_y - (rx_x + rx_z) > 1) begin
			$display("FAIL subframe A/B counts differ: A=%0d B=%0d",
				rx_x + rx_z, rx_y);
			errors = errors + 1;
		end else begin
			$display("  ok  one A subframe per B subframe");
		end

		$display("");
		$display("=== 2. audio payload round trip ===");

		play(16'sh1234, 16'sh5678, 8);
		check("left  = 0x1234", got_l, 16'h1234);
		check("right = 0x5678", got_r, 16'h5678);

		play(16'shFFFF, 16'sh0001, 8);
		check("left  = 0xFFFF", got_l, 16'hFFFF);
		check("right = 0x0001", got_r, 16'h0001);

		play(16'sh8000, 16'sh7FFF, 8);
		check("left  = 0x8000 (most negative)", got_l, 16'h8000);
		check("right = 0x7FFF (most positive)", got_r, 16'h7FFF);

		play(16'sh0000, 16'sh0000, 8);
		check("left  = 0x0000", got_l, 16'h0000);
		check("right = 0x0000", got_r, 16'h0000);

		$display("");
		$display("=== 3. block structure and channel status ===");

		// A block is 192 FRAMES, so a Z preamble appears once every
		// 192 -- the first version of this test ran 44 frames and
		// concluded there were none.
		// 400 frames: a block start appears within 192, and the
		// channel-status field at bits 24-27 needs another 28 after
		// it before there is anything to read.
		rx_z = 0; rx_x = 0; rx_y = 0;
		play(16'sh0F0F, 16'sh1E1E, 400);

		$display("  over 400 frames: Z=%0d X=%0d Y=%0d", rx_z, rx_x, rx_y);
		if (rx_z < 1) begin
			$display("FAIL no block-start preamble in 400 frames");
			errors = errors + 1;
		end else begin
			$display("  ok  block-start preamble present");
		end
		if (rx_z > 4) begin
			$display("FAIL too many block starts -- block is not 192 frames");
			errors = errors + 1;
		end else begin
			$display("  ok  roughly one block start per 192 frames");
		end

		// Channel status, consumer format: bit 0 clear = consumer,
		// bit 1 clear = linear PCM, bit 2 set = copy permitted, and
		// bits 24-27 carry fs_code.
		check("cs bit 0 (consumer)", rx_cs[0], 0);
		check("cs bit 1 (linear PCM)", rx_cs[1], 0);
		check("cs bit 2 (copy permitted)", rx_cs[2], 1);
		check("cs bits 24-27 (fs code)", rx_cs[27:24], 4'b0100);

		$display("");
		$display("=== 4. enable low is silence, not a dead line ===");

		enable = 0;
		play(16'sh4321, 16'sh4321, 8);
		check("muted left", got_l, 16'h0000);
		check("muted right", got_r, 16'h0000);
		if (rx_subframes < 20) begin
			$display("FAIL transmitter stopped when muted");
			errors = errors + 1;
		end else begin
			$display("  ok  still transmitting while muted");
		end
		enable = 1;

		$display("");
		if (errors == 0) $display("=== tb_audio_spdif: PASS ===");
		else $display("=== tb_audio_spdif: %0d FAILURE(S) ===", errors);
		$display("");
		$finish;
	end

	initial begin
		#40000000;
		$display("TIMEOUT");
		$finish;
	end

endmodule
