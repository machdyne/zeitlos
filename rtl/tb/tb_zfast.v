/*
 * Zeitlos SOC
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Testbench for rtl/zfast.v, fast zlink: two blocks, as on two boards,
 * joined by a modelled SATA cable.
 *
 *   make test_zfast                   # +300 ppm, 0.5 ns of jitter
 *   make test_zfast PPM=-1000 JIT=1.5
 *
 * Each "board" has its own clocks -- bus, 100 and 200 MHz -- and B's run
 * PPM parts per million off A's, as two crystals do. The cable carries
 * DS1 and DS2 (each one LVDS pair, modelled as one wire), straight or
 * crossed, optionally inverting a pair, with JIT ns of random delay on
 * every edge. A drives like a Sechzig ML0/ML1 (TX_DS2 = 1); B like an ML2
 * (TX_DS2 = 0: it can only transmit on DS1). If both ends ever drive one
 * pair at once, the test fails then and there.
 *
 * What it covers:
 *   1. FCTL reads back, TX_DS2 shows, and B refuses to drive DS2.
 *   2. A alone, internal loopback: data and K symbols.
 *   3. Straight cable at 100, 50 and 25 Mbit/s, both ways at once: A on
 *      DS2, B on DS1 -- the only way an ML2 and an ML1 meet.
 *   4. Crossed cable: both on DS1.
 *   5. An inverted pair, undone with RXINV.
 *   6. A burst of noise on the wire: errors counted, the link clean
 *      again after the next comma.
 *   7. RX overflow, levels, flush.
 */

