/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/montmul.v's register file and commands, against
 * a Python model (gen_montmul_rf_vectors.py): random programs of MUL,
 * ADD and SUB over sixteen registers, for P-384, P-256, P-384's scalar
 * field and 2^255-19. Also the claim register, CONFIG, the windowed
 * reads, and the classic interface still working between programs.
 *
 *   iverilog -g2012 -o /tmp/tb tb_montmul_rf.v ../montmul.v && vvp /tmp/tb
 *
 * tb_montmul.v, unchanged, remains the test of the classic interface.
 */

`timescale 1ns/1ps
module tb;
	reg clk = 0, resetn = 0;
	reg [31:0] adr = 0, dat_w = 0;
	reg we = 0, stb = 0, cyc = 0;
	wire [31:0] dat_r;
	wire ack;
	integer guard;

	montmul #(.LIMBS(12)) dut (
		.clk(clk), .resetn(resetn),
		.wb_adr_i(adr), .wb_dat_i(dat_w), .wb_dat_o(dat_r),
		.wb_we_i(we), .wb_sel_i(4'hf), .wb_stb_i(stb),
		.wb_ack_o(ack), .wb_cyc_i(cyc));

	always #10 clk = ~clk;

	initial begin
		#40000000;
		$display("WATCHDOG: simulation did not finish");
		$finish;
	end

	reg [31:0] rdata;

	task wbw(input [31:0] a, input [31:0] d);
		begin
			while (ack) @(posedge clk);
			adr = a; dat_w = d; we = 1; stb = 1; cyc = 1;
			guard = 0;
			while (!ack && guard < 50) begin @(posedge clk); guard = guard + 1; end
			if (guard >= 50) $display("FAIL: write to %0d never acked", a);
			// Drop the request on the ack, as the CPU's bus does.
			// Holding it a cycle longer is a SECOND access: harmless
			// to an ordinary register, and a skipped word to RDATA,
			// which advances on every access -- which is how this
			// testbench's first version loaded every register wrong.
			stb = 0; cyc = 0; we = 0;
			@(posedge clk);
		end
	endtask

	task wbr(input [31:0] a);
		begin
			while (ack) @(posedge clk);
			adr = a; we = 0; stb = 1; cyc = 1;
			guard = 0;
			while (!ack && guard < 50) begin @(posedge clk); guard = guard + 1; end
			if (guard >= 50) $display("FAIL: read of %0d never acked", a);
			rdata = dat_r;
			stb = 0; cyc = 0;
			@(posedge clk);
		end
	endtask

	integer polls, cycles_start, cycles, maxmul, maxadd, maxsub, minmul, minadd, minsub;
	integer fd, rc, ncase, c, r, k, n, ncmd, fails, checked;
	integer now;
	always @(posedge clk) now = now + 1;
	initial now = 0;

	task wait_idle;
		begin
			polls = 0; rdata = 1;
			while (rdata[0] && polls < 3000) begin wbr(7); polls = polls + 1; end
			if (polls >= 3000) begin $display("FAIL: stuck busy"); fails = fails + 1; end
		end
	endtask

	reg [31:0] ni;
	reg [31:0] vn [0:11];
	reg [31:0] init [0:191];
	reg [31:0] want [0:191];
	reg [15:0] cmds [0:255];

	initial begin
		fails = 0; checked = 0; maxmul = 0; maxadd = 0; maxsub = 0;
		minmul = 99999; minadd = 99999; minsub = 99999;
		repeat (4) @(posedge clk);
		resetn = 1;
		repeat (2) @(posedge clk);

		// -- CONFIG: sixteen registers, twelve limbs --
		wbr(2);
		if (rdata[15:8] !== 8'd16 || rdata[7:0] !== 8'd12) begin
			$display("FAIL: CONFIG %08x", rdata); fails = fails + 1; end

		// -- the claim --
		wbr(4); if (rdata !== 0) begin $display("FAIL: owner at reset %0d", rdata); fails = fails + 1; end
		wbw(4, 5); wbr(4); if (rdata !== 5) begin $display("FAIL: claim"); fails = fails + 1; end
		wbw(4, 7); wbr(4); if (rdata !== 5) begin $display("FAIL: a claim over another's"); fails = fails + 1; end
		wbw(4, 32'h80000007); wbr(4); if (rdata !== 5) begin $display("FAIL: released by another"); fails = fails + 1; end
		wbw(4, 32'h80000005); wbr(4); if (rdata !== 0) begin $display("FAIL: release"); fails = fails + 1; end
		wbw(4, 7); wbr(4); if (rdata !== 7) begin $display("FAIL: claim after release"); fails = fails + 1; end
		wbw(4, 32'h80000007);

		// -- the window: write a register, read it back, auto-increment --
		wbw(5, 8'h30);
		for (k = 0; k < 12; k = k + 1) wbw(6, 32'hA0000000 + k);
		wbw(5, 8'h30);
		for (k = 0; k < 12; k = k + 1) begin
			wbr(6);
			if (rdata !== 32'hA0000000 + k) begin
				$display("FAIL: window word %0d = %08x", k, rdata); fails = fails + 1; end
		end
		wbr(5); if (rdata[7:0] !== 8'h3C) begin $display("FAIL: RSEL after 12 reads %02x", rdata[7:0]); fails = fails + 1; end

		fd = $fopen("montmul_rf_vectors.txt", "r");
		if (fd == 0) begin $display("no vectors"); $finish; end
		rc = $fscanf(fd, "%d\n", ncase);

		for (c = 0; c < ncase; c = c + 1) begin
			rc = $fscanf(fd, "%h\n", ni);
			for (k = 0; k < 12; k = k + 1) rc = $fscanf(fd, "%h", vn[k]);
			for (k = 0; k < 192; k = k + 1) rc = $fscanf(fd, "%h", init[k]);
			rc = $fscanf(fd, "%d\n", ncmd);
			for (k = 0; k < ncmd; k = k + 1) rc = $fscanf(fd, "%h", cmds[k]);
			for (k = 0; k < 192; k = k + 1) rc = $fscanf(fd, "%h", want[k]);

			wbw(3, ni);
			for (k = 0; k < 12; k = k + 1) wbw(40 + k, vn[k]);
			for (r = 0; r < 16; r = r + 1) begin
				wbw(5, r * 16);
				for (k = 0; k < 12; k = k + 1) wbw(6, init[r * 12 + k]);
			end

			for (n = 0; n < ncmd; n = n + 1) begin
				cycles_start = now;
				wbw(7, cmds[n]);
				wait_idle;
				cycles = now - cycles_start;
				if (cmds[n][15:12] == 1 && cycles > maxmul) maxmul = cycles;
				if (cmds[n][15:12] == 2 && cycles > maxadd) maxadd = cycles;
				if (cmds[n][15:12] == 3 && cycles > maxsub) maxsub = cycles;
				if (cmds[n][15:12] == 1 && cycles < minmul) minmul = cycles;
				if (cmds[n][15:12] == 2 && cycles < minadd) minadd = cycles;
				if (cmds[n][15:12] == 3 && cycles < minsub) minsub = cycles;
			end

			for (r = 0; r < 16; r = r + 1) begin
				wbw(5, r * 16);
				for (k = 0; k < 12; k = k + 1) begin
					wbr(6);
					checked = checked + 1;
					if (rdata !== want[r * 12 + k]) begin
						$display("FAIL: case %0d r%0d word %0d = %08x, want %08x",
							c, r, k, rdata, want[r * 12 + k]);
						fails = fails + 1;
					end
				end
			end

			// The classic interface still works between programs:
			// 0 * (N-1) is 0. (tb_montmul.v checks it in full.)
			for (k = 0; k < 12; k = k + 1) wbw(16 + k, 0);
			for (k = 0; k < 12; k = k + 1) wbw(28 + k, vn[k] - (k == 0));
			wbw(1, 1);
			wait_idle;
			for (k = 0; k < 12; k = k + 1) begin
				wbr(52 + k);
				if (rdata !== 0) begin $display("FAIL: classic 0 * x, word %0d = %08x", k, rdata); fails = fails + 1; end
			end
		end

		$display("montmul register file: %0d words checked over %0d programs, %0d failed", checked, ncase, fails);
		$display("  cycles per command, bus included: MUL %0d-%0d, ADD %0d-%0d, SUB %0d-%0d", minmul, maxmul, minadd, maxadd, minsub, maxsub);
		// Constant time: each command the same, whatever its operands
		// (a borrow, a carry, a final subtraction or not).
		if (minmul != maxmul || minadd != maxadd || minsub != maxsub) begin
			$display("FAIL: a command's time depends on its operands"); fails = fails + 1;
		end
		$finish;
	end
endmodule
