/*
 * pll1_xc7 -- Xilinx 7-series (Artix-7) equivalent of rtl/clk/pll1.v.
 *
 *   clkin    48 MHz   board crystal
 *   clkout0  126 MHz  TMDS bit clock (bclk)
 *   clkout1  25.2 MHz pixel clock (pclk), 640x480@60
 *
 * -- Why one VCO for both --
 *
 * The same reason pll1.v gives, and it is worth repeating because it
 * is the whole point of this file existing separately from pll0_xc7:
 * TMDS needs a serial bit rate of 10x the pixel clock, and the DDR
 * output stage supplies a factor of two, so bclk must be EXACTLY
 * 5 * pclk. Not approximately. A ratio that is off by a fraction of a
 * percent means the shift register and the pixel counter drift apart
 * and the picture tears at whatever the beat frequency happens to be.
 *
 * 48 * 21 = 1008 MHz, and:
 *
 *   1008 / 8  = 126.0
 *   1008 / 40 =  25.2      126.0 / 25.2 = 5.000000
 *
 * Both from one VCO, so the ratio is a property of two integer
 * dividers off a common source and cannot drift at all.
 *
 * 25.2 MHz is 0.099% above the VESA 25.175 MHz for 640x480@60 -- see
 * pll0.v's comment for why 60 Hz and why that deviation is fine.
 *
 * 1008 MHz is inside the MMCM VCO range on every 7-series speed grade
 * (DS181: 600-1200 on -1 and up).
 *
 * -- BUFGs --
 *
 * Both outputs are buffered here for the reason given in pll0_xc7.v:
 * nothing downstream inserts them, and a clock on local routing is a
 * failure that looks like a timing problem rather than a wiring one.
 *
 * bclk is 126 MHz and is the fastest clock in the design by a wide
 * margin, so it is the one to look at first in the nextpnr report
 * after any change to rtl/gpu/gpu_video.v.
 */

`ifdef OSC48
module pll1_xc7
(
    input clkin,        // 48 MHz
    input rst,          // MMCM reset, see rtl/sysctl.v
    output clkout0,     // 126 MHz -- bclk
    output clkout1,     // 25.2 MHz -- pclk
    output locked
);

wire clkfb;
wire clkout0_raw;
wire clkout1_raw;

MMCME2_ADV #(
    .BANDWIDTH("OPTIMIZED"),
    .COMPENSATION("ZHOLD"),
    .STARTUP_WAIT("FALSE"),
    .DIVCLK_DIVIDE(1),
    .CLKFBOUT_MULT_F(21.000),       // 48 * 21 = 1008 MHz VCO
    .CLKFBOUT_PHASE(0.000),
    .CLKIN1_PERIOD(20.833),         // 48 MHz
    .CLKIN2_PERIOD(0.000),
    .CLKOUT0_DIVIDE_F(8.000),       // 126 MHz
    .CLKOUT0_PHASE(0.000),
    .CLKOUT0_DUTY_CYCLE(0.500),
    .CLKOUT1_DIVIDE(40),            // 25.2 MHz
    .CLKOUT1_PHASE(0.000),
    .CLKOUT1_DUTY_CYCLE(0.500),
    .REF_JITTER1(0.010)
) mmcm_i (
    .CLKIN1(clkin),
    .CLKIN2(1'b0),
    .CLKINSEL(1'b1),
    .CLKFBIN(clkfb),
    .CLKFBOUT(clkfb),
    .CLKOUT0(clkout0_raw),
    .CLKOUT1(clkout1_raw),
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

endmodule
`endif
