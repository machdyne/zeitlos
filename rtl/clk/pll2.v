/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * PLL2 -- DDR3 clock generation (ECP5, 48MHz reference)
 *
 * ONLY instantiated when a board defines `MEM_DDR3. Boards without
 * DDR3 never build this file's contents, so nothing that ships today
 * is affected by it.
 *
 * -- why a third PLL rather than reusing pll0/pll1 --
 *
 * The DDR3 PHY needs a 96MHz edge clock (2x the 48MHz system clock;
 * see docs/ddr3.md). Neither existing PLL can produce it, and that is
 * arithmetic rather than a tuning problem: every output of an EHXPLLL
 * is an integer division of ONE shared VCO.
 *
 *   pll0: CLKI_DIV=12 -> 4MHz PFD, CLKFB_DIV=25 -> CLKOP 100MHz,
 *         CLKOP_DIV=6 -> VCO 600MHz.   600/96 = 6.25   NOT INTEGER
 *   pll1: CLKI_DIV=8  -> 6MHz PFD, CLKFB_DIV=21 -> CLKOP 126MHz,
 *         CLKOP_DIV=5 -> VCO 630MHz.   630/96 = 6.5625 NOT INTEGER
 *
 * Retuning pll0 to a 576MHz VCO WOULD give 96 (/6), 48 (/12) and 12
 * (/48) cleanly -- but not 50 (576/50 = 11.52), and 50MHz is the RMII
 * reference. Mozart survives that because its ETH_REFCLK is an input
 * from a board oscillator, but Sergei defines `ETH_RMII_DRIVE_REFCLK
 * and generates 50MHz from pll0's CLKOS2. pll1's 630MHz VCO cannot
 * make 50 either (630/50 = 12.6). So retuning pll0 silently breaks
 * Ethernet on Sergei/ML2, and a third PLL is the cheap way out.
 *
 * That is affordable because both ML2 modules are LFE5U-45F, which
 * has FOUR PLLs (prjtrellis tilegrid lists PLL sites at LL, LR, UL and
 * UR; the 25F has only LL and LR). pll0 and pll1 are untouched here,
 * so video, USB and Ethernet cannot regress.
 *
 * A 2-PLL DDR3 board would instead reuse pll0's ALREADY GENERATED but
 * currently unused 100MHz CLKOP as the edge clock, run the controller
 * at 50MHz and cross to the 48MHz bus. See docs/ddr3.md for why that
 * is a real fallback (measured at roughly 11% throughput) and not
 * something this file needs to anticipate.
 *
 * -- the two outputs --
 *
 * clkout0 (96MHz) becomes the ECLK, and sys_clk is CLKDIVF'd down
 * from it in rtl/sysctl.v so that the bus and the DDR3 edge clock are
 * phase-aligned and no clock-domain crossing is needed at all.
 *
 * clkout2 (48MHz) is the "init" clock and exists because of a
 * non-obvious hazard: the PHY bring-up sequence in
 * rtl/mem/ddr3_phy_init.v STOPS and RESETS the ECLK domain while the
 * DDRDLL locks. Anything clocked from the CLKDIVF output stops with
 * it -- including sys_clk. The init sequencer therefore has to run on
 * a clock that comes STRAIGHT off the PLL and keeps ticking. Same
 * frequency as sys_clk, deliberately not the same net.
 */

`ifdef OSC48
module pll2
(
    input clkin,     // 48 MHz reference (CLK_48)
    output clkout0,  // 96 MHz -- DDR3 edge clock source (sys2x_i)
    output clkout2,  // 48 MHz -- free-running PHY init clock
    output locked
);
(* FREQUENCY_PIN_CLKI="48" *)
(* FREQUENCY_PIN_CLKOP="96" *)
(* FREQUENCY_PIN_CLKOS2="48" *)
(* ICP_CURRENT="12" *) (* LPF_RESISTOR="8" *) (* MFG_ENABLE_FILTEROPAMP="1" *) (* MFG_GMCREF_SEL="2" *)
EHXPLLL #(
        .PLLRST_ENA("DISABLED"),
        .INTFB_WAKE("DISABLED"),
        .STDBY_ENABLE("DISABLED"),
        .DPHASE_SOURCE("DISABLED"),
        .OUTDIVIDER_MUXA("DIVA"),
        .OUTDIVIDER_MUXB("DIVB"),
        .OUTDIVIDER_MUXC("DIVC"),
        .OUTDIVIDER_MUXD("DIVD"),
        // 48MHz / 1 = 48MHz PFD; x2 = 96MHz CLKOP; x6 = 576MHz VCO,
        // comfortably inside the ECP5's 400-800MHz VCO range.
        .CLKI_DIV(1),
        .CLKOP_ENABLE("ENABLED"),
        .CLKOP_DIV(6),
        .CLKOP_CPHASE(2),
        .CLKOP_FPHASE(0),
        .CLKOS_ENABLE("DISABLED"),
        .CLKOS_DIV(6),
        .CLKOS_CPHASE(2),
        .CLKOS_FPHASE(0),
        .CLKOS2_ENABLE("ENABLED"),
        .CLKOS2_DIV(12),
        .CLKOS2_CPHASE(2),
        .CLKOS2_FPHASE(0),
        .CLKOS3_ENABLE("DISABLED"),
        .CLKOS3_DIV(12),
        .CLKOS3_CPHASE(2),
        .CLKOS3_FPHASE(0),
        .FEEDBK_PATH("CLKOP"),
        .CLKFB_DIV(2)
    ) pll_i (
        .RST(1'b0),
        .STDBY(1'b0),
        .CLKI(clkin),
        .CLKOP(clkout0),
        .CLKOS(),
        .CLKOS2(clkout2),
        .CLKOS3(),
        .CLKFB(clkout0),
        .CLKINTFB(),
        .PHASESEL0(1'b0),
        .PHASESEL1(1'b0),
        .PHASEDIR(1'b1),
        .PHASESTEP(1'b1),
        .PHASELOADREG(1'b1),
        .PLLWAKESYNC(1'b0),
        .ENCLKOP(1'b0),
        .LOCK(locked)
	);
endmodule
`endif
