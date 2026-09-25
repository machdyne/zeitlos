/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DDR3 clocking (ECP5)
 *
 * Everything a DDR3 board needs to make its clocks, in one place, so
 * the top level gains one instantiation rather than five.
 *
 *   pll2        48MHz in -> 96MHz edge clock source, and a free-running
 *               48MHz for the sequencer
 *   ECLKSYNCB   the edge clock, stoppable without glitching
 *   CLKDIVF     the 48MHz SYSTEM clock, divided from the edge clock
 *   phy_init    DDRDLL and the stop/reset/update sequence
 *
 * -- the system clock comes from HERE on a DDR3 board --
 *
 * The PHY hands data between its 96MHz and 48MHz halves every cycle,
 * and that only works if the two are phase-locked. Deriving sys_clk
 * from the edge clock with CLKDIVF makes that true by construction
 * rather than by constraint.
 *
 * The consequence: stopping the edge clock stops sys_clk, which is why
 * the sequencer runs on pll2's second, free-running output. A
 * sequencer clocked from sys_clk would stop itself half way through
 * its own sequence and hang the machine at power-on, every time.
 */

`default_nettype none

module ddr3_clk (
	input wire clk48_i,       // board oscillator
	input wire rst_i,

	output wire sys_clk_o,    // 48MHz, phase-locked to eclk_o
	output wire eclk_o,       // 96MHz DDR edge clock
	output wire ddrdel_o,     // DLL delay code, to every DQSBUFM
	output wire pause_o,      // DQSBUFM PAUSE during updates
	output wire phy_rst_o,    // resets the DDR primitives, ECLK stopped
	output wire ready_o,      // PHY may be used
	output wire locked_o      // PLL locked; sys_clk_o is running
);

	wire eclk_raw, init_clk, pll_locked;
	wire eclk_stop, eclk_reset;

	pll2 pll (
		.clkin(clk48_i),
		.clkout0(eclk_raw),
		.clkout2(init_clk),
		.locked(pll_locked)
	);

	ECLKSYNCB eclksync (
		.ECLKI(eclk_raw),
		.STOP(eclk_stop),
		.ECLKO(eclk_o)
	);

	CLKDIVF #(.DIV("2.0")) clkdiv (
		.CLKI(eclk_o),
		.RST(eclk_reset),
		.ALIGNWD(1'b0),
		.CDIVX(sys_clk_o)
	);

	ddr3_phy_init init (
		.clk_i(init_clk),
		.rst_i(rst_i | ~pll_locked),
		.eclk_i(eclk_o),
		.ddrdel_o(ddrdel_o),
		.eclk_stop_o(eclk_stop),
		.eclk_reset_o(eclk_reset),
		.pause_o(pause_o),
		.ready_o(ready_o)
	);

	assign locked_o = pll_locked;
	assign phy_rst_o = eclk_reset;

endmodule

`default_nettype wire
