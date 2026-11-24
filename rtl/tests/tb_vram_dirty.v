/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/socctl.v's DIRTY registers (words 7, 20..23):
 * the per-stripe record of which parts of the framebuffer have been
 * written.
 *
 * Two socctls on the same bus: one as a VIDEO_WIDE build has it
 * (VMODES_AVAIL and WIDE_AVAIL: 100 stripes, 1280x800) and one as every
 * other build (30 stripes). Every check is made on both.
 *
 *   - reset state is every stripe dirty; DIRTY's bit 31 says whether
 *     DIRTY1..3 exist;
 *   - every one of the 32768 word addresses the port carries sets
 *     exactly its own stripe, word / 320, in the register that holds
 *     it -- and nothing at all past the last stripe the build tracks
 *     (word 32000 with 100, word 9600 with 30);
 *   - reading has no side effect;
 *   - write-one-to-clear clears only the 1s written, lane by lane, and
 *     only in the word written;
 *   - word 20 is DIRTY again, read and cleared, where DIRTY1..3 are,
 *     and nothing where they are not;
 *   - THE RACE: a VRAM write at every offset from -6 to +3 cycles
 *     around the clearing store, in DIRTY and in DIRTY2. Whatever
 *     lands from the clear's own cycle on must survive it -- including
 *     a set and a clear on the same edge -- and a write the clear does
 *     erase must have reached VRAM BEFORE the clear, so the copy that
 *     follows it sees the pixels anyway.
 *
 *   iverilog -g2012 -o /tmp/tb tb_vram_dirty.v ../socctl.v && vvp /tmp/tb
 */