`timescale 1ns / 1ps

`ifndef PPM
`define PPM 300
`endif
`ifndef JIT
`define JIT 0.5
`endif

module tb_zfast;

	real ppm = `PPM;
	real jit = `JIT;

	// -- clocks --

	reg a48 = 0, a100 = 0, a200 = 0;
	reg b48 = 0, b100 = 0, b200 = 0;
	real ta200 = 2.5, tb200, ta48 = 10.4167, tb48;

	initial begin
		tb200 = 2.5 * (1.0 + ppm * 1e-6);
		tb48 = 10.4167 * (1.0 + ppm * 1e-6);
	end
	always #(ta200) a200 = ~a200;
	always #(tb200) b200 = ~b200;
	always @(posedge a200) a100 <= ~a100;	// aligned to the 200, as from one PLL
	always @(posedge b200) b100 <= ~b100;
	always #(ta48) a48 = ~a48;
	always #(tb48) b48 = ~b48;

	reg rst = 1;

	// -- the two blocks --

	reg a_acc = 0, a_we = 0, b_acc = 0, b_we = 0;
	reg [3:0] a_ra = 0, b_ra = 0;
	reg [31:0] a_wd = 0, b_wd = 0;
	wire a_done, b_done;
	wire [31:0] a_rdo, b_rdo;
	wire a1, a2, b1, b2;		// each end's own pads

	zfast #(.TX_DS2(1)) A (
		.clk(a48), .rst(rst), .acc(a_acc), .we(a_we), .ra(a_ra), .wd(a_wd),
		.done(a_done), .rdo(a_rdo),
		.clk100(a100), .clk200(a200), .fclk_ok(1'b1), .ds1(a1), .ds2(a2));
	zfast #(.TX_DS2(0)) B (
		.clk(b48), .rst(rst), .acc(b_acc), .we(b_we), .ra(b_ra), .wd(b_wd),
		.done(b_done), .rdo(b_rdo),
		.clk100(b100), .clk200(b200), .fclk_ok(1'b1), .ds1(b1), .ds2(b2));

	// -- the cable --
	//
	// What each end drives, as the pad model drives it, and whether it is
	// driving; the far end sees it JIT-delayed. An undriven pair reads 1
	// (a terminated LVDS input with nothing on it is undefined; a steady
	// level is the harmless case).

	reg crossed = 0;	// A's DS1 is B's DS2
	reg inv_ab = 0;		// the pair A transmits on arrives inverted at B
	reg noise = 0;		// garbage on every pair

	wire a1_dr = A.p1.oe_r, a2_dr = A.p2.oe_r, b1_dr = B.p1.oe_r, b2_dr = B.p2.oe_r;
	wire a1_v = A.p1.od, a2_v = A.p2.od, b1_v = B.p1.od, b2_v = B.p2.od;

	// the far end's drive on each of A's and B's pins, before delay
	wire to_a1_dr = crossed ? b2_dr : b1_dr, to_a1_v = crossed ? b2_v : b1_v;
	wire to_a2_dr = crossed ? b1_dr : b2_dr, to_a2_v = crossed ? b1_v : b2_v;
	wire to_b1_dr = crossed ? a2_dr : a1_dr, to_b1_v = (crossed ? a2_v : a1_v) ^ inv_ab;
	wire to_b2_dr = crossed ? a1_dr : a2_dr, to_b2_v = (crossed ? a1_v : a2_v) ^ inv_ab;

	reg [3:0] far_dr = 0, far_v = 4'hF;
	always @(to_a1_dr or to_a1_v) far_dr[0] <= #(1.0 + jit * ($random & 255) / 256.0) to_a1_dr;
	always @(to_a1_dr or to_a1_v) far_v[0]  <= #(1.0 + jit * ($random & 255) / 256.0) to_a1_v;
	always @(to_a2_dr or to_a2_v) far_dr[1] <= #(1.0 + jit * ($random & 255) / 256.0) to_a2_dr;
	always @(to_a2_dr or to_a2_v) far_v[1]  <= #(1.0 + jit * ($random & 255) / 256.0) to_a2_v;
	always @(to_b1_dr or to_b1_v) far_dr[2] <= #(1.0 + jit * ($random & 255) / 256.0) to_b1_dr;
	always @(to_b1_dr or to_b1_v) far_v[2]  <= #(1.0 + jit * ($random & 255) / 256.0) to_b1_v;
	always @(to_b2_dr or to_b2_v) far_dr[3] <= #(1.0 + jit * ($random & 255) / 256.0) to_b2_dr;
	always @(to_b2_dr or to_b2_v) far_v[3]  <= #(1.0 + jit * ($random & 255) / 256.0) to_b2_v;

	reg nz = 0;
	always #(1.7) if (noise) nz = $random;

	assign a1 = (!a1_dr && far_dr[0]) ? (noise ? nz : far_v[0]) : 1'bz;
	assign a2 = (!a2_dr && far_dr[1]) ? (noise ? nz : far_v[1]) : 1'bz;
	assign b1 = (!b1_dr && far_dr[2]) ? (noise ? nz : far_v[2]) : 1'bz;
	assign b2 = (!b2_dr && far_dr[3]) ? (noise ? nz : far_v[3]) : 1'bz;
	pullup(a1); pullup(a2); pullup(b1); pullup(b2);

	integer errors = 0;

	// both ends driving one pair: the thing that must never happen
	wire w1_both = crossed ? (a1_dr && b2_dr) : (a1_dr && b1_dr);
	wire w2_both = crossed ? (a2_dr && b1_dr) : (a2_dr && b2_dr);
	always @(posedge a200) if (!rst && (w1_both || w2_both)) begin
		$display("FAIL: both ends drive one pair at %t", $time);
		errors = errors + 1;
	end

	// -- the buses --

	reg [31:0] rd;

	task a_bus(input w, input [3:0] r, input [31:0] d);
	begin
		@(posedge a48);
		a_acc <= 1; a_we <= w; a_ra <= r; a_wd <= d;
		@(posedge a48);
		while (!a_done) @(posedge a48);
		rd = a_rdo;
		a_acc <= 0;
		@(posedge a48);
	end
	endtask

	task b_bus(input w, input [3:0] r, input [31:0] d);
	begin
		@(posedge b48);
		b_acc <= 1; b_we <= w; b_ra <= r; b_wd <= d;
		@(posedge b48);
		while (!b_done) @(posedge b48);
		rd = b_rdo;
		b_acc <= 0;
		@(posedge b48);
	end
	endtask

	task check(input [639:0] what, input [31:0] got, input [31:0] want);
	begin
		if (got !== want) begin
			$display("FAIL: %0s: got 0x%08x, want 0x%08x", what, got, want);
			errors = errors + 1;
		end else
			$display("ok:   %0s = 0x%08x", what, got);
	end
	endtask

	// FCTL fields
	function [31:0] fctl;
		input [1:0] txsel;
		input rxsel;
		input [1:0] rate;
		input loop;
		input rxinv;
		fctl = 32'd1 | (txsel << 1) | (rxsel << 3) | (1 << 4) | (loop << 5) |
		       (rate << 6) | (rxinv << 9);
	endfunction

	// N bytes of a pattern each way, at once, and check what arrives
	reg [7:0] pa [0:4095];
	reg [7:0] pb [0:4095];
	integer ia, ib, ga, gb;

	task wait_aligned;
		integer n;
	begin
		n = 0;
		rd = 0;
		while (!(rd[14]) && n < 400) begin
			a_bus(0, 3, 0);
			if (rd[14]) b_bus(0, 3, 0);
			n = n + 1;
		end
		if (n == 400) begin
			$display("FAIL: never aligned");
			errors = errors + 1;
		end
	end
	endtask

	reg [639:0] cur;
	integer lv, k;
	task take_a(input [7:0] v);
	begin
		if (v !== pb[ga]) begin
			if (errors < 20) $display("FAIL: %0s: A got 0x%02x at %0d, want 0x%02x", cur, v, ga, pb[ga]);
			errors = errors + 1;
		end
		ga = ga + 1;
	end
	endtask
	task take_b(input [7:0] v);
	begin
		if (v !== pa[gb]) begin
			if (errors < 20) $display("FAIL: %0s: B got 0x%02x at %0d, want 0x%02x", cur, v, gb, pa[gb]);
			errors = errors + 1;
		end
		gb = gb + 1;
	end
	endtask

	task both_ways(input [639:0] what, input integer n, input [7:0] seed);
		integer i, t;
	begin
		cur = what;
		for (i = 0; i < n; i = i + 1) begin
			pa[i] = (i * 7 + seed) ^ (i >> 3);
			pb[i] = (i * 13 + seed + 1) ^ (i >> 2);
		end
		ia = 0; ib = 0; ga = 0; gb = 0;
		b_bus(1, 9, 32'd7);		// flush everything, both ends
		a_bus(1, 9, 32'd7);
		t = 0;
		while ((ga < n || gb < n) && t < 20000) begin
			// A sends four, B sends four, each reads what it has
			if (ia < n) begin a_bus(1, 5, { pa[ia+3], pa[ia+2], pa[ia+1], pa[ia] }); ia = ia + 4; end
			if (ib < n) begin b_bus(1, 5, { pb[ib+3], pb[ib+2], pb[ib+1], pb[ib] }); ib = ib + 4; end
			// read all that is waiting, four at a time when there are four
			a_bus(0, 3, 0);
			lv = rd[26:16];
			while (lv != 0) begin
				if (lv >= 4) begin
					a_bus(0, 7, 0);
					for (k = 0; k < 4; k = k + 1) take_a(rd[k*8 +: 8]);
					lv = lv - 4;
				end else begin
					a_bus(0, 6, 0);
					take_a(rd[7:0]);
					lv = lv - 1;
				end
			end
			b_bus(0, 3, 0);
			lv = rd[26:16];
			while (lv != 0) begin
				if (lv >= 4) begin
					b_bus(0, 7, 0);
					for (k = 0; k < 4; k = k + 1) take_b(rd[k*8 +: 8]);
					lv = lv - 4;
				end else begin
					b_bus(0, 6, 0);
					take_b(rd[7:0]);
					lv = lv - 1;
				end
			end
			t = t + 1;
		end
		check({ what, ": A received everything" }, ga, n);
		check({ what, ": B received everything" }, gb, n);
		a_bus(0, 8, 0);
		check({ what, ": no code errors at A" }, rd, 0);
		b_bus(0, 8, 0);
		check({ what, ": no code errors at B" }, rd, 0);
	end
	endtask

	integer i;

	initial begin
		repeat (20) @(posedge a48);
		rst = 0;
		repeat (20) @(posedge a48);

		// 1. registers
		a_bus(1, 0, fctl(2'd2, 1'b0, 2'd0, 1'b0, 1'b0));
		a_bus(0, 0, 0);
		check("A: FCTL, and DS2 can transmit", rd, fctl(2'd2, 1'b0, 2'd0, 1'b0, 1'b0) | (1 << 16));
		a_bus(1, 0, 0);
		b_bus(1, 0, fctl(2'd2, 1'b0, 2'd0, 1'b0, 1'b0));
		b_bus(0, 0, 0);
		check("B (an ML2): asked for DS2, drives nothing", rd, fctl(2'd0, 1'b0, 2'd0, 1'b0, 1'b0));
		b_bus(1, 0, 0);
		b_bus(0, 10, 0);
		check("FLOCK: free, then taken", rd, 0);
		b_bus(0, 10, 0);
		check("FLOCK: now taken", rd, 1);

		// 2. A alone, inside
		a_bus(1, 0, fctl(2'd0, 1'b0, 2'd0, 1'b1, 1'b0));
		repeat (200) @(posedge a100);
		a_bus(1, 9, 32'd7);
		a_bus(1, 4, 32'h1FB);			// K27.7
		a_bus(1, 5, 32'h44332211);
		a_bus(1, 4, 32'h1FD);			// K29.7
		repeat (800) @(posedge a100);
		a_bus(0, 3, 0);
		check("loopback: six received", rd[26:16], 6);
		a_bus(0, 6, 0); check("loopback: K27.7", rd, 32'h3FB);
		a_bus(0, 7, 0); check("loopback: four bytes (FRX4)", rd, 32'h44332211);
		a_bus(0, 6, 0); check("loopback: K29.7", rd, 32'h3FD);
		a_bus(0, 6, 0); check("loopback: empty", rd, 0);

		// 3. straight cable: A transmits on DS2 and listens on DS1; B
		//    (an ML2) transmits on DS1 and listens on DS2
		a_bus(1, 0, fctl(2'd2, 1'b0, 2'd0, 1'b0, 1'b0));
		b_bus(1, 0, fctl(2'd1, 1'b1, 2'd0, 1'b0, 1'b0));
		wait_aligned;
		both_ways("straight, 100 Mbit/s", 2048, 8'd1);
		a_bus(1, 0, fctl(2'd2, 1'b0, 2'd1, 1'b0, 1'b0));
		b_bus(1, 0, fctl(2'd1, 1'b1, 2'd1, 1'b0, 1'b0));
		wait_aligned;
		both_ways("straight, 50 Mbit/s", 1024, 8'd2);
		a_bus(1, 0, fctl(2'd2, 1'b0, 2'd2, 1'b0, 1'b0));
		b_bus(1, 0, fctl(2'd1, 1'b1, 2'd2, 1'b0, 1'b0));
		wait_aligned;
		both_ways("straight, 25 Mbit/s", 512, 8'd3);

		// 4. crossed cable: both on DS1, listening on DS2
		a_bus(1, 0, 0);
		b_bus(1, 0, 0);
		repeat (20) @(posedge a100);
		crossed = 1;
		a_bus(1, 0, fctl(2'd1, 1'b1, 2'd0, 1'b0, 1'b0));
		b_bus(1, 0, fctl(2'd1, 1'b1, 2'd0, 1'b0, 1'b0));
		wait_aligned;
		both_ways("crossed, 100 Mbit/s", 2048, 8'd4);

		// 5. a pair the wrong way round (P and N swapped), undone at B
		inv_ab = 1;
		b_bus(1, 0, fctl(2'd1, 1'b1, 2'd0, 1'b0, 1'b1));
		wait_aligned;
		both_ways("an inverted pair, RXINV at B", 1024, 8'd5);
		inv_ab = 0;
		b_bus(1, 0, fctl(2'd1, 1'b1, 2'd0, 1'b0, 1'b0));
		wait_aligned;

		// 6. noise on the wire for 2 us: errors, then clean
		b_bus(1, 9, 32'd7);
		noise = 1;
		#2000;
		noise = 0;
		#2000;
		b_bus(0, 8, 0);
		if (rd == 0) begin
			$display("FAIL: noise left no code errors");
			errors = errors + 1;
		end else
			$display("ok:   noise counted as %0d code errors", rd);
		wait_aligned;
		both_ways("after the noise", 1024, 8'd6);

		// 7. B not reading: its RX FIFO fills at 1024 and says so
		b_bus(1, 9, 32'd7);
		for (i = 0; i < 1100; i = i + 4) a_bus(1, 5, i);
		repeat (20000) @(posedge a100);
		b_bus(0, 3, 0);
		check("B: RX level 1024, overrun", { rd[26:16], rd[12] }, { 11'd1024, 1'b1 });
		b_bus(1, 9, 32'd6);
		b_bus(0, 3, 0);
		check("B: flushed", { rd[26:16], rd[12] }, 12'd0);

		if (errors == 0) $display("tb_zfast: PASS (PPM=%0d, JIT=%0.2f ns)", `PPM, jit);
		else $display("tb_zfast: %0d FAILED", errors);
		$finish;
	end

	initial begin
		#20000000;
		$display("FAIL: timeout");
		$finish;
	end

endmodule
