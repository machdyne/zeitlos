/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DDR3 read burst assembly
 *
 * Takes the four beats per system clock that each DQ input register
 * delivers and assembles one BL8 burst -- eight beats of sixteen bits,
 * 128 bits -- for the controller.
 *
 * Kept apart from the PHY on purpose. DQSBUFM and IDDRX2DQA have no
 * simulation models, so anything tangled up with them is observable
 * only through a hardware build and a line of console output. This is
 * ordinary synchronous logic, and every fault found in this part of
 * the read path last time was an off-by-one: exactly what a testbench
 * finds in seconds (rtl/tb/tb_ddr3_rdasm.v).
 *
 * -- captured at a FIXED latency, not on DATAVALID --
 *
 * The capture instant is a fixed number of cycles after the READ
 * command (cap_i, from the PHY's read pipe), exactly as LiteDRAM's
 * ECP5 PHY does -- the one datapath that has read correctly on this
 * board. It takes read data a fixed 13 cycles after the read enable
 * and never consults DATAVALID.
 *
 * This module used to frame each lane on its own DATAVALID. On
 * hardware DATAVALID fired at gates where BURSTDET had seen no strobe
 * at all, so reads "completed" with nothing in them. The pipeline from
 * command to input register is deterministic; the capture point can be
 * too.
 *
 * -- one knob per lane, and it reaches --
 *
 * Every input beat enters a history, newest first. At the capture
 * instant each lane takes eight beats out of it at its own offset:
 *
 *     burst beat b  =  history[off + 7 - b]
 *
 * One number per lane covers the bit slip within a cycle AND whole
 * cycles either side, in single-beat steps, so the two lanes can
 * arrive at different times -- they are separate strobes, pads and
 * FIFOs. It is four bits and the history is deep enough for all
 * sixteen values: every setting selects a different window.
 */

`default_nettype none

module ddr3_rdasm #(
	// Beats of history kept: enough that off = 15 still reaches the
	// eighth beat of the burst.
	parameter HIST = 32
) (
	input wire clk_i,
	input wire rst_i,

	// Capture now: the read enable, a fixed latency later.
	input wire cap_i,

	// Four beats per cycle from the input registers, Q0 first.
	input wire [15:0] q0_i,
	input wire [15:0] q1_i,
	input wire [15:0] q2_i,
	input wire [15:0] q3_i,

	// Beat offset into the history, per lane.
	input wire [3:0] off0_i,
	input wire [3:0] off1_i,

	output reg [127:0] rdata_o,
	output reg rvalid_o
);

	// -- history, per lane -----------------------------------------
	//
	// hist[i] is the beat that arrived i beats ago. Within a cycle Q0
	// is the oldest, so it lands at the highest index of the four.
	reg [7:0] hist0 [0:HIST-1];
	reg [7:0] hist1 [0:HIST-1];

	integer h;
	always @(posedge clk_i) begin
		for (h = HIST - 1; h >= 4; h = h - 1) begin
			hist0[h] <= hist0[h - 4];
			hist1[h] <= hist1[h - 4];
		end
		hist0[3] <= q0_i[7:0];   hist1[3] <= q0_i[15:8];
		hist0[2] <= q1_i[7:0];   hist1[2] <= q1_i[15:8];
		hist0[1] <= q2_i[7:0];   hist1[1] <= q2_i[15:8];
		hist0[0] <= q3_i[7:0];   hist1[0] <= q3_i[15:8];
	end

	// -- capture ------------------------------------------------------
	integer b;
	always @(posedge clk_i) begin
		rvalid_o <= 1'b0;
		if (rst_i) begin
			rvalid_o <= 1'b0;
		end else if (cap_i) begin
			// Beat 0 lowest; each lane's byte of each beat from its own
			// offset into its own history.
			for (b = 0; b < 8; b = b + 1) begin
				rdata_o[b*16 +: 8]     <= hist0[off0_i + 7 - b];
				rdata_o[b*16 + 8 +: 8] <= hist1[off1_i + 7 - b];
			end
			rvalid_o <= 1'b1;
		end
	end

endmodule

`default_nettype wire