`timescale 1ns/1ps
module tb;
	reg clk = 0, rst = 1;
	reg [31:0] adr = 0, dat_w = 0;
	reg [3:0] sel = 4'hf;
	reg we = 0, stb = 0, cyc = 0;
	wire [31:0] dat_r, dat_r30;
	wire ack, ack30;
	reg vram_we = 0;
	reg [14:0] vram_adr = 0;

	integer fails = 0, guard, w, off, s, k, rg;
	reg [31:0] rdata, rdata30, r2;
	// the four words of each build, as last read by rd4
	reg [31:0] got [0:3];
	reg [31:0] got30 [0:3];
	reg [31:0] want [0:3];
	reg [31:0] want30 [0:3];

	// as a VIDEO_WIDE build (1280x800)
	socctl_wb #(.VMODES_AVAIL(1'b1), .WIDE_AVAIL(1'b1)) dut (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.wb_adr_i(adr), .wb_dat_i(dat_w), .wb_dat_o(dat_r),
		.wb_we_i(we), .wb_sel_i(sel), .wb_stb_i(stb),
		.wb_ack_o(ack), .wb_cyc_i(cyc),
		.cursor_busy(), .cursor_hide(), .video_mode(), .view_load(), .game_en(),
		.game_wrap(), .view_x(), .view_y(),
		.frame_ctr(16'd0), .in_vblank(1'b0), .reconfig(),
		.vram_we(vram_we), .vram_adr(vram_adr));

	// as every other build
	socctl_wb dut30 (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.wb_adr_i(adr), .wb_dat_i(dat_w), .wb_dat_o(dat_r30),
		.wb_we_i(we), .wb_sel_i(sel), .wb_stb_i(stb),
		.wb_ack_o(ack30), .wb_cyc_i(cyc),
		.cursor_busy(), .cursor_hide(), .video_mode(), .view_load(), .game_en(),
		.game_wrap(), .view_x(), .view_y(),
		.frame_ctr(16'd0), .in_vblank(1'b0), .reconfig(),
		.vram_we(vram_we), .vram_adr(vram_adr));

	always #10 clk = ~clk;

	initial begin
		#200000000;
		$display("WATCHDOG: simulation did not finish");
		$finish;
	end

	// Inputs change 1ns after a rising edge, as a synchronous master's
	// registered outputs would.
	task tick; begin @(posedge clk); #1; end endtask

	task wbw(input [31:0] word, input [31:0] d, input [3:0] lanes);
		begin
			adr = word; dat_w = d; sel = lanes; we = 1; stb = 1; cyc = 1;
			guard = 0;
			tick;
			while (!ack && guard < 50) begin tick; guard = guard + 1; end
			if (guard >= 50) begin $display("FAIL: write to %0d never acked", word); fails = fails + 1; end
			if (ack30 !== ack) begin $display("FAIL: the two builds ack differently"); fails = fails + 1; end
			stb = 0; cyc = 0; we = 0; sel = 4'hf;
		end
	endtask

	task wbr(input [31:0] word);
		begin
			adr = word; we = 0; stb = 1; cyc = 1;
			guard = 0;
			tick;
			while (!ack && guard < 50) begin tick; guard = guard + 1; end
			if (guard >= 50) begin $display("FAIL: read of %0d never acked", word); fails = fails + 1; end
			rdata = dat_r;
			rdata30 = dat_r30;
			stb = 0; cyc = 0;
			tick;
		end
	endtask

	// register k of the four: word 7, 21, 22, 23
	function [31:0] word_of(input integer k);
		word_of = (k == 0) ? 32'd7 : 32'd20 + k;
	endfunction

	task rd4;
		begin
			for (k = 0; k < 4; k = k + 1) begin
				wbr(word_of(k));
				got[k] = rdata;
				got30[k] = rdata30;
			end
		end
	endtask

	// clear, in each build, everything it reads
	task clear4;
		begin
			for (k = 0; k < 4; k = k + 1)
				wbw(word_of(k), 32'hffff_ffff, 4'hf);
		end
	endtask

	// One accepted VRAM write: high for exactly one clock edge.
	task vwrite(input [14:0] a);
		begin
			vram_adr = a; vram_we = 1;
			tick;
			vram_we = 0;
		end
	endtask

	task settle; begin tick; tick; tick; end endtask

	// DIRTY's bit 31 in each build
	`define HI100 32'h8000_0000
	`define HI30  32'h0000_0000

	task expect4(input [8*32-1:0] what);
		begin
			rd4;
			for (k = 0; k < 4; k = k + 1) begin
				if (got[k] !== want[k]) begin
					$display("FAIL: %0s: 100 stripes, word %0d = %08x, expected %08x",
						what, word_of(k), got[k], want[k]);
					fails = fails + 1;
				end
				if (got30[k] !== want30[k]) begin
					$display("FAIL: %0s: 30 stripes, word %0d = %08x, expected %08x",
						what, word_of(k), got30[k], want30[k]);
					fails = fails + 1;
				end
			end
		end
	endtask

	// the four words with stripe st set, in a build tracking n stripes
	task stripe_words(input integer st, input integer n, input [31:0] hi);
		begin
			for (k = 0; k < 4; k = k + 1) want[k] = 0;
			want[0] = hi;
			if (st < n)
				want[st / 30] = want[st / 30] | (32'h1 << (st % 30));
		end
	endtask

	task set_want(input [31:0] a, input [31:0] b, input [31:0] c, input [31:0] d);
		begin
			want[0] = a | `HI100; want[1] = b; want[2] = c; want[3] = d;
		end
	endtask

	task set_want30(input [31:0] a);
		begin
			want30[0] = a; want30[1] = 0; want30[2] = 0; want30[3] = 0;
		end
	endtask

	// THE RACE, in register k (stripe st = 30k + 5): see below
	task race(input integer rk);
		reg [14:0] va;
		reg [31:0] word;
		reg [31:0] r100, r30;
		begin
			va = 15'd320 * (30 * rk + 5) + 7;
			word = word_of(rk);
			for (s = 0; s < 2; s = s + 1) begin
				for (off = -6; off <= 3; off = off + 1) begin
					clear4; settle;
					if (s) begin vwrite(va); settle; end
					// Line the two up: from a common start, the VRAM
					// write's edge is max(off,0)+1 and the clear's is
					// max(-off,0)+1, so the write lands at E+off.
					fork
						begin
							if (off > 0) repeat (off) tick;
							vram_adr = va; vram_we = 1;
							tick; vram_we = 0;
						end
						begin
							if (off < 0) repeat (-off) tick;
							adr = word; dat_w = 32'h0000_0020; sel = 4'hf;
							we = 1; stb = 1; cyc = 1;
							tick;	// edge E
							while (!ack) tick;
							stb = 0; cyc = 0; we = 0;
						end
					join
					settle;
					wbr(word);
					r100 = rdata & 32'h3fff_ffff;
					r30 = rdata30 & 32'h3fff_ffff;
					if ((off >= -2) && r100 !== 32'h20) begin
						$display("FAIL: word %0d, %0s stripe, write at E%0d LOST by the clear (%08x)",
							word, s ? "dirty" : "clean", off, rdata);
						fails = fails + 1;
					end
					if ((off < -2) && r100 !== 32'h0) begin
						$display("FAIL: word %0d, %0s stripe, write at E%0d should have been cleared (%08x)",
							word, s ? "dirty" : "clean", off, rdata);
						fails = fails + 1;
					end
					// with 30 stripes, the same in DIRTY and nothing in DIRTY2
					if (rk == 0 && r30 !== r100) begin
						$display("FAIL: 30 stripes, race at E%0d: %08x", off, rdata30);
						fails = fails + 1;
					end
					if (rk != 0 && rdata30 !== 32'h0) begin
						$display("FAIL: 30 stripes, word %0d reads %08x", word, rdata30);
						fails = fails + 1;
					end
				end
			end
		end
	endtask

	initial begin
		tick; tick; rst = 0; tick;

		// -- reset --
		set_want(32'h3fff_ffff, 32'h3fff_ffff, 32'h3fff_ffff, 32'h0000_03ff);
		set_want30(32'h3fff_ffff);
		expect4("after reset");
		wbr(20);
		if (rdata !== 32'hbfff_ffff || rdata30 !== 32'h0) begin
			$display("FAIL: word 20 after reset: %08x / %08x", rdata, rdata30); fails = fails + 1;
		end
		clear4;
		set_want(0, 0, 0, 0);
		set_want30(0);
		expect4("after clearing all");

		// -- reads are pure --
		vwrite(15'd0); vwrite(15'd320 * 64); settle;
		wbr(7); r2 = rdata;
		wbr(7);
		if (rdata !== r2 || r2 !== (`HI100 | 32'h1)) begin
			$display("FAIL: word 0: DIRTY %08x then %08x", r2, rdata); fails = fails + 1;
		end
		wbr(22); r2 = rdata;
		wbr(22);
		if (rdata !== r2 || r2 !== 32'h10) begin
			$display("FAIL: word 20480: DIRTY2 %08x then %08x", r2, rdata); fails = fails + 1;
		end
		clear4;

		// -- every word address, its own stripe and nothing else --
		for (w = 0; w < 32768; w = w + 1) begin
			vwrite(w[14:0]); settle;
			rd4;
			stripe_words(w / 320, 100, `HI100);
			for (k = 0; k < 4; k = k + 1) begin
				if (got[k] !== want[k]) begin
					if (fails < 20)
						$display("FAIL: 100 stripes: word %0d -> word %0d = %08x, expected %08x",
							w, word_of(k), got[k], want[k]);
					fails = fails + 1;
				end
			end
			for (k = 0; k < 4; k = k + 1) begin
				want30[k] = (k == 0 && w < 9600) ? (32'h1 << (w / 320)) : 32'h0;
				if (got30[k] !== want30[k]) begin
					if (fails < 20)
						$display("FAIL: 30 stripes: word %0d -> word %0d = %08x, expected %08x",
							w, word_of(k), got30[k], want30[k]);
					fails = fails + 1;
				end
			end
			// clear exactly what was read, as software does
			for (k = 0; k < 4; k = k + 1)
				if (got[k] | got30[k])
					wbw(word_of(k), got[k] | got30[k], 4'hf);
		end
		set_want(0, 0, 0, 0);
		set_want30(0);
		expect4("after the 32768 words");
		$display("32768 word addresses: each set exactly stripe word/320, in its register;");
		$display("  nothing past word 32000 (100 stripes) or word 9600 (30 stripes)");

		// -- several stripes accumulate; clear by lane, word by word --
		vwrite(15'd0); vwrite(15'd320 * 8); vwrite(15'd320 * 17); vwrite(15'd320 * 29);
		vwrite(15'd320 * 30); vwrite(15'd320 * 47); vwrite(15'd320 * 59);
		vwrite(15'd320 * 60); vwrite(15'd320 * 76);
		vwrite(15'd320 * 90); vwrite(15'd320 * 99);
		settle;
		set_want(32'h2002_0101, 32'h2002_0001, 32'h0001_0001, 32'h0000_0201);
		set_want30(32'h2002_0101);
		expect4("eleven stripes");
		wbw(7, 32'hffff_ffff, 4'b0001);		// lane 0: stripes 0..7
		set_want(32'h2002_0100, 32'h2002_0001, 32'h0001_0001, 32'h0000_0201);
		set_want30(32'h2002_0100);
		expect4("DIRTY lane 0 cleared");
		wbr(20);
		if (rdata !== (`HI100 | 32'h2002_0100) || rdata30 !== 32'h0) begin
			$display("FAIL: word 20 is not DIRTY: %08x / %08x", rdata, rdata30); fails = fails + 1;
		end
		wbw(20, 32'h0000_0100, 4'hf);		// stripe 8, by word 20
		set_want(32'h2002_0000, 32'h2002_0001, 32'h0001_0001, 32'h0000_0201);
		set_want30(32'h2002_0100);		// word 20 is nothing here
		expect4("stripe 8 cleared by word 20");
		wbw(21, 32'hffff_ffff, 4'b0100);	// lane 2: stripes 46..53
		set_want(32'h2002_0000, 32'h2000_0001, 32'h0001_0001, 32'h0000_0201);
		expect4("DIRTY1 lane 2 cleared");
		wbw(22, 32'h0000_0000, 4'hf);		// 0s clear nothing
		expect4("zeros written");
		wbw(23, 32'h0000_0200, 4'hf);		// only the 1s
		set_want(32'h2002_0000, 32'h2000_0001, 32'h0001_0001, 32'h0000_0001);
		expect4("stripe 99 cleared");
		wbw(22, 32'hffff_ffff, 4'hf);
		set_want(32'h2002_0000, 32'h2000_0001, 32'h0000_0000, 32'h0000_0001);
		expect4("DIRTY2 cleared, and only it");
		clear4;
		set_want(0, 0, 0, 0);
		set_want30(0);
		expect4("all cleared");

		// -- THE RACE --
		// The clear's edge E is the one at which its store is
		// registered (stb high, ack still low). A VRAM write at port
		// edge E+off reaches the register at E+off+2. Survives iff
		// off >= -2; erased only if it reached VRAM strictly before
		// E. Two cases each: the stripe already dirty before the
		// clear (the clear names it), and clean (the clear does not).
		race(0);
		race(2);
		$display("race: VRAM write at E-6..E+3 around the clear, stripe dirty and clean before it,");
		$display("  in DIRTY and in DIRTY2");

		if (fails == 0) $display("PASS: tb_vram_dirty");
		else $display("FAILED: %0d", fails);
		$finish;
	end
endmodule
