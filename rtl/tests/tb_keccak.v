/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/keccak.v: Keccak-f[1600] on the states in
 * keccak_vectors.txt (gen_keccak_vectors.py: a reference written from
 * FIPS 202, checked there against hashlib's SHA-3 and SHAKE). Also: OUT
 * leaves the state as it was, XIN xors, CLEAR clears, writes while busy
 * are ignored, the claim, and cycles per permutation.
 *
 *   iverilog -o /tmp/tb tb_keccak.v ../keccak.v && vvp /tmp/tb
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

	keccak dut (
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

	task wbw(input [31:0] word, input [31:0] d);
		begin
			while (ack) @(posedge clk);
			adr = word; dat_w = d; we = 1; stb = 1; cyc = 1;
			guard = 0;
			while (!ack && guard < 50) begin @(posedge clk); guard = guard + 1; end
			if (guard >= 50) $display("FAIL: write to %0d never acked", word);
			stb = 0; cyc = 0; we = 0;
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

	integer fd, rc, n, k, fails, polls, t0, cyc_perm, worst, nvec;
	reg [31:0] vin [0:49];
	reg [31:0] vout [0:49];
	reg [31:0] prev [0:49];
	integer now;
	always @(posedge clk) now = now + 1;

	task check_out(input integer which);	// 50 OUT reads against vout (which 0) or vin (1)
		begin
			for (k = 0; k < 50; k = k + 1) begin
				wbr(6);
				if (rdata !== (which ? vin[k] : vout[k])) begin
					if (fails < 8) $display("FAIL: vector %0d word %0d: %08x, want %08x", n, k, rdata, which ? vin[k] : vout[k]);
					fails = fails + 1;
				end
			end
		end
	endtask

	initial begin
		now = 0; fails = 0; worst = 0; nvec = 0;
		repeat (4) @(posedge clk);
		resetn = 1;
		@(posedge clk);

		wbr(0); if (rdata !== 32'h5A4B4543) begin $display("FAIL: MAGIC %08x", rdata); fails = fails + 1; end
		wbr(2); if (rdata !== 32'h4B450001) begin $display("FAIL: CONFIG %08x", rdata); fails = fails + 1; end

		fd = $fopen("keccak_vectors.txt", "r");
		if (fd == 0) begin $display("FAIL: no keccak_vectors.txt"); $finish; end
		n = 0;
		while (!$feof(fd)) begin
			rc = 0;
			for (k = 0; k < 50; k = k + 1) rc = rc + $fscanf(fd, "%h", vin[k]);
			for (k = 0; k < 50; k = k + 1) rc = rc + $fscanf(fd, "%h", vout[k]);
			if (rc == 100) begin
				for (k = 0; k < 50; k = k + 1) wbw(4, vin[k]);			// IN
				check_out(1);											// read back unchanged
				t0 = now;
				wbw(1, 32'd1);											// START
				if (n == 5) wbw(4, 32'hDEADBEEF);						// while busy: ignored
				if (n == 7) wbw(1, 32'd2);								// CLEAR while busy: ignored too
				polls = 0;
				wbr(1);
				while (rdata[0]) begin polls = polls + 1; wbr(1); end
				cyc_perm = now - t0;
				if (cyc_perm > worst) worst = cyc_perm;
				check_out(0);
				check_out(0);											// OUT left it as it was
				// XIN: the permuted state xor the input words; then back
				for (k = 0; k < 50; k = k + 1) wbw(5, vin[k]);
				for (k = 0; k < 50; k = k + 1) begin
					wbr(6);
					if (rdata !== (vout[k] ^ vin[k])) begin
						if (fails < 8) $display("FAIL: XIN, vector %0d word %0d", n, k);
						fails = fails + 1;
					end
				end
				for (k = 0; k < 50; k = k + 1) wbw(5, vin[k]);
				check_out(0);
				nvec = nvec + 1;
				n = n + 1;
			end
		end
		$fclose(fd);

		// CLEAR
		wbw(1, 32'd2);
		for (k = 0; k < 50; k = k + 1) begin
			wbr(6);
			if (rdata !== 32'd0) begin if (fails < 8) $display("FAIL: CLEAR left %08x at word %0d", rdata, k); fails = fails + 1; end
		end

		// the claim
		wbw(3, 32'd5);  wbr(3); if (rdata !== 32'd5) begin $display("FAIL: claim"); fails = fails + 1; end
		wbw(3, 32'd7);  wbr(3); if (rdata !== 32'd5) begin $display("FAIL: a second claim took it"); fails = fails + 1; end
		wbw(3, 32'h80000007); wbr(3); if (rdata !== 32'd5) begin $display("FAIL: another's release freed it"); fails = fails + 1; end
		wbw(3, 32'h80000005); wbr(3); if (rdata !== 32'd0) begin $display("FAIL: release"); fails = fails + 1; end

		$display("keccak: %0d permutations against the reference, OUT/XIN/CLEAR/busy/claim checked, %0d failed; <= %0d cycles a permutation, bus included",
			nvec, fails, worst);
		$finish;
	end
endmodule
