/*
 * tb_ddr3_rdasm -- exercises rtl/mem/ddr3_rdasm.v
 *
 * Every beat of every burst is distinct, so a burst shifted by one
 * beat in either direction cannot pass. (A pattern whose halves match
 * hides exactly that shift -- which is how two cancelling one-beat
 * errors survived in the controller testbench's doubles.)
 *
 * For each misalignment -- the burst starting 0 to 3 beats into a
 * cycle, and lane 1 arriving up to 2 cycles after lane 0 -- it sweeps
 * both offsets and requires:
 *
 *   * exactly ONE offset per lane assembles the burst correctly;
 *   * every offset selects a DIFFERENT window (the knob reaches).
 *
 * The second is the check that was missing last time, when four
 * separate controls turned out to be connected to nothing and each
 * produced a clean, confident, uniform map.
 */

`timescale 1ns/1ps
`default_nettype none

module tb_ddr3_rdasm;

	reg clk = 0;
	always #5 clk = ~clk;

	reg rst = 1;
	reg cap = 0;
	reg [15:0] q0, q1, q2, q3;
	reg [3:0] off0, off1;
	wire [127:0] rdata;
	wire rvalid;

	ddr3_rdasm dut (
		.clk_i(clk), .rst_i(rst), .cap_i(cap),
		.q0_i(q0), .q1_i(q1), .q2_i(q2), .q3_i(q3),
		.off0_i(off0), .off1_i(off1),
		.rdata_o(rdata), .rvalid_o(rvalid)
	);

	// Beat b of lane L carries a value nothing else does.
	function [7:0] beatval(input integer lane, input integer b);
		beatval = (lane ? 8'hb0 : 8'ha0) + b[7:0];
	endfunction

	// The lane's beat stream: burst beat b appears at absolute beat
	// position start + b; everything else is filler that differs from
	// every burst beat.
	function [7:0] stream(input integer lane, input integer pos,
	                      input integer start);
		if (pos >= start && pos < start + 8)
			stream = beatval(lane, pos - start);
		else
			stream = 8'h5a ^ pos[7:0];
	endfunction

	integer errors = 0;
	integer cyc, lane, s, skew, o0, o1, good0, good1, b, nwin;
	reg [63:0] got0, got1, want0, want1;
	reg [63:0] seen [0:15];
	integer k, dup, dup1;
	integer c, st0, st1;
	reg [63:0] seen1 [0:15];

	// Run one burst: lane 0 starts at beat position 8+s, lane 1 skew
	// cycles later. The capture is a single pulse at a FIXED cycle,
	// as the PHY now does, so a lane arriving later needs a larger
	// offset -- four beats per cycle.
	task run(input integer s_, input integer skew_,
	         input integer o0_, input integer o1_);
		begin
			off0 = o0_[3:0]; off1 = o1_[3:0];
			st0 = 8 + s_;
			st1 = 8 + s_ + skew_ * 4;
			for (c = 0; c < 12; c = c + 1) begin
				q0 = {stream(1, c*4+0, st1), stream(0, c*4+0, st0)};
				q1 = {stream(1, c*4+1, st1), stream(0, c*4+1, st0)};
				q2 = {stream(1, c*4+2, st1), stream(0, c*4+2, st0)};
				q3 = {stream(1, c*4+3, st1), stream(0, c*4+3, st0)};
				cap = (c == 7);
				@(negedge clk);
			end
			cap = 0;
			repeat (4) @(negedge clk);
		end
	endtask

	// Capture the result of a run.
	reg [127:0] last;
	always @(posedge clk) if (rvalid) last <= rdata;

	initial begin
		q0 = 0; q1 = 0; q2 = 0; q3 = 0; off0 = 0; off1 = 0;
		repeat (4) @(negedge clk);
		rst = 0;

		for (b = 0; b < 8; b = b + 1) begin
			want0[b*8 +: 8] = beatval(0, b);
			want1[b*8 +: 8] = beatval(1, b);
		end

		$display("offset that assembles each lane, per misalignment:");
		for (skew = 0; skew < 3; skew = skew + 1)
		for (s = 0; s < 4; s = s + 1) begin
			good0 = -1; good1 = -1;
			for (o0 = 0; o0 < 16; o0 = o0 + 1) begin
				run(s, skew, o0, o0);
				for (b = 0; b < 8; b = b + 1) begin
					got0[b*8 +: 8] = last[b*16 +: 8];
					got1[b*8 +: 8] = last[b*16 + 8 +: 8];
				end
				if (got0 == want0) begin
					if (good0 >= 0) begin
						$display("  FAIL: two offsets work for lane 0");
						errors = errors + 1;
					end
					good0 = o0;
				end
				if (got1 == want1) begin
					if (good1 >= 0) begin
						$display("  FAIL: two offsets work for lane 1");
						errors = errors + 1;
					end
					good1 = o0;
				end
				seen[o0] = got0;
				seen1[o0] = got1;
			end

			// The knob reaches: sixteen settings, sixteen windows.
			dup = 0;
			for (o0 = 0; o0 < 16; o0 = o0 + 1)
				for (k = o0 + 1; k < 16; k = k + 1) begin
					if (seen[o0] == seen[k]) dup = dup + 1;
					if (seen1[o0] == seen1[k]) dup = dup + 1;
				end

			$display("  start+%0d, lane1 +%0d cycles:  lane0 off=%0d  lane1 off=%0d",
				s, skew, good0, good1);
			if (dup) $display("  FAIL: offsets alias");
			if (good0 < 0 || good1 < 0) begin
				$display("  FAIL: no offset assembles a lane");
				errors = errors + 1;
			end
			if (dup) errors = errors + 1;
		end

		$display("");
		if (errors == 0)
			$display("== PASS: every misalignment has exactly one offset, per lane ==");
		else
			$display("== FAIL: %0d error(s) ==", errors);
		$finish;
	end

endmodule

`default_nettype wire
