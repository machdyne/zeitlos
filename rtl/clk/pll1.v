// diamond 3.7 accepts this PLL
// diamond 3.8-3.9 is untested
// diamond 3.10 or higher is likely to abort with error about unable to use feedback signal
// cause of this could be from wrong CPHASE/FPHASE parameters
`ifdef OSC48
module pll1
(
    input clkin, // 48 MHz, 0 deg
    output clkout0, // 126 MHz, 0 deg -- TMDS bit clock (bclk)
    output clkout1, // 25.2 MHz, 0 deg -- pixel clock (pclk) for
                     // 640x480@60Hz. Both outputs now come from THIS
                     // one PLL, sharing a single 630MHz VCO, giving an
                     // EXACT 5:1 bclk:pclk ratio (126/25.2 = 5.0 exactly)
                     // -- TMDS needs the serial bit rate at 10x pclk,
                     // and with the ODDRX1F DDR output stage's own 2x
                     // doubling (see rtl/gpu/gpu_video.v), that's
                     // bclk = pclk*5. This replaces the previous setup
                     // (pclk on pll0, bclk here on pll1, only an
                     // approximate 4.988:1 ratio) -- see pll0.v's own
                     // comment on why 60Hz was chosen over 75Hz
                     // (broader compatibility, including with HDMI TVs
                     // built around the CEA-861 timing set rather than
                     // VESA DMT, plus 25.2MHz's tighter 0.099%
                     // deviation from the true VESA 25.175MHz vs the
                     // old setup's 0.25%).
    output locked
);
wire clkfb;
(* FREQUENCY_PIN_CLKI="48" *)
(* FREQUENCY_PIN_CLKOP="126" *)
(* FREQUENCY_PIN_CLKOS="25.2" *)
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
        .CLKI_DIV(8),
        .CLKOP_ENABLE("ENABLED"),
        .CLKOP_DIV(5),
        .CLKOP_CPHASE(2),
        .CLKOP_FPHASE(0),
        .CLKOS_ENABLE("ENABLED"),
        .CLKOS_DIV(25),
        .CLKOS_CPHASE(0),
        .CLKOS_FPHASE(0),
        .FEEDBK_PATH("CLKOP"),
        .CLKFB_DIV(21)
    ) pll_i (
        .RST(1'b0),
        .STDBY(1'b0),
        .CLKI(clkin),
        .CLKOP(clkfb),
        .CLKOS(clkout1),
        .CLKFB(clkfb),
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

// CLKOP (126MHz) drives the internal feedback path (clkfb, above) AND
// clkout0 simultaneously -- a single physical clock net fanning out to
// both is standard PLL usage, nothing unusual about reusing it here.
assign clkout0 = clkfb;

endmodule
`endif
