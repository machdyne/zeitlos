/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/sha256.v: whole messages hashed through the block,
 * compared with hashlib's digests (gen_sha256_vectors.py). Words go in
 * through W (as a little-endian CPU loads them; the block swaps) for
 * even messages and through WBE (already big-endian) for odd ones.
 * Also MAGIC, CONFIG, the claim, and cycles per block.
 *
 *   iverilog -g2012 -o /tmp/tb tb_sha256.v ../sha256.v && vvp /tmp/tb
 */

`timescale 1ns/1ps
module tb;
	reg clk = 0, resetn = 0;
	reg [31:0] adr = 0, dat_w = 0;
	reg we = 0, stb = 0, cyc = 0;
	wire [31:0] dat_r;
	wire ack;
	integer guard;
	reg [31:0] rdata;

	sha256 dut (
		.clk(clk), .resetn(resetn),
		.wb_adr_i(adr), .wb_dat_i(dat_w), .wb_dat_o(dat_r),
		.wb_we_i(we), .wb_sel_i(4'hf), .wb_stb_i(stb),
		.wb_ack_o(ack), .wb_cyc_i(cyc));

	always #10 clk = ~clk;

	initial begin
		#20000000;
		$display("WATCHDOG: simulation did not finish");
		$finish;
	end

	// Word addresses, as sysctl.v hands the block.
	task wbw(input [31:0] word, input [31:0] d);
		begin
			while (ack) @(posedge clk);
			adr = word; dat_w = d; we = 1; stb = 1; cyc = 1;
			guard = 0;
			while (!ack && guard < 50) begin @(posedge clk); guard = guard + 1; end
			if (guard >= 50) $display("FAIL: write to %0d never acked", word);
			stb = 0; cyc = 0; we = 0;		// on the ack, as the CPU's bus does
			@(posedge clk);
		end
	endtask

	task wbr(input [31:0] word);
		begin
			while (ack) @(posedge clk);
			adr = word; we = 0; stb = 1; cyc = 1;
			guard = 0;
			while (!ack && guard < 50) begin @(posedge clk); guard = guard + 1; end
			if (guard >= 50) $display("FAIL: read of %0d never acked", word);
			rdata = dat_r;
			stb = 0; cyc = 0;
			@(posedge clk);
		end
	endtask

	function [31:0] bswap(input [31:0] x);
		bswap = { x[7:0], x[15:8], x[23:16], x[31:24] };
	endfunction

	integer fd, rc, nmsg, m, nblk, bk, k, polls, fails, t0, now, worst;
	reg [31:0] blk [0:15];
	reg [31:0] want [0:7];
	reg [31:0] iv [0:7];
	always @(posedge clk) now = now + 1;

	initial begin
		now = 0; fails = 0; worst = 0;
		iv[0] = 32'h6a09e667; iv[1] = 32'hbb67ae85; iv[2] = 32'h3c6ef372; iv[3] = 32'ha54ff53a;
		iv[4] = 32'h510e527f; iv[5] = 32'h9b05688c; iv[6] = 32'h1f83d9ab; iv[7] = 32'h5be0cd19;
		repeat (4) @(posedge clk);
		resetn = 1;
		repeat (2) @(posedge clk);

		wbr(0); if (rdata !== 32'h5A534841) begin $display("FAIL: MAGIC %08x", rdata); fails = fails + 1; end
		wbr(2); if (rdata !== 32'h53480001) begin $display("FAIL: CONFIG %08x", rdata); fails = fails + 1; end
		wbw(3, 9); wbr(3); if (rdata !== 9) begin $display("FAIL: claim"); fails = fails + 1; end
		wbw(3, 4); wbr(3); if (rdata !== 9) begin $display("FAIL: claimed over"); fails = fails + 1; end
		wbw(3, 32'h80000009); wbr(3); if (rdata !== 0) begin $display("FAIL: release"); fails = fails + 1; end

		fd = $fopen("sha256_vectors.txt", "r");
		if (fd == 0) begin $display("no vectors"); $finish; end
		rc = $fscanf(fd, "%d\n", nmsg);
		for (m = 0; m < nmsg; m = m + 1) begin
			for (k = 0; k < 8; k = k + 1) wbw(8 + k, iv[k]);
			rc = $fscanf(fd, "%d\n", nblk);
			for (bk = 0; bk < nblk; bk = bk + 1) begin
				for (k = 0; k < 16; k = k + 1) rc = $fscanf(fd, "%h", blk[k]);
				for (k = 0; k < 16; k = k + 1)
					if (m % 2 == 0) wbw(4, blk[k]);
					else wbw(5, bswap(blk[k]));
				t0 = now;
				wbw(1, 1);
				polls = 0; rdata = 1;
				while (rdata[0] && polls < 1000) begin wbr(1); polls = polls + 1; end
				if (now - t0 > worst) worst = now - t0;
				if (polls >= 1000) begin $display("FAIL: stuck busy"); fails = fails + 1; end
			end
			for (k = 0; k < 8; k = k + 1) rc = $fscanf(fd, "%h", want[k]);
			for (k = 0; k < 8; k = k + 1) begin
				wbr(8 + k);
				if (rdata !== want[k]) begin
					$display("FAIL: message %0d (%0d blocks) H%0d = %08x, want %08x", m, nblk, k, rdata, want[k]);
					fails = fails + 1;
				end
			end
		end
		$display("sha256: %0d messages against hashlib, %0d failed; <= %0d cycles a block, bus included",
			nmsg, fails, worst);
		$finish;
	end
endmodule
