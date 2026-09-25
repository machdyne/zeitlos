/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DDR3 PHY bring-up sequencer (ECP5)
 *
 * This module owns the ECP5's DDR delay hardware: it runs the DDRDLL,
 * and it performs the fixed stop/reset/update dance that the edge
 * clock and the DQS buffers require before any DDR3 traffic can be
 * trusted. It issues NO DRAM commands -- rtl/mem/ddr3.v does that,
 * and waits for `ready` here before it starts.
 *
 * -- what the DDRDLL is for --
 *
 * Capturing read data on ECP5 means delaying DQS by 90 degrees so its
 * edges land in the middle of the data eye rather than on its edges.
 * That delay is a analogue property of the silicon and drifts with
 * voltage and temperature, so it cannot be a fixed constant. DDRDLLA
 * measures it continuously against a known clock and emits DDRDEL, a
 * digital delay code that every DQSBUFM consumes. Getting this wrong
 * does not fail cleanly -- it reads plausible-looking corrupt data.
 *
 * -- why this runs on its own clock --
 *
 * THE IMPORTANT DETAIL IN THIS FILE. The sequence below asserts
 * eclk_stop and eclk_reset, which stop and reset the edge clock
 * domain. On a `MEM_DDR3 board, sys_clk is a CLKDIVF of that very
 * edge clock (see rtl/sysctl.v), so stopping the ECLK stops sys_clk
 * with it. A sequencer clocked from sys_clk would therefore stop
 * itself half way through and hang the machine at power-on, every
 * time, with no obvious cause.
 *
 * So clk_i here is the "init" clock: rtl/clk/pll2.v's CLKOS2, taken
 * STRAIGHT off the PLL, never divided and never stopped. It happens
 * to also be 48MHz, which makes it easy to mistake for sys_clk in a
 * waveform viewer. It is not the same net and must not be.
 *
 * Because the two domains are unrelated while the ECLK is stopped,
 * `ready` is the only signal leaving here for the sys domain, and
 * rtl/sysctl.v synchronises it before use.
 *
 * -- the sequence --
 *
 * Ten evenly spaced steps, an order that is NOT arbitrary: the DLL is
 * frozen before the clock it measures is disturbed, the clock is
 * stopped before it is reset, and the DQS buffers are paused before
 * the new delay code is loaded into them. Each step is given STEP
 * cycles, far longer than the hardware needs, because this runs once
 * at power-on and costs about 18us total -- not worth tightening.
 */

`default_nettype none

module ddr3_phy_init #(
	// Cycles per step of the bring-up sequence, in clk_i ticks.
	parameter STEP = 8
) (
	input wire clk_i,          // free-running init clock (NOT sys_clk)
	input wire rst_i,

	input wire eclk_i,         // 96MHz edge clock, drives the DDRDLL

	output wire ddrdel_o,      // delay code -> every DQSBUFM
	output wire eclk_stop_o,   // -> ECLKSYNCB STOP
	output wire eclk_reset_o,  // -> edge clock domain reset
	output wire pause_o,       // -> DQSBUFM PAUSE
	output wire ready_o        // 1 once the PHY may be used
);

	wire dll_lock;
	wire ddrdel;

	reg freeze;
	reg update;
	reg eclk_stop;
	reg eclk_reset;
	reg pause;
	reg ready;

	reg lock_s0;
	reg lock_s1;
	reg lock_d;
	reg running;
	reg [9:0] step_cnt;

	assign ddrdel_o     = ddrdel;
	assign eclk_stop_o  = eclk_stop;
	assign eclk_reset_o = eclk_reset;
	assign pause_o      = pause;
	assign ready_o      = ready;

	// UDDCNTLN is active low: held high (no update) except during the
	// one step where the freshly measured code is pushed out.
	DDRDLLA ddrdll_i (
		.RST(rst_i),
		.CLK(eclk_i),
		.UDDCNTLN(~update),
		.FREEZE(freeze),
		.DDRDEL(ddrdel),
		.LOCK(dll_lock)
	);

	// LOCK is generated in the edge clock domain; bring it across
	// before any decision is made on it.
	always @(posedge clk_i) begin
		if (rst_i) begin
			lock_s0 <= 1'b0;
			lock_s1 <= 1'b0;
			lock_d <= 1'b0;
		end else begin
			lock_s0 <= dll_lock;
			lock_s1 <= lock_s0;
			lock_d <= lock_s1;
		end
	end

	always @(posedge clk_i) begin

		if (rst_i) begin

			freeze <= 1'b0;
			update <= 1'b0;
			eclk_stop <= 1'b0;
			eclk_reset <= 1'b0;
			pause <= 1'b0;
			ready <= 1'b0;
			running <= 1'b0;
			step_cnt <= 10'd0;

		end else if (!running) begin

			// Start on the RISING edge of lock, so a DLL that loses
			// lock and regains it re-runs the sequence rather than
			// leaving the DQS buffers holding a stale delay code.
			if (lock_s1 && !lock_d) begin
				running <= 1'b1;
				ready <= 1'b0;
				step_cnt <= 10'd0;
			end

		end else begin

			step_cnt <= step_cnt + 10'd1;

			// Freeze the DLL before disturbing the clock it measures.
			if (step_cnt == (1*STEP)) freeze <= 1'b1;
			// Stop the edge clock, then reset it, then release in the
			// reverse order. Reset while running would be a glitch.
			if (step_cnt == (2*STEP)) eclk_stop <= 1'b1;
			if (step_cnt == (3*STEP)) eclk_reset <= 1'b1;
			if (step_cnt == (4*STEP)) eclk_reset <= 1'b0;
			if (step_cnt == (5*STEP)) eclk_stop <= 1'b0;
			if (step_cnt == (6*STEP)) freeze <= 1'b0;
			// Pause the DQS buffers across the delay code update, so
			// they never observe a half-changed value.
			if (step_cnt == (7*STEP)) pause <= 1'b1;
			if (step_cnt == (8*STEP)) update <= 1'b1;
			if (step_cnt == (9*STEP)) update <= 1'b0;
			if (step_cnt == (10*STEP)) begin
				pause <= 1'b0;
				running <= 1'b0;
				ready <= 1'b1;
			end

		end

	end

endmodule

`default_nettype wire
