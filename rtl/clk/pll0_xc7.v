/*
 * pll0_xc7 -- Xilinx 7-series (Artix-7) equivalent of rtl/clk/pll0.v.
 *
 * Same interface, same output frequencies, different primitive. The
 * ECP5 version wraps EHXPLLL; this wraps MMCME2_ADV. rtl/sysctl.v
 * picks between them on `XC7 vs `ECP5, so nothing else in the tree
 * knows which one it got.
 *
 *   clkin    48 MHz   the board's 48 MHz (Sechzig MX1: F5, an MRCC pin,
 *                     driven by the carrier's RP2040)
 *   clkout0  100 MHz
 *   clkout1  48 MHz   sys_clk, 0 deg   } with `SDRAM_CLK90 (rtl/sysctl.v);
 *   clkout4  48 MHz   90 deg           } otherwise unused and removed
 *   clkout2  50 MHz   ethernet RMII reference
 *   clkout3  12 MHz   USB full-speed (usb_hid_host, usb_cdc)
 *
 * -- The VCO --
 *
 * 48 MHz * 25 = 1200 MHz, and every output divides it EXACTLY:
 *
 *   1200 / 12  = 100.0
 *   1200 / 25  =  48.0  (clkout1, clkout4)
 *   1200 / 24  =  50.0
 *   1200 / 100 =  12.0
 *
 * That matters most for the 12 MHz. USB full-speed tolerates 0.25%,
 * and both USB blocks in this tree derive their bit timing by counting
 * this clock -- so a 12.00 MHz that is exactly 12.00 leaves the whole
 * error budget for the HOST's crystal instead of spending half of it
 * here.
 *
 * 1200 MHz is inside the MMCM VCO range for every 7-series speed grade
 * from -1 up (DS181: 600-1200 on -1, 600-1440 on -2). The MX1 boards
 * build for -1 (most modules carry a -2), where it is exactly at the
 * limit -- raise DIVCLK_DIVIDE rather than the multiplier if more
 * outputs are ever needed.
 *
 * -- Feedback --
 *
 * CLKFBOUT is wired straight back to CLKFBIN rather than through a
 * BUFG. Routing feedback through a global buffer is what you do when
 * the MMCM's outputs must be PHASE ALIGNED to the input clock, because
 * the BUFG delay is then inside the loop and gets compensated. Nothing
 * here cares about the phase relationship to the 48 MHz crystal --
 * these are all internal clocks feeding internal logic -- and the
 * direct path is both simpler and slightly lower jitter.
 *
 * The SDRAM is the one place where a phase relationship WILL matter
 * (rtl/mem/sdram_kianv.v's header explains why), and that is a
 * relationship between two MMCM OUTPUTS, which CLKOUT_PHASE handles
 * without touching the feedback path.
 *
 * -- BUFGs --
 *
 * Instantiated here, explicitly, one per output. yosys does not insert
 * global buffers on 7-series, and nextpnr-xilinx will not promote a
 * clock to the global network by itself: without these the MMCM drives
 * local routing, which either fails to place or produces a design with
 * enormous clock skew. This is the single easiest thing to leave out
 * and the hardest to diagnose afterwards.
 */

`ifdef OSC48
module pll0_xc7
(
    input clkin,        // 48 MHz
    input rst,          // MMCM reset, see rtl/sysctl.v
    output clkout0,     // 100 MHz
    output clkout1,     // 48 MHz, 0 deg   (sys_clk with SDRAM_CLK90)
    output clkout2,     // 50 MHz
    output clkout3,     // 12 MHz
    output clkout4,     // 48 MHz, 90 deg  (the SDRAM clock with SDRAM_CLK90)
    output locked
);

wire clkfb;
wire clkout0_raw;
wire clkout1_raw;
wire clkout2_raw;
wire clkout3_raw;
wire clkout4_raw;

MMCME2_ADV #(
    .BANDWIDTH("OPTIMIZED"),
    .COMPENSATION("ZHOLD"),
    .STARTUP_WAIT("FALSE"),
    .DIVCLK_DIVIDE(1),
    .CLKFBOUT_MULT_F(25.000),       // 48 * 25 = 1200 MHz VCO
    .CLKFBOUT_PHASE(0.000),
    .CLKIN1_PERIOD(20.833),         // 48 MHz
    .CLKIN2_PERIOD(0.000),
    .CLKOUT0_DIVIDE_F(12.000),      // 100 MHz
    .CLKOUT0_PHASE(0.000),
    .CLKOUT0_DUTY_CYCLE(0.500),
    .CLKOUT1_DIVIDE(25),            // 48 MHz
    .CLKOUT1_PHASE(0.000),
    .CLKOUT1_DUTY_CYCLE(0.500),
    .CLKOUT2_DIVIDE(24),            // 50 MHz
    .CLKOUT2_PHASE(0.000),
    .CLKOUT2_DUTY_CYCLE(0.500),
    .CLKOUT3_DIVIDE(100),           // 12 MHz
    .CLKOUT3_PHASE(0.000),
    .CLKOUT3_DUTY_CYCLE(0.500),
    // The SDRAM clock, 90 degrees behind CLKOUT1 -- as LiteX's sys_ps.
    // Both come from this one MMCM, so the 90 degrees is exact.
    .CLKOUT4_DIVIDE(25),            // 48 MHz
    .CLKOUT4_PHASE(90.000),
    .CLKOUT4_DUTY_CYCLE(0.500),
    .REF_JITTER1(0.010)
) mmcm_i (
    .CLKIN1(clkin),
    .CLKIN2(1'b0),
    .CLKINSEL(1'b1),
    .CLKFBIN(clkfb),
    .CLKFBOUT(clkfb),
    .CLKOUT0(clkout0_raw),
    .CLKOUT1(clkout1_raw),
    .CLKOUT2(clkout2_raw),
    .CLKOUT3(clkout3_raw),
    .CLKOUT4(clkout4_raw),
    .LOCKED(locked),
    .PWRDWN(1'b0),
    .RST(rst),
    .DADDR(7'b0),
    .DCLK(1'b0),
    .DEN(1'b0),
    .DI(16'b0),
    .DWE(1'b0),
    .PSCLK(1'b0),
    .PSEN(1'b0),
    .PSINCDEC(1'b0)
);

BUFG bufg0 (.I(clkout0_raw), .O(clkout0));
BUFG bufg1 (.I(clkout1_raw), .O(clkout1));
BUFG bufg2 (.I(clkout2_raw), .O(clkout2));
BUFG bufg3 (.I(clkout3_raw), .O(clkout3));
BUFG bufg4 (.I(clkout4_raw), .O(clkout4));

endmodule
`endif
