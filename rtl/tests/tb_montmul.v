/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/montmul.v, against Montgomery products computed
 * in Python (gen_montmul_vectors.py).
 *
 *   iverilog -g2012 -o /tmp/tb tb_montmul.v ../montmul.v && vvp /tmp/tb
 *
 * Three moduli -- P-384, P-256, and P-384's scalar field n -- because
 * the block is not curve-specific and the scalar field is the one
 * with no fast-reduction form of its own. Corner cases first:
 * (N-1)^2, zero, one, and R mod N, which is where carry and borrow
 * propagation goes wrong.
 *
 * -- what this caught --
 *
 * Every one of these was invisible to inspection and fatal in
 * hardware:
 *
 *   - the multiplier has TWO cycles of latency (registered operands
 *     AND a registered product), not one, so the valid/index flags
 *     needed a second pipeline stage. Without it every accumulate
 *     consumed the previous iteration's product and the whole result
 *     came out X.
 *   - the inner loops read one word past their operand arrays on the
 *     final iteration, which is X in simulation and whatever the
 *     synthesised RAM returns on hardware.
 *   - m = T[0] * n0inv was read the same cycle its operands were
 *     latched, giving a stale product.
 *
 * The testbench had its own share: a read captured the previous
 * transaction's data because it exited its wait loop on a stale ack,
 * and a $fscanf return value was assigned into the loop index, which
 * is a zero-delay infinite loop that even a simulation watchdog
 * cannot interrupt.
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

	// In-simulation watchdog. A killed process loses buffered
	// $display output, so a hang outside must not be how this ends.
	initial begin
		#2000000;
		$display("WATCHDOG: simulation did not finish");
		$finish;
	end

	// Bounded, so a broken slave shows up as a message rather than a
	// simulation that never ends.
	task wbw(input [31:0] a, input [31:0] d);
		begin
			// Wait out the previous ack first. Without this the
			// loop below exits immediately on a stale ack and the
			// read captures the last transaction's data.
			while (ack) @(posedge clk);
			adr = a; dat_w = d; we = 1; stb = 1; cyc = 1;
			guard = 0;
			while (!ack && guard < 50) begin @(posedge clk); guard = guard + 1; end
			if (guard >= 50) $display("FAIL: write to %0d never acked", a);
			@(posedge clk);
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
			@(posedge clk);
			stb = 0; cyc = 0;
			@(posedge clk);
		end
	endtask

	reg [31:0] rdata;
	integer fd, ncase, c, k, polls, fails, worst, bad, rc;
	integer nl;
	reg [31:0] ni;
	reg [31:0] va [0:11], vb [0:11], vn [0:11], vr [0:11];
	reg [31:0] got [0:11], diff [0:11];
	integer borrow, zero, isn;

	initial begin
		fails = 0; worst = 0;
		repeat (4) @(posedge clk);
		resetn = 1;
		repeat (2) @(posedge clk);

		wbr(0);
		if (rdata !== 32'h5A4D4F4E) begin
			$display("FAIL: MAGIC %08x", rdata); fails = fails + 1; end
		wbr(2);
		if (rdata[7:0] !== 8'd12) begin
			$display("FAIL: CONFIG %0d", rdata[7:0]); fails = fails + 1; end

		fd = $fopen("montmul_vectors.txt", "r");
		if (fd == 0) begin $display("no vectors"); $finish; end
		rc = $fscanf(fd, "%d\n", ncase);

		for (c = 0; c < ncase; c = c + 1) begin
			rc = $fscanf(fd, "%d %h\n", nl, ni);
			for (k = 0; k < nl; k = k + 1) rc = $fscanf(fd, "%h", va[k]);
			for (k = 0; k < nl; k = k + 1) rc = $fscanf(fd, "%h", vb[k]);
			for (k = 0; k < nl; k = k + 1) rc = $fscanf(fd, "%h", vn[k]);
			for (k = 0; k < nl; k = k + 1) rc = $fscanf(fd, "%h", vr[k]);

			wbw(2, nl);
			wbw(3, ni);
			for (k = 0; k < nl; k = k + 1) wbw(16 + k, va[k]);
			for (k = 0; k < nl; k = k + 1) wbw(28 + k, vb[k]);
			for (k = 0; k < nl; k = k + 1) wbw(40 + k, vn[k]);

			
			wbw(1, 1);

			polls = 0; rdata = 1;
			while (rdata[0] && polls < 3000) begin
				wbr(1);
				polls = polls + 1;
			end
			if (polls >= 3000) begin
				$display("FAIL case %0d: engine never idle", c);
				fails = fails + 1;
			end
			if (polls > worst) worst = polls;

			if (polls > 200)
				$display("case %0d: %0d polls", c, polls);
			// The block leaves the result congruent mod N and below
			// 2N -- the final conditional subtract is software's job
			// now -- so accept either the reduced value or that plus
			// N. Compared as whole numbers, not word by word, since
			// the two differ everywhere.
			for (k = 0; k < nl; k = k + 1) begin
				wbr(52 + k);
				got[k] = rdata;
			end

			// got - vr should be 0 or N.
			bad = 0;
			borrow = 0;
			for (k = 0; k < nl; k = k + 1) begin
				diff[k] = got[k] - vr[k] - borrow;
				borrow = (got[k] < (vr[k] + borrow)) ? 1 : 0;
			end
			if (borrow != 0) bad = 1;
			zero = 1; isn = 1;
			for (k = 0; k < nl; k = k + 1) begin
				if (diff[k] !== 32'd0) zero = 0;
				if (diff[k] !== vn[k]) isn = 0;
			end
			if (!zero && !isn) bad = 1;
			if (bad) begin
				$display("FAIL case %0d: result not congruent (word0 %08x)",
					c, got[0]);
				fails = fails + 1;
			end
		end

		$display("%s: %0d cases, %0d failures, worst %0d polls",
			fails ? "FAIL" : "ok", ncase, fails, worst);
		$finish;
	end
endmodule
