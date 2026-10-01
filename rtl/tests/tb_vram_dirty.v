/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/socctl.v's DIRTY register (word 7): the per-stripe
 * record of which parts of the framebuffer have been written.
 *
 *   - reset state is all thirty stripes dirty;
 *   - every one of the 9600 framebuffer words sets exactly its own
 *     stripe, word / 320, and nothing else;
 *   - every word address past the framebuffer, up to the 15 bits the
 *     port carries, sets nothing;
 *   - reading has no side effect;
 *   - write-one-to-clear clears only the 1s written, lane by lane;
 *   - THE RACE: a VRAM write at every offset from -6 to +3 cycles
 *     around the clearing store. Whatever lands from the clear's own
 *     cycle on must survive it -- including a set and a clear on the
 *     same edge -- and a write the clear does erase must have reached
 *     VRAM BEFORE the clear, so the copy that follows it sees the
 *     pixels anyway.
 *
 *   iverilog -g2012 -o /tmp/tb tb_vram_dirty.v ../socctl.v && vvp /tmp/tb
 */

`timescale 1ns/1ps
module tb;
	reg clk = 0, rst = 1;
	reg [31:0] adr = 0, dat_w = 0;
	reg [3:0] sel = 4'hf;
	reg we = 0, stb = 0, cyc = 0;
	wire [31:0] dat_r;
	wire ack;
	reg vram_we = 0;
	reg [14:0] vram_adr = 0;

	integer fails = 0, guard, w, off, s;
	reg [31:0] rdata, r2;

	socctl_wb dut (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.wb_adr_i(adr), .wb_dat_i(dat_w), .wb_dat_o(dat_r),
		.wb_we_i(we), .wb_sel_i(sel), .wb_stb_i(stb),
		.wb_ack_o(ack), .wb_cyc_i(cyc),
		.cursor_busy(), .video_mode(), .view_load(), .game_en(),
		.game_wrap(), .view_x(), .view_y(),
		.frame_ctr(16'd0), .in_vblank(1'b0), .reconfig(),
		.vram_we(vram_we), .vram_adr(vram_adr));

	always #10 clk = ~clk;

	initial begin
		#50000000;
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
			stb = 0; cyc = 0;
			tick;
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

	task expect_dirty(input [31:0] want, input [8*24-1:0] what);
		begin
			wbr(7);
			if (rdata !== want) begin
				$display("FAIL: %0s: DIRTY %08x, expected %08x", what, rdata, want);
				fails = fails + 1;
			end
		end
	endtask

	initial begin
		tick; tick; rst = 0; tick;

		// -- reset --
		expect_dirty(32'h3fff_ffff, "after reset");
		wbw(7, 32'hffff_ffff, 4'hf);
		expect_dirty(32'h0, "after clearing all");

		// -- reads are pure --
		vwrite(15'd0); settle;
		wbr(7); r2 = rdata;
		expect_dirty(r2, "second read");
		if (r2 !== 32'h1) begin $display("FAIL: word 0 -> %08x", r2); fails = fails + 1; end
		wbw(7, 32'h1, 4'hf);

		// -- every framebuffer word, its own stripe and nothing else --
		for (w = 0; w < 9600; w = w + 1) begin
			vwrite(w[14:0]); settle;
			wbr(7);
			if (rdata !== (32'h1 << (w / 320))) begin
				if (fails < 20)
					$display("FAIL: word %0d -> %08x, expected stripe %0d", w, rdata, w / 320);
				fails = fails + 1;
			end
			wbw(7, rdata, 4'hf);
		end
		expect_dirty(32'h0, "after the 9600 words");
		$display("9600 framebuffer words: each set exactly stripe word/320");

		// -- past the framebuffer: nothing --
		for (w = 9600; w < 32768; w = w + 1) begin
			vram_adr = w[14:0]; vram_we = 1;
			tick;
		end
		vram_we = 0; settle;
		expect_dirty(32'h0, "words 9600..32767");
		$display("words 9600..32767: no stripe set");

		// -- several stripes accumulate; clear by lane --
		vwrite(15'd0); vwrite(15'd320 * 8); vwrite(15'd320 * 17); vwrite(15'd320 * 29);
		settle;
		expect_dirty(32'h2002_0101, "four stripes");
		wbw(7, 32'hffff_ffff, 4'b0001);		// lane 0: stripes 0..7
		expect_dirty(32'h2002_0100, "lane 0 cleared");
		wbw(7, 32'h0000_0000, 4'hf);		// 0s clear nothing
		expect_dirty(32'h2002_0100, "zeros written");
		wbw(7, 32'h2000_0000, 4'hf);		// only the 1s
		expect_dirty(32'h0002_0100, "stripe 29 cleared");
		wbw(7, 32'hffff_ffff, 4'hf);
		expect_dirty(32'h0, "all cleared");

		// -- THE RACE --
		// The clear's edge E is the one at which its store is
		// registered (stb high, ack still low). A VRAM write at port
		// edge E+off reaches DIRTY at E+off+2. Survives iff
		// off >= -2; erased only if it reached VRAM strictly before
		// E. Two cases each: the stripe already dirty before the
		// clear (the clear names it), and clean (the clear does not).
		for (s = 0; s < 2; s = s + 1) begin
			for (off = -6; off <= 3; off = off + 1) begin
				wbw(7, 32'hffff_ffff, 4'hf); settle;
				if (s) begin vwrite(15'd320 * 5); settle; end
				// Line the two up: from a common start, the VRAM
				// write's edge is max(off,0)+1 and the clear's is
				// max(-off,0)+1, so the write lands at E+off.
				fork
					begin
						if (off > 0) repeat (off) tick;
						vram_adr = 15'd320 * 5 + 7; vram_we = 1;
						tick; vram_we = 0;
					end
					begin
						if (off < 0) repeat (-off) tick;
						adr = 7; dat_w = 32'h0000_0020; sel = 4'hf;
						we = 1; stb = 1; cyc = 1;
						tick;	// edge E
						while (!ack) tick;
						stb = 0; cyc = 0; we = 0;
					end
				join
				settle;
				wbr(7);
				if ((off >= -2) && rdata !== 32'h20) begin
					$display("FAIL: %0s stripe, write at E%0d LOST by the clear (DIRTY %08x)",
						s ? "dirty" : "clean", off, rdata);
					fails = fails + 1;
				end
				if ((off < -2) && rdata !== 32'h0) begin
					$display("FAIL: %0s stripe, write at E%0d should have been cleared (DIRTY %08x)",
						s ? "dirty" : "clean", off, rdata);
					fails = fails + 1;
				end
			end
		end
		$display("race: VRAM write at E-6..E+3 around the clear, stripe dirty and clean before it");

		if (fails == 0) $display("PASS: tb_vram_dirty");
		else $display("FAILED: %0d", fails);
		$finish;
	end
endmodule
