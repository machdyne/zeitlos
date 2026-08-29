/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/audio_mixer.v.
 *
 *   iverilog -g2005 -o /tmp/tb_mix rtl/tb/tb_audio_mixer.v rtl/audio_mixer.v
 *   vvp /tmp/tb_mix
 *
 * The mixer is a wishbone MASTER, so the thing this testbench has to
 * get right is the memory on the other end of it -- and the lesson
 * from rtl/tb/tb_spim.v applies again: a model that acks instantly and
 * ignores the protocol proves nothing. The memory below inserts a
 * settable number of wait states and checks that the mixer holds cyc
 * and adr stable for the whole transaction, which is what a real slave
 * requires and what a sloppy master gets away with against a
 * zero-latency model.
 *
 * Every expected value here is arithmetic, not a golden capture: the
 * sample data is a known ramp, so what the mixer should produce for a
 * given step and gain can be written down.
 */

`timescale 1ns / 1ps

module tb_audio_mixer;

	localparam real CLK_HALF_NS = 10.416667;   // 48MHz
	localparam integer FRAC = 14;

	reg clk;
	reg rst;
	reg frame_req;

	reg cfg_we;
	reg [2:0] cfg_ch;
	reg [2:0] cfg_reg;
	reg [31:0] cfg_dat;
	reg [7:0] mixvol;

	wire [31:0] m_adr;
	wire [31:0] m_dat_o;
	reg [31:0] m_dat_i;
	wire m_we;
	wire [3:0] m_sel;
	wire m_stb;
	wire m_cyc;
	reg m_ack;

	wire signed [15:0] out_l;
	wire signed [15:0] out_r;
	wire [7:0] active;

	integer errors;
	integer i;
	integer waits;
	integer reads;
	reg [31:0] hold_adr;
	reg in_cycle;

	// -- behavioural main memory --
	//
	// 4KB, byte addressable, holding a known ramp so any byte the
	// mixer fetches identifies itself. Base 32'h4000_0000 to match
	// Obst's SRAM window.
	reg [7:0] mem [0:4095];
	localparam [31:0] MEM_BASE = 32'h4000_0000;

	audio_mixer #(.FRAC_BITS(FRAC)) dut (
		.clk(clk),
		.rst(rst),
		.frame_req(frame_req),
		.cfg_we(cfg_we),
		.cfg_ch(cfg_ch),
		.cfg_reg(cfg_reg),
		.cfg_dat(cfg_dat),
		.mixvol(mixvol),
		.m_adr_o(m_adr),
		.m_dat_o(m_dat_o),
		.m_dat_i(m_dat_i),
		.m_we_o(m_we),
		.m_sel_o(m_sel),
		.m_stb_o(m_stb),
		.m_cyc_o(m_cyc),
		.m_ack_i(m_ack),
		.out_l(out_l),
		.out_r(out_r),
		.active_o(active)
	);

	initial begin
		clk = 1'b0;
		forever #(CLK_HALF_NS) clk = ~clk;
	end

	// ------------------------------------------------------------
	// behavioural wishbone memory, with wait states
	// ------------------------------------------------------------
	integer wcount;

	always @(posedge clk) begin
		if (rst) begin
			m_ack <= 1'b0;
			wcount <= 0;
			in_cycle <= 1'b0;
			reads <= 0;
		end else begin
			m_ack <= 1'b0;

			if (m_cyc && m_stb && !m_ack) begin

				if (!in_cycle) begin
					in_cycle <= 1'b1;
					hold_adr <= m_adr;
					wcount <= 0;
				end else begin
					// A master that moves its address mid-cycle is
					// broken in a way a zero-latency model would never
					// notice -- see this file's header.
					if (m_adr !== hold_adr) begin
						$display("FAIL master moved adr mid-cycle: %08x -> %08x",
							hold_adr, m_adr);
						errors = errors + 1;
					end
					if (m_we) begin
						$display("FAIL mixer asserted we -- it is read only");
						errors = errors + 1;
					end
				end

				if (wcount >= waits) begin
					m_dat_i <= {
						mem[(m_adr - MEM_BASE) + 3],
						mem[(m_adr - MEM_BASE) + 2],
						mem[(m_adr - MEM_BASE) + 1],
						mem[(m_adr - MEM_BASE) + 0] };
					m_ack <= 1'b1;
					in_cycle <= 1'b0;
					reads <= reads + 1;
				end else begin
					wcount <= wcount + 1;
				end

			end else begin
				in_cycle <= 1'b0;
				wcount <= 0;
			end
		end
	end

	// ------------------------------------------------------------
	// helpers
	// ------------------------------------------------------------

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

	task cfg;
		input [2:0] ch;
		input [2:0] rg;
		input [31:0] dat;
		begin
			@(posedge clk);
			cfg_ch <= ch;
			cfg_reg <= rg;
			cfg_dat <= dat;
			cfg_we <= 1'b1;
			@(posedge clk);
			cfg_we <= 1'b0;
			@(posedge clk);
		end
	endtask

	// Set up channel `ch` to play `len` bytes from MEM_BASE+`off`,
	// looping `llen` bytes from `lst`, at `step`, gains `gl`/`gr`.
	task setup_ch;
		input [2:0] ch;
		input [31:0] off;
		input [31:0] len;
		input [31:0] lst;
		input [31:0] llen;
		input [31:0] step;
		input [7:0] gl;
		input [7:0] gr;
		begin
			cfg(ch, 3'd0, MEM_BASE + off);
			cfg(ch, 3'd1, len);
			cfg(ch, 3'd2, lst);
			cfg(ch, 3'd3, llen);
			cfg(ch, 3'd4, step);
			// EN + TRIG, offset 0
			cfg(ch, 3'd5, { 8'd0, 6'b0, 1'b1, 1'b1, gr, gl });
		end
	endtask

	task disable_ch;
		input [2:0] ch;
		begin
			cfg(ch, 3'd5, 32'h0000_0000);
		end
	endtask

	// One frame, and wait for the mixer to finish it.
	//
	// Waits for the sequencer to LEAVE idle before waiting for it to
	// come back. Checking only the second condition looks equivalent
	// and is not: dut.state is updated non-blockingly, so sampling it
	// in the same timestep as the edge that starts the frame still
	// reads IDLE, the wait falls straight through, and every check
	// downstream compares against a frame that never ran. That is
	// exactly what the first version of this task did, and it failed
	// 17 of 22 checks against a mixer that turned out to be correct.
	//
	// The #1 is the same defence in the other direction: it moves the
	// sample off the edge entirely rather than relying on event
	// ordering within a timestep.
	task one_frame;
		begin
			@(posedge clk);
			frame_req <= 1'b1;
			@(posedge clk);
			frame_req <= 1'b0;
			#1;
			while (dut.state == 4'd0) @(posedge clk);
			#1;
			// worst case is 8 channels of bus reads with wait states
			while (dut.state != 4'd0) @(posedge clk);
			@(posedge clk);
			#1;
		end
	endtask

	// ------------------------------------------------------------

	initial begin

		if ($test$plusargs("vcd")) begin
			$dumpfile("/tmp/tb_audio_mixer.vcd");
			$dumpvars(0, tb_audio_mixer);
		end

		errors = 0;
		rst = 1'b1;
		frame_req = 1'b0;
		cfg_we = 1'b0;
		cfg_ch = 3'd0;
		cfg_reg = 3'd0;
		cfg_dat = 32'd0;
		mixvol = 8'd255;
		waits = 2;
		m_dat_i = 32'd0;

		// sample memory: byte n holds n, so a fetched byte names its
		// own offset. Read as SIGNED 8-bit by the mixer, so offsets
		// 128..255 come back negative -- which the expected values
		// below account for.
		for (i = 0; i < 4096; i = i + 1)
			mem[i] = i[7:0];

		repeat (8) @(posedge clk);
		rst = 1'b0;
		repeat (4) @(posedge clk);

		$display("");
		$display("=== 1. one channel, unity step ===");

		// 64 bytes from offset 0, no loop, step = exactly 1.0
		setup_ch(3'd0, 32'd0, 32'd64, 32'd0, 32'd0,
			32'd1 << FRAC, 8'd255, 8'd255);

		one_frame;
		// first frame plays byte 0 -> 0 * 255 = 0
		check("frame 0 out_l", out_l, 0);
		check("channel active", active[0], 1);

		one_frame;
		// byte 1 -> 1*255 = 255; scaled (255 * 255) >> 10 = 63
		check("frame 1 out_l", out_l, (1 * 255 * 255) >>> 10);

		one_frame;
		check("frame 2 out_l", out_l, (2 * 255 * 255) >>> 10);

		$display("");
		$display("=== 2. gains are independent per side ===");

		setup_ch(3'd0, 32'd0, 32'd64, 32'd0, 32'd0,
			32'd1 << FRAC, 8'd255, 8'd64);
		one_frame;      // byte 0
		one_frame;      // byte 1
		one_frame;      // byte 2
		check("out_l at gain 255", out_l, (2 * 255 * 255) >>> 10);
		check("out_r at gain 64",  out_r, (2 * 64 * 255) >>> 10);

		$display("");
		$display("=== 3. step controls pitch ===");

		// step 2.0 should advance two bytes per frame
		setup_ch(3'd0, 32'd0, 32'd64, 32'd0, 32'd0,
			32'd2 << FRAC, 8'd255, 8'd255);
		one_frame;      // byte 0
		one_frame;      // byte 2
		check("step 2.0 -> byte 2", out_l, (2 * 255 * 255) >>> 10);
		one_frame;      // byte 4
		check("step 2.0 -> byte 4", out_l, (4 * 255 * 255) >>> 10);

		// step 0.5 should take two frames per byte
		setup_ch(3'd0, 32'd0, 32'd64, 32'd0, 32'd0,
			32'd1 << (FRAC - 1), 8'd255, 8'd255);
		one_frame;      // byte 0
		one_frame;      // still byte 0 (pos 0.5)
		check("step 0.5 -> still byte 0", out_l, 0);
		one_frame;      // byte 1
		check("step 0.5 -> byte 1", out_l, (1 * 255 * 255) >>> 10);

		$display("");
		$display("=== 4. one-shot deactivates at the end ===");

		// 4 bytes, no loop, unity step
		setup_ch(3'd0, 32'd0, 32'd4, 32'd0, 32'd0,
			32'd1 << FRAC, 8'd255, 8'd255);
		one_frame;      // 0
		one_frame;      // 1
		one_frame;      // 2
		one_frame;      // 3
		check("still active at last byte", active[0], 1);
		one_frame;      // past the end
		check("deactivated past the end", active[0], 0);
		one_frame;
		check("silent once deactivated", out_l, 0);

		$display("");
		$display("=== 5. looping wraps and keeps playing ===");

		// 16 bytes, loop the last 4 (offset 12..15), unity step
		setup_ch(3'd0, 32'd0, 32'd16, 32'd12, 32'd4,
			32'd1 << FRAC, 8'd255, 8'd255);
		for (i = 0; i < 16; i = i + 1) one_frame;
		// after 16 frames pos would be 16 -> wrapped back to 12
		check("looped: still active", active[0], 1);
		// 16 unity-step frames play bytes 0..15; the 16th leaves pos
		// at 16, which wraps to loop start 12. So the NEXT frame is
		// byte 12, not 13 -- the loop start itself, played again.
		one_frame;
		check("looped: playing inside the loop", out_l,
			(12 * 255 * 255) >>> 10);

		// and it must not run away over many frames
		for (i = 0; i < 200; i = i + 1) one_frame;
		check("still active after 200 more frames", active[0], 1);
		if (dut.ch_pos[0] >= (32'd16 << FRAC)) begin
			$display("FAIL loop escaped the sample: pos=%08x", dut.ch_pos[0]);
			errors = errors + 1;
		end else begin
			$display("  ok  loop stayed inside the sample");
		end

		$display("");
		$display("=== 5b. NEGATIVE samples ===");

		// The ramp in memory is mem[i] = i, read as SIGNED, so byte
		// 200 is -56. Every test above happened to use bytes 0..15,
		// all positive -- which is exactly how a sign-extension bug in
		// the output scaler survived 22 passing checks.
		setup_ch(3'd0, 32'd192, 32'd64, 32'd0, 32'd0,
			32'd1 << FRAC, 8'd255, 8'd255);
		one_frame;                       // byte 192 -> -64
		check("negative sample scales negative", out_l,
			(-64 * 255 * 255) >>> 10);
		one_frame;                       // byte 193 -> -63
		check("negative sample, next", out_l, (-63 * 255 * 255) >>> 10);

		// straddling the sign boundary: 127 then -128
		setup_ch(3'd0, 32'd127, 32'd64, 32'd0, 32'd0,
			32'd1 << FRAC, 8'd255, 8'd255);
		one_frame;
		check("most positive sample", out_l, (127 * 255 * 255) >>> 10);
		one_frame;
		check("most negative sample", out_l, (-128 * 255 * 255) >>> 10);

		// and mixvol must scale a negative sum correctly too
		mixvol = 8'd51;
		one_frame;
		check("negative sum at mixvol 51", out_l,
			(-127 * 255 * 51) >>> 10);
		mixvol = 8'd255;

		$display("");
		$display("=== 6. eight channels sum ===");

		// every channel on the same byte, gain 32 each, so the sum is
		// exactly 8x one channel
		for (i = 0; i < 8; i = i + 1)
			setup_ch(i[2:0], 32'd0, 32'd64, 32'd0, 32'd0,
				32'd1 << FRAC, 8'd32, 8'd32);
		one_frame;      // byte 0
		one_frame;      // byte 1
		check("8 channels at gain 32", out_l, (8 * 1 * 32 * 255) >>> 10);
		check("all eight active", active, 255);

		$display("");
		$display("=== 7. mixvol scales the sum ===");

		mixvol = 8'd128;
		one_frame;      // byte 2
		check("mixvol 128", out_l, (8 * 2 * 32 * 128) >>> 10);
		mixvol = 8'd255;

		$display("");
		$display("=== 8. TRIG offset, and gain change without retrigger ===");

		for (i = 1; i < 8; i = i + 1) disable_ch(i[2:0]);

		// TRIG with offset field = 1 -> start at byte 256
		cfg(3'd0, 3'd0, MEM_BASE);
		cfg(3'd0, 3'd1, 32'd1024);
		cfg(3'd0, 3'd2, 32'd0);
		cfg(3'd0, 3'd3, 32'd0);
		cfg(3'd0, 3'd4, 32'd1 << FRAC);
		cfg(3'd0, 3'd5, { 8'd1, 6'b0, 1'b1, 1'b1, 8'd255, 8'd255 });
		one_frame;
		// byte 256 of the ramp is 0 again (mem[i] = i[7:0]); byte 257
		// is 1, which is the one that proves the offset landed
		one_frame;
		check("TRIG offset 1 -> byte 257", out_l, (1 * 255 * 255) >>> 10);

		// Now change gain only: EN set, TRIG clear. The sample must
		// keep playing from where it was, not restart -- this is the
		// commonest write a tracker makes and restarting on it is the
		// classic way a hardware channel ends up sounding clicky.
		cfg(3'd0, 3'd5, { 8'd0, 6'b0, 1'b0, 1'b1, 8'd128, 8'd128 });
		one_frame;
		check("gain change did not restart", out_l,
			(2 * 128 * 255) >>> 10);

		$display("");
		$display("=== 9. survives a slow bus ===");

		// Ten wait states per read is far worse than the real SRAM,
		// and eight channels still have to fit in a frame.
		waits = 10;
		for (i = 0; i < 8; i = i + 1)
			setup_ch(i[2:0], 32'd0, 32'd64, 32'd0, 32'd0,
				32'd1 << FRAC, 8'd32, 8'd32);
		reads = 0;
		one_frame;
		one_frame;
		check("8 reads per frame", reads, 16);
		check("still correct with waits", out_l,
			(8 * 1 * 32 * 255) >>> 10);
		waits = 2;

		$display("");
		if (errors == 0)
			$display("=== tb_audio_mixer: PASS ===");
		else
			$display("=== tb_audio_mixer: %0d FAILURE(S) ===", errors);
		$display("");

		$finish;

	end

endmodule
