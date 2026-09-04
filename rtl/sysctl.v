/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * System Controller (top)
 *
 */

`include "boards.vh"
`include "csrs.vh"

localparam SYSCLK = 48_000_000;

// Instruction cache geometry defaults, if a board enabled `ICACHE
// without pinning them (rtl/boards.vh). 8KB with 4-word lines suits
// the SDRAM boards: rtl/mem/sdram.v has no burst path, so a fill costs
// ~11 cycles per word regardless and short lines keep the miss penalty
// down. PSRAM boards will eventually want longer lines -- rtl/mem/
// qqspi.v spends ~40 of its ~63 cycles per word on fixed command/
// address/dummy overhead, which a burst would amortize -- but that
// needs burst support in qqspi_wb first, so don't raise this there yet.
`ifdef ICACHE
`ifndef ICACHE_KB
`define ICACHE_KB 8
`endif
`ifndef ICACHE_LINE_WORDS
`define ICACHE_LINE_WORDS 4
`endif
`endif

// USB CDC-ACM console defaults, if a board enabled `USB_CDC without
// pinning them (rtl/boards.vh). See rtl/usb_cdc_uart.v's parameter
// block for what each one costs.
`ifdef USB_CDC
// Machdyne's USB identity, shared with the DFU bootloader on these
// same boards -- see docs/usb_cdc.md's identity section for why that
// is deliberate and what it costs on Windows.
`ifndef USB_CDC_VID
`define USB_CDC_VID 16'h16d0
`endif
`ifndef USB_CDC_PID
`define USB_CDC_PID 16'h116d
`endif
// Bulk packet size. 8 bytes, which is worth about what the 1 Mbaud
// console it replaces was worth -- and the FIFOs behind it are
// flip-flops, so this is the dominant cost in the block: 1257 LUT4 at
// 8 against 3038 at 64, measured on ECP5.
`ifndef USB_CDC_MPS
`define USB_CDC_MPS 8
`endif
// Ten seconds at 48MHz before a transmit that nothing is draining
// gives up and starts discarding. Without this the BIOS blocks
// forever on a board whose USB-C socket is plugged into a charger,
// which is not a degraded console but a machine that never reaches
// load_zeitlos(). See rtl/usb_cdc_uart.v.
`ifndef USB_CDC_STALL_CYCLES
`define USB_CDC_STALL_CYCLES 32'd480_000_000
`endif
`endif

// Audio geometry defaults, if a board enabled `AUDIO without pinning
// them (rtl/boards.vh).
//
// 1024 frames: 46ms at 22kHz, 23ms at 44.1kHz.
//
// This was 128 and that was WRONG -- see rtl/audio.v's header for the
// scheduling arithmetic that says so. The short version: this is a
// preemptive round-robin system and wm and sh both busy-poll, so a
// player can be off the CPU for two or three 1.365ms ticks at a
// stretch and must refill a whole round's worth during its own slice.
// 128 frames is 5.8ms at 22kHz, which leaves no margin at all.
//
// The width is what makes 1024 the right number rather than 512: a
// DP16KD is 18 bits wide, so a 32-bit FIFO needs two of them whatever
// the depth. 512 frames and 1024 frames cost exactly the same two
// blocks; 1024 uses them fully. Below 1024 you are paying for BRAM
// you are not using.
`ifdef AUDIO
`ifndef AUDIO_FIFO_LOG2
`define AUDIO_FIFO_LOG2 10
`endif
// Power-on sample rate divider. fs = SYSCLK / (64 * RATE), so 17 is
// 44117.6Hz at 48MHz -- 0.04% high, a 0.7-cent pitch error.
//
// A board with the optical S/PDIF transmitter (Sergei, not supported
// yet) will want 16 instead, giving 46875Hz: that is the only sample
// rate on this clocking whose biphase half-cell is an exact integer
// number of sys_clk cycles. See docs/audio.md.
`ifndef AUDIO_RATE_RESET
`define AUDIO_RATE_RESET 8'd17
`endif
// log2 of the hardware mixer's channel count. 3 = eight channels;
// 2 = four, which is all a ProTracker MOD needs and is the dial to
// reach for when placement is tight. See rtl/audio_mixer.v.
`ifndef AUDIO_MIXER_CH_BITS
`define AUDIO_MIXER_CH_BITS 3
`endif
// Power-on CTRL. Muted, no interrupt, no channel swap.
//
// The one bit a board is likely to want here is SWAPLR (bit 2). PT8211
// WS polarity is the single thing in this subsystem not verified
// against a datasheet with confidence, so if a board's channels come
// out reversed the fix belongs HERE -- once, at power-on -- rather
// than in every app that plays a sound. docs/audio.md said so before
// this define existed, which made the advice untrue.
`ifndef AUDIO_CTRL_RESET
`define AUDIO_CTRL_RESET 8'h00
`endif
`endif

// Which DACs this board actually has pins for, reported to software in
// rtl/audio.v's CONFIG register. Software cannot otherwise tell -- the
// register interface is identical whichever output is wired.
//
// Bit 2 is RESERVED for the optical S/PDIF transmitter on Sergei and
// is deliberately not defined anywhere yet; see rtl/audio.v's header
// for what attaching one involves.
localparam [3:0] AUDIO_FORMATS =
`ifdef AUDIO_SD
	4'b0001 |
`endif
`ifdef AUDIO_PT8211
	4'b0010 |
`endif
`ifdef AUDIO_SPDIF
	4'b0100 |
`endif
	4'b0000;

// How many GPIO ports this build has pins for, handed to rtl/gpio.v as
// its NPORTS parameter and reported to software in that block's CONFIG
// register (and in rtl/csrs.v's FEATURES2).
//
// Derived from the `GPIO_PORT0..3 defines rather than being a define of
// its own, so there is exactly one thing to edit when a board gains a
// port and no way for a count and a pin declaration to disagree. It is
// a SUM rather than a highest-index search, which makes a gap
// (`GPIO_PORT0 and `GPIO_PORT2 but not `GPIO_PORT1) come out as 2 --
// wrong, and deliberately so: gpio.v numbers its ports densely from 0,
// so a gap is a board description error and reporting a count that
// does not match the pins is how it gets noticed. Define them in
// order.
localparam GPIO_NPORTS = 0
`ifdef GPIO_PORT0
	+ 1
`endif
`ifdef GPIO_PORT1
	+ 1
`endif
`ifdef GPIO_PORT2
	+ 1
`endif
`ifdef GPIO_PORT3
	+ 1
`endif
	;

module sysctl #()
(

`ifdef OSC100
   input CLK_100,
`endif
`ifdef OSC50
   input CLK_50,
`endif
`ifdef OSC48
   input CLK_48,
`endif
`ifdef OSC25
   input CLK_25,
`endif

`ifdef LED_RGB
	output LED_R, LED_G,
`endif
	output LED_B,

`ifdef UART0
   output UART0_TX,
   input UART0_RX,
`endif

	// USB CDC-ACM console (rtl/usb_cdc_uart.v, docs/usb_cdc.md) --
	// the USB-C socket, which on Obst and Lakritz is wired straight
	// to the FPGA through series resistors and is otherwise doing
	// nothing but supplying power once the DFU bootloader has handed
	// over.
	//
	// "ufp" is upstream-facing port, the USB term for the device end
	// of a link, and the names match the board's own bootloader
	// constraints (tinydfu-bootloader/boards/obst) so that the two
	// bitstreams describe the same three balls the same way.
	//
	// All three are inout. dp/dm turn around every transaction, and
	// the pull-up pin is driven high or RELEASED rather than driven
	// low -- see rtl/usb_cdc_uart.v's line-buffer section for why
	// those are not the same thing.
	//
	// This is mutually exclusive with `UART0; rtl/boards.vh enforces
	// that with an `undef rather than leaving it to each board to
	// remember.
`ifdef USB_CDC
	inout usb_ufp_dp,
	inout usb_ufp_dm,
	inout usb_ufp_pull,
`endif

`ifdef UART1
	output UART1_TX,
	input UART1_RX,
`endif

`ifdef LED_DEBUG
	output [7:0] DBG,
`endif

	// GPIO ports (rtl/gpio.v). Bidirectional, eight pins each, one
	// port per PMOD connector by convention -- bit 0 is PMOD pin 1,
	// and bits 4-7 are pins 7-10, so a port maps onto a connector in
	// the order the pins are numbered on it. See docs/gpio.md.
	//
	// Declared one at a time rather than derived from a count,
	// because the preprocessor cannot compare numbers: a single
	// `GPIO_PORTS 3 could not gate three port declarations and not a
	// fourth. So the defines ARE the count -- GPIO_NPORTS below is
	// derived from them, not the other way round, and there is one
	// place to change to add a port.
	//
	// Four, not eight. The register map in rtl/gpio.v reserves eight
	// (an ECPIX-5 has that many PMOD connectors) but nothing in the
	// current lineup has more than two, and each port declared here
	// is eight more balls the placer has to find. Adding ports 4-7 is
	// this block plus the matching tri-state section further down and
	// nothing else -- gpio.v already handles them.
`ifdef GPIO_PORT0
`ifdef GPIO_PORT0_NARROW
	// Four pins, not eight -- for a board whose connector is a 6-pin
	// PMOD (Sergei). The pins that do not exist are not in the port
	// at all, which is the only thing that works: an unconstrained
	// top-level IO is a hard nextpnr-ecp5 error, and the flag that
	// suppresses it would place them on whatever balls happen to be
	// free -- which on a populated board includes the SDRAM, the PHY
	// and the SD card.
	//
	// Software still sees an eight-bit port. DIR and OUT bits 4-7
	// exist and drive nothing; IN bits 4-7 read 0 (tied off below)
	// where a real floating pin would read 1. That asymmetry is the
	// whole cost of doing this the cheap way, and it is deliberate:
	// a register reporting per-port pin counts would be honest but is
	// machinery on every board for one connector on one board. The
	// release notes for such a target say how many pins it has.
	inout [3:0] GPIO0,
`else
	inout [7:0] GPIO0,
`endif
`endif
`ifdef GPIO_PORT1
	inout [7:0] GPIO1,
`endif
`ifdef GPIO_PORT2
	inout [7:0] GPIO2,
`endif
`ifdef GPIO_PORT3
	inout [7:0] GPIO3,
`endif

`ifdef MEM_SRAM
	inout [31:0] SRAM_D,
	output [18:0] SRAM_A,
	output SRAM1_CE, SRAM0_CE,
	output SRAM1_OE, SRAM0_OE,
	output SRAM1_WE, SRAM0_WE,
	output SRAM1_UB, SRAM0_UB,
	output SRAM1_LB, SRAM0_LB,
`endif

`ifdef MEM_SDRAM
	output [12:0] sdram_a,
	inout [15:0] sdram_dq,
	output sdram_cs_n,
	output sdram_cke,
	output sdram_ras_n,
	output sdram_cas_n,
	output sdram_we_n,
	output [1:0] sdram_dm,
	output [1:0] sdram_ba,
	output sdram_clock,
`endif

`ifdef MEM_QQSPI
`ifndef MEM_QQSPI_SINGLE
	output QQSPI_CS1, QQSPI_CS0,
`endif
	output QQSPI_SS,
	output QQSPI_SCK,
	inout QQSPI_MOSI, QQSPI_MISO, QQSPI_SIO2, QQSPI_SIO3,
`endif

`ifdef SPI_SDCARD
   output SD_SS,
   inout SD_MISO,
   output SD_MOSI,
   output SD_SCK,
`endif 

`ifdef SPI_ETH
   output ETH_SS,
   inout ETH_MISO,
   output ETH_MOSI,
   output ETH_SCLK,
   input ETH_INT,
`endif

`ifdef ETH_RMII
`ifdef ETH_RMII_DRIVE_REFCLK
   output ETH_REFCLK,
`else
   input ETH_REFCLK,
`endif
   input [1:0] ETH_RXD,
   output [1:0] ETH_TXD,
   output ETH_TX_EN,
   input ETH_CRS_DV,
   output ETH_RST_N,
`endif

`ifdef AUDIO
`ifdef AUDIO_SD
	output AUDIO_L,
	output AUDIO_R,
`endif
`ifdef AUDIO_PT8211
	output AUD_BCK,
	output AUD_WS,
	output AUD_DIN,
`endif
`ifdef AUDIO_SPDIF
	output AUD_OPTICAL,
`endif
`endif

`ifdef GPU_COMPOSITE
	// Monochrome composite video out. Four bits into an R-2R ladder,
	// then one 75R series resistor to the centre pin of an RCA
	// socket -- see docs/composite.md for values and for why four
	// bits when the picture only needs three levels.
	//
	// A board defining `GPU_COMPOSITE must add COMP_DAC[3:0] to its
	// own .lpf/.ccf. No board in this tree does yet; composite needs
	// four pins and a handful of resistors none of them have.
	output [3:0] COMP_DAC,
`endif

// The VGA and DDMI pin declarations below are suppressed entirely on a
// composite build, not merely left unconnected. An output port that
// nothing drives is not harmless: it synthesises to a pin held at a
// constant, and on a board with a real VGA connector that means a
// monitor sees a dead signal rather than no signal -- which looks like
// a broken output rather than one that was never built. Removing the
// port makes the board file fail loudly at place-and-route instead,
// pointing at the constraint that no longer has anything to bind to.
`ifndef GPU_COMPOSITE
`ifdef GPU_VGA
	output VGA_R,
	output VGA_G,
	output VGA_B,
	output VGA_HS,
	output VGA_VS,
`endif

`ifdef GPU_DDMI
	output DDMI_D0_P,
	output DDMI_D1_P,
	output DDMI_D2_P,
	output DDMI_CK_P,
`endif
`endif

`ifdef USB_HID
	inout [1:0] usb_host_dp,
	inout [1:0] usb_host_dm,
`endif

`ifdef MEM_ROM
   output CSPI_SS_FLASH,
   input CSPI_MISO,
   output CSPI_MOSI,
`ifndef FPGA_ECP5
   output CSPI_SCK,
`endif
`endif

`ifdef BOARD_ULX3S
	output wifi_en,
	output wifi_gpio0,
	output flash_holdn,
	output flash_wpn,
	output LED1,
	output LED2,
	// US2 host pull control: 0 = 15 kΩ pulldown on D+/D- (usb_hid_host)
	output usb_fpga_pu_dp,
	output usb_fpga_pu_dn,
	// SPI 1-bit SD: DAT1/DAT2 must sit high. They were only in the
	// LPF (no net), so ECP5 default pulldown held them low and many
	// cards never left FR_NOT_READY.
	output SD_DAT1,
	output SD_DAT2,
`endif

);

	// BOARD LEDS

`ifdef LED_RGB
	assign LED_R = !cpu_trap;
	assign LED_G = ~(|cpu_irq);
`endif

	// CLOCKS

	wire clk126mhz;
	wire clk100mhz;
	wire clk25_2mhz;
	wire clk50mhz;
	wire clk48mhz;
	wire clk25mhz;
	wire clk12mhz;

`ifdef OSC48
	assign clk48mhz = CLK_48;
`elsif OSC25
	assign clk25mhz = CLK_25;
`endif

	wire sys_clk = clk48mhz;

`ifdef ECP5

	wire pll_locked = pll0_locked && pll1_locked;
	wire pll0_locked;
	wire pll1_locked;

`ifdef OSC48

	pll0 #() pll0_i (
		.clkin(clk48mhz),
		.clkout0(clk100mhz),
		.clkout2(clk50mhz),
		.clkout3(clk12mhz),
		.locked(pll0_locked)
	);

	pll1 #() pll1_i (
		.clkin(clk48mhz),
		.clkout0(clk126mhz),
		.clkout1(clk25_2mhz),
		.locked(pll1_locked)
	);

`elsif OSC25

	pll0_25 #() pll0_i (
		.clkin(clk25mhz),
		.clkout2(clk48mhz),
		.clkout3(clk12mhz),
		.locked(pll0_locked)
	);

	pll1_25 #() pll1_i (
		.clkin(clk25mhz),
		.clkout2(clk126mhz),
		.clkout3(clk25_2mhz),
		.locked(pll1_locked)
	);

`endif

`elsif GATEMATE

	wire pll_locked = pll0_locked && pll1_locked && pll2_locked;
	wire pll0_locked;
	wire pll1_locked;
	wire pll2_locked;

   CC_PLL #(
      .REF_CLK(48.0),      // reference input in MHz
      .OUT_CLK(126.0),     // pll output frequency in MHz
      .PERF_MD("ECONOMY"), // LOWPOWER, ECONOMY, ECONOMY
      .LOW_JITTER(1),      // 0: disable, 1: enable low jitter mode
      .CI_FILTER_CONST(2), // optional CI filter constant
      .CP_FILTER_CONST(4)  // optional CP filter constant
   ) pll_inst0 (
      .CLK_REF(CLK_48), .CLK_FEEDBACK(1'b0), .USR_CLK_REF(1'b0),
      .USR_LOCKED_STDY_RST(1'b0),
		.USR_PLL_LOCKED(pll0_locked),
      .CLK0(clk126mhz),
   );

   CC_PLL #(
      .REF_CLK(48.0),      // reference input in MHz
      .OUT_CLK(25.2),      // pll output frequency in MHz
      .PERF_MD("ECONOMY"), // LOWPOWER, ECONOMY, ECONOMY
      .LOW_JITTER(1),      // 0: disable, 1: enable low jitter mode
      .CI_FILTER_CONST(2), // optional CI filter constant
      .CP_FILTER_CONST(4)  // optional CP filter constant
   ) pll_inst1 (
      .CLK_REF(CLK_48), .CLK_FEEDBACK(1'b0), .USR_CLK_REF(1'b0),
      .USR_LOCKED_STDY_RST(1'b0),
		.USR_PLL_LOCKED(pll1_locked),
      .CLK0(clk25_2mhz),
   );

   CC_PLL #(
      .REF_CLK(48.0),      // reference input in MHz
      .OUT_CLK(12.0),      // pll output frequency in MHz
      .PERF_MD("ECONOMY"), // LOWPOWER, ECONOMY, ECONOMY
      .LOW_JITTER(1),      // 0: disable, 1: enable low jitter mode
      .CI_FILTER_CONST(2), // optional CI filter constant
      .CP_FILTER_CONST(4)  // optional CP filter constant
   ) pll_inst2 (
      .CLK_REF(CLK_48), .CLK_FEEDBACK(1'b0), .USR_CLK_REF(1'b0),
      .USR_LOCKED_STDY_RST(1'b0),
		.USR_PLL_LOCKED(pll2_locked),
      .CLK0(clk12mhz),
   );

`endif

	// RESET
	reg [11:0] resetn_counter = 0;
	wire sys_rstn = &resetn_counter;

	always @(posedge sys_clk) begin
		if (!pll_locked)
			resetn_counter <= 0;
		else if (!sys_rstn)
			resetn_counter <= resetn_counter + 1;
	end

`ifdef BOARD_ULX3S
	// Hold the ESP32 in reset so it does not fight the FPGA for the
	// SD bus (CLK/CMD/DAT are shared). Drive flash HOLD#/WP# high —
	// those pins float to pulldown on unused ECP5 IOs and would
	// otherwise freeze SPI reads of the kernel. LED1/LED2 are
	// bring-up taps that do not depend on the CPU: PLL lock and
	// released reset. LED_B (LED0) stays the gpio_wb software LED.
`ifndef ESP32_LINK
	assign wifi_en = 1'b0;
	assign wifi_gpio0 = 1'b0;
`endif
	assign flash_holdn = 1'b1;
	assign flash_wpn = 1'b1;
	assign LED1 = pll_locked;
	assign LED2 = sys_rstn;
	assign usb_fpga_pu_dp = 1'b0;
	assign usb_fpga_pu_dn = 1'b0;
	assign SD_DAT1 = 1'b1;
	assign SD_DAT2 = 1'b1;
`endif

	// RTC
	reg [15:0] rtc_ctr;	// ~732hz

	always @(posedge sys_clk) begin
		rtc_ctr <= rtc_ctr + 1;
	end

	// INTERRUPTS

	reg irq_timer;
	// -- ethernet receive interrupt --
	//
	// One wire from whichever MAC this board has, into cpu_irq[8].
	// Declared unconditionally and tied low when there is no ethernet
	// at all, so the interrupt assignment below needs no `ifdef of its
	// own -- the same arrangement gpu_frame_ctr uses.
	//
	// A RISING-EDGE PULSE, not a level -- and the first version of
	// this got it wrong in a way the file already warned about.
	//
	// eth_rx_ready below IS a level: high for as long as a frame sits
	// in the RX buffer. Feeding that straight into cpu_irq[8] looked
	// safer (no window in which the interrupt is acknowledged but a
	// packet is still unread) and instead brought the machine to a
	// crawl -- UART output visibly printing character by character,
	// the dock drawing its icons one at a time.
	//
	// The reason is written twenty lines above, about bits 4 and 7: "a
	// latched level source re-fires the instant the handler returns
	// and the machine stops making forward progress." Bit 8 is latched
	// in LATCHED_IRQ, so a level there re-fires forever.
	//
	// The UART and the audio FIFO escape this by being non-latched
	// AND by having handlers that can clear the source -- draining the
	// FIFO lowers the level. Ethernet has neither property: only
	// `net`, a userspace process, can consume the frame, and it cannot
	// run while the ISR is re-entering. Non-latched would storm just
	// the same.
	//
	// So the interrupt is one pulse per ARRIVAL. If a second frame
	// lands before the first is read there is no new rising edge and
	// no second interrupt -- which is fine, because net drains every
	// pending frame once it runs, and its timeout (docs/networking.md)
	// is the backstop for the case where it somehow does not.
	wire eth_rx_ready;
	reg eth_rx_ready_d;
	always @(posedge wbm_clk) eth_rx_ready_d <= eth_rx_ready;
	wire eth_rx_int = eth_rx_ready && !eth_rx_ready_d;
	// The macro is SPI_ETH (see rtl/boards.vh). This tie-off tested
	// ETH_SPI, which is not defined anywhere, so on an ENC28J60 board
	// it stayed active alongside the real driver further down and
	// eth_rx_ready had TWO continuous assignments -- the constant here
	// and the synchronised INT pin. The wire resolves to x the moment
	// a frame arrives, so the interrupt never worked on those boards
	// and net fell back to its backstop timeout for every packet.
	// RMII boards were unaffected: that guard spells its macro
	// correctly.
`ifndef ETH_RMII
`ifndef SPI_ETH
	assign eth_rx_ready = 1'b0;
`endif
`endif


	always @* begin
		cpu_irq = 0;
		// cpu_irq[0] = irq_cpu_timer;
		// cpu_irq[1] = irq_ebreak;
		// cpu_irq[2] = irq_bus_error;
		cpu_irq[3] = irq_timer;
		cpu_irq[4] = wbs_uart0_int;
		cpu_irq[5] = wbs_usb0_int;
`ifdef USB_HID
		cpu_irq[6] = wbs_usb1_int;
`endif
		// rtl/audio.v's FIFO watermark. LEVEL-SENSITIVE, and therefore
		// non-latched in LATCHED_IRQ below -- bit 7 is cleared there
		// alongside bit 4 (the UART), for exactly the same reason: a
		// latched level source re-fires the instant the handler returns
		// and the machine stops making forward progress.
`ifdef AUDIO
		cpu_irq[7] = wbs_audio_int;
`endif

		// Ethernet receive. See eth_rx_int above for why this is a
		// PULSE rather than a level, and docs/networking.md for what
		// it replaced -- sw/apps/net woke ~732 times a second on a
		// timer to discover nothing had arrived, and on a machine
		// whose scheduler splits the CPU between RUNNABLE processes
		// that came out of the foreground app's share.
		//
		// OUTSIDE the `ifdef AUDIO above, which it was accidentally
		// nested inside. eth_rx_ready is declared unconditionally and
		// tied low when the board has no ethernet, precisely so this
		// line needs no `ifdef of its own -- but sitting inside the
		// audio guard, a board with ethernet and no audio would have
		// silently had no ethernet interrupt at all. Every board with
		// ethernet today also has audio, so this was latent rather
		// than active.
		cpu_irq[8] = eth_rx_int;
	end

	always @(posedge sys_clk) begin
		irq_timer <= 0;
		if (rtc_ctr == 0) irq_timer <= 1;
	end

	// WISHBONE BUS

/*
	// XX
	reg [7:0] leds = 0;
	assign DBG = leds;
	always @(posedge sys_clk) begin
		if (varb_master) leds <= leds + 1;
	end
	// XX
*/

   wire [1:0] varb_master;

	wire wbm_clk = sys_clk;
	wire wbm_rst = !sys_rstn;

	wire [31:0] wbm_adr;
	wire [31:0] wbm_dat_o;
	wire [31:0] wbm_dat_i;
	wire [3:0] wbm_sel;
	wire wbm_we;
	wire wbm_stb;
	wire wbm_ack;
	wire wbm_cyc;

	// The CPU's own side of the main bus, upstream of wb_arbiter_main
	// (rtl/arbiter_main.v). This used to BE wbm_* -- the CPU was the
	// only
	// master here, so the two were the same wires. The blitter's
	// memory-copy mode (rtl/gpu/gpu_blit.v, CTRL_SRCMEM) makes it a
	// second master, so the CPU now drives wbc_* and the arbiter
	// drives wbm_*.
	//
	// Everything downstream -- every cs_* decode, every slave, the
	// wbm_dat_i/wbm_ack muxes -- is unchanged and still sees wbm_*,
	// which is now simply "whichever master currently holds the bus"
	// rather than "the CPU".
	wire [31:0] wbc_adr;
	wire [31:0] wbc_dat_o;
	wire [31:0] wbc_dat_i;
	wire [3:0] wbc_sel;
	wire wbc_we;
	wire wbc_stb;
	wire wbc_ack;
	wire wbc_cyc;

	// The blitter's source-read port. Tied off below when the blitter
	// isn't in this bitstream.
	wire [31:0] wbm_blitsrc_adr;
	wire [31:0] wbm_blitsrc_dat_i;
	wire wbm_blitsrc_we;
	wire [3:0] wbm_blitsrc_sel;
	wire wbm_blitsrc_stb;
	wire wbm_blitsrc_cyc;
	wire wbm_blitsrc_ack;
	wire [1:0] marb_master;		// 0=CPU 1=blitter 2=audio mixer

	// Physical (post-MTU) CPU address. This is wb_mtu's translated
	// output, and is what the instruction cache tags on -- see
	// rtl/cache.v's header for why the cache must sit AFTER the MTU
	// rather than before it (tagging virtual 0x8000_xxxx would make
	// every app's addresses alias every other app's, and would force a
	// full invalidate on every context switch).
	//
	// Without ICACHE this is simply wired straight through to wbm_adr
	// and the bus behaves exactly as it did before the cache existed.
	wire [31:0] wbm_cpu_padr;
	wire [31:0] mtu_base;		// from mtu_i, consumed by zeitlos32_wb -- declared
								// here, ahead of both, so it is never an
								// implicit 1-bit net. Unused with picorv32.
	wire wbm_cpu_instr;

	wire [31:0] wbm_vram_adr;
	wire [31:0] wbm_vram_dat_o;
	wire [31:0] wbm_vram_dat_i;
	wire [3:0] wbm_vram_sel;
	wire wbm_vram_we;
	wire wbm_vram_stb;
	wire wbm_vram_cyc;

	wire [27:0] wbm_cpu_adr_sel = (wbm_cpu_adr & 32'h0fff_ffff);

	wire [27:0] wbm_adr_sel = (wbm_adr & 32'h0fff_ffff);
	wire [25:0] wbm_adr_sel_word = wbm_adr_sel[27:2];

	wire [27:0] wbm_vram_adr_sel = wbm_vram_adr - 32'h2000_0000; // subtract base addr
	wire [25:0] wbm_vram_adr_sel_word = wbm_vram_adr_sel[27:2];

	wire [31:0] wbs_bram_dat_o;
	wire [31:0] wbs_mtu_dat_o;
	wire [31:0] wbs_sram_dat_o;
	wire [31:0] wbs_sdram_dat_o;
	wire [31:0] wbs_qqspi_dat_o;
	wire [31:0] wbs_vram_dat_o;
	wire [31:0] wbs_rom_dat_o;
	wire [31:0] wbs_gpio_dat_o;
	wire [31:0] wbs_uart0_dat_o;
`ifdef UART1
	wire [31:0] wbs_uart1_dat_o;
`endif
`ifdef ESP32_LINK
	wire [31:0] wbs_esp32ctl_dat_o;
	wire [31:0] wbs_esp32rx_dat_o;
`endif
	wire [31:0] wbs_spisdcard_dat_o;
	wire [31:0] wbs_usb0_dat_o;
	wire [31:0] wbs_usb1_dat_o;
	wire [31:0] wbs_gpu_dat_o;
	wire [31:0] wbs_gpu_blit_dat_o;
	wire [31:0] wbs_glyph_dat_o;
	wire [31:0] wbs_spieth_dat_o;
	wire [31:0] wbs_ethmac_dat_o;
	wire [31:0] wbs_csrs_dat_o;
	wire [31:0] wbs_socctl_dat_o;
`ifdef RTC
	wire [31:0] wbs_rtc_dat_o;
`endif
`ifdef TRNG
	wire [31:0] wbs_trng_dat_o;
`endif
`ifdef AUDIO
	wire [31:0] wbs_audio_dat_o;
`endif
`ifdef ICACHE
`endif

	// Equivalent to (wbm_adr < 8192), but written as an equality on the
	// upper bits so yosys builds a LUT tree rather than a 32-bit
	// subtractor: the `<` form came out as a 14-stage CCU2C carry chain
	// on the critical path.
	wire cs_bram = (wbm_adr[31:13] == 19'd0);
	wire cs_mtu = ((wbm_adr & 32'hf000_0000) == 32'h9000_0000);

`ifdef MEM_SRAM
	wire cs_sram = ((wbm_adr & 32'hf000_0000) == 32'h4000_0000);
`elsif MEM_SDRAM
	wire cs_sdram = ((wbm_adr & 32'hf000_0000) == 32'h4000_0000);
`elsif MEM_QQSPI
	wire cs_qqspi = ((wbm_adr & 32'hf000_0000) == 32'h4000_0000);
`endif
`ifdef MEM_VRAM
	wire cs_vram = ((wbm_adr & 32'hf000_0000) == 32'h2000_0000);
`endif
`ifdef MEM_ROM
	wire cs_rom = ((wbm_adr & 32'hf000_0000) == 32'h1000_0000);
`endif

// TODO: move peripherals to 32'hfxxx_0000
`ifdef SPI_SDCARD
	wire cs_spisdcard = ((wbm_adr & 32'hf000_0000) == 32'hb000_0000);
`endif
`ifdef USB_HID
	// Pointer sensitivity (usb_hid_wb's SENS_SHIFT): boards whose
	// mouse counts and screen pixels are of the same order leave this
	// alone and get the historical 1:1 pointer.
`ifdef USB_HID_SENS_SHIFT
	localparam USB_HID_SENS = `USB_HID_SENS_SHIFT;
`else
	localparam USB_HID_SENS = 0;
`endif
	// two independent usb_hid_wb instances (port 0 / port 1, see
	// boards/*.lpf's usb_host_dp[1:0]/usb_host_dm[1:0] -- Obst and
	// Lakritz both break out two USB host ports). Both instances share
	// the same 256MB top-nibble slot (0xc for USB_HID, unchanged from
	// before this was two instances) and are discriminated by address
	// bit 5 -- NOT bit 4: wb_adr_i (usb_hid_wb's own address input,
	// see below) is wbm_adr_sel_word = wbm_adr_sel[27:2], a WORD-
	// shifted address, so byte-address bit 4 lands on wb_adr_i[2] --
	// exactly one of the 3 bits (wb_adr_i[2:0]) usb_hid_wb uses
	// internally to select among its own 4 registers (info/keys/
	// mouse/cursor, see rtl/usb_hid.v). Using bit 4 here collided with
	// that: port 1's addresses decoded to a register-select value of
	// 4 inside the module, which matches none of its four cases, so
	// it acked but never drove wb_dat_o. Bit 5 (word bit 3) sits
	// safely above that 3-bit field. Port 0's own register addresses
	// (bit5=0) are therefore bit-for-bit unchanged from before this
	// was two instances -- sw/bios/bios.c and sw/apps/gpu3d/gpu3d.c
	// both have their own private copies of these #defines and don't
	// need updating. Port 1 lives at 0xc0000020-0xc000002c (info/
	// keys/mouse/cursor) -- see zeitlos.h's reg_usb1_* -- not
	// 0xc0000010-0xc000001c as an earlier version of this file had it.
	//
	// which physical device (keyboard/mouse/gamepad) ends up on which
	// port isn't fixed -- see sw/os/hid.c and sw/apps/wm/wm.c, which
	// each poll both instances' own `typ` field and decide dynamically.
	wire cs_usb0 = ((wbm_adr & 32'hf000_0020) == 32'hc000_0000);
	wire cs_usb1 = ((wbm_adr & 32'hf000_0020) == 32'hc000_0020);
`endif
`ifdef GPU_RASTER
	wire cs_gpu = ((wbm_adr & 32'hf000_0000) == 32'ha000_0000);
`endif
`ifdef GPU_BLIT
	wire cs_gpu_blit = ((wbm_adr & 32'hf000_0000) == 32'hd000_0000);
`endif
`ifdef MEM_GLYPH
	wire cs_glyph = ((wbm_adr & 32'hf000_0000) == 32'h3000_0000);
`endif
`ifdef SPI_ETH
	wire cs_spieth = ((wbm_adr & 32'hf000_0000) == 32'h5000_0000);
`endif
`ifdef ETH_RMII
	wire cs_ethmac = ((wbm_adr & 32'hf000_0000) == 32'h6000_0000);
`endif
	// CSRs (rtl/csrs.v) -- always decoded, no `ifdef guard, unlike
	// every peripheral above/below this line -- see csrs.v's own
	// header comment for why. Nibble 0x7 was the first free slot in
	// this address map at the time this was added (0x0/0x9 bram/mtu,
	// 0x1-0x6 mem/glyph/spieth/ethmac, 0xa-0xf gpu/sdcard/usb/debug/
	// uart -- see the full cs_* list in this file).
	// Nibble 0x7 is split: csrs.v keeps 0x7000_00xx and the instruction
	// cache's control/status registers take 0x7000_01xx. Bit 8 selects.
	// The cache deliberately does NOT live inside csrs.v -- that block
	// is documented as read-only and side-effect-free, and a flush
	// register is neither. Nibble 0x8 was the other candidate and was
	// rejected: that's the virtual window apps execute in, so a stale
	// app pointer dereferenced in kernel context would land on cache
	// control registers, which is a bad failure mode to invent.
	// The 0x7000_01xx sub-window MUST be acknowledged on every board,
	// with or without a cache, for a reason learned the hard way: an
	// address nothing decodes gets NO ACK on this bus (the wbm_ack mux
	// below falls through to 1'b0), and picorv32_wb then waits for that
	// ack forever. It is not a harmless read of undefined data, it is a
	// dead hang -- which is exactly what a flush write did on a build
	// without the cache.
	//
	// sw/bios/bios.c and sw/os/fs/fs.c write the flush register
	// unconditionally, and they have to: the obvious alternative --
	// read INFO first to see whether a cache exists -- would hang on
	// that very read for the same reason.
	//
	// So without ICACHE, csrs_wb simply keeps the whole 0x7 nibble it
	// always had. It acks any cycle in range, ignores writes, and reads
	// back 32'h0 for offsets it doesn't know (see rtl/csrs.v) -- which
	// is precisely the stub behaviour needed here, and zero returned
	// from INFO fails z_icache_present()'s magic check, correctly
	// telling software the cache isn't there. A dedicated stub slave
	// was tried first and cost ~800 LUT4 on ECP5: adding another term
	// to the wbm_dat_i mux is expensive because that mux resolves
	// through a 32'hzzzz_zzzz default, which yosys handles poorly.
	// Nibble 0x7 has four tenants:
	//   0x7000_00xx  csrs.v      read-only "what does this bitstream have"
	//   0x7000_01xx  cache.v     instruction cache control/stats  (`ICACHE)
	//   0x7000_02xx  socctl.v    writable global config
	//   0x7000_03xx  rtc.v       wall-clock seconds/sub-seconds   (`RTC)
	//   0x7000_04xx  trng.v      ring-oscillator entropy source   (`TRNG)
	//
	// The tenant mask is 0x700, not 0x300: four tenants fit in bits
	// [9:8], the fifth needed bit 10 as well. Every address above has
	// bit 10 clear, so widening it moved nothing -- the only visible
	// change is that 0x7000_05xx..07xx now fall through to csrs.v
	// instead of aliasing onto cache/socctl/rtc, which is strictly
	// better and is what a future sixth tenant will want anyway.
	//
	// Two of them are optional, and the rule that makes that safe is:
	// CSRS ABSORBS THE WINDOW OF ANY TENANT THAT ISN'T BUILT. It acks
	// any cycle in range, ignores writes, and reads back 32'h0 for
	// offsets it doesn't know (see rtl/csrs.v), which is exactly the
	// stub behaviour needed -- and a zero read correctly fails both
	// z_icache_present()'s and z_rtc_available()'s magic checks, so
	// software is told the hardware isn't there rather than being
	// handed garbage.
	//
	// That absorption is not tidiness. An address NOTHING decodes gets
	// no ack at all on this bus and picorv32_wb waits for it forever
	// -- a dead hang, not a read of undefined data. sw/bios/bios.c and
	// sw/os/fs/fs.c write the icache flush register unconditionally
	// and have to (reading INFO first to check would hang on that very
	// read), which is what taught us this the hard way.
	//
	// cs_csrs is therefore written once, below, as "the whole nibble
	// minus whichever tenants actually exist in THIS build", rather
	// than as a separate expression per combination. With two optional
	// tenants that would be four cases to keep in agreement, and the
	// one that mattered would be the one nobody tested.
	wire cs_socctl = ((wbm_adr & 32'hf000_0700) == 32'h7000_0200);
	wire wbm_cyc_socctl = cs_socctl && wbm_cyc;
`ifdef ICACHE
	// NOTE: 0x7000_01xx is the instruction cache's register window, but
	// the cache is NOT a slave here -- it answers those addresses
	// itself, from the CPU address, upstream of wb_arbiter_main (see
	// rtl/cache.v). So the CPU's accesses never reach this bus.
	//
	// The window is deliberately left to csrs_wb below rather than
	// decoded to nothing: an address nothing decodes gets no ack and
	// hangs whoever asked. Any stray access from another master, or
	// from the CPU on a build without ICACHE, is acked and discarded.
`endif
`ifdef RTC
	wire cs_rtc = ((wbm_adr & 32'hf000_0700) == 32'h7000_0300);
	wire wbm_cyc_rtc = cs_rtc && wbm_cyc;
`endif
`ifdef TRNG
	wire cs_trng = ((wbm_adr & 32'hf000_0700) == 32'h7000_0400);
	wire wbm_cyc_trng = cs_trng && wbm_cyc;
`endif
	// rtl/audio.v -- the SIXTH tenant of nibble 7, at 0x7000_05xx. No
	// mask change was needed: the tenant mask above was already widened
	// from 0x300 to 0x700 when trng arrived, and this file's own comment
	// said 0x7000_05xx..07xx falling through to csrs.v "is what a future
	// sixth tenant will want anyway". This is that tenant.
`ifdef AUDIO
	wire cs_audio = ((wbm_adr & 32'hf000_0700) == 32'h7000_0500);
	wire wbm_cyc_audio = cs_audio && wbm_cyc;
`endif
	wire cs_csrs = ((wbm_adr & 32'hf000_0000) == 32'h7000_0000)
		&& !cs_socctl
`ifdef ICACHE
`endif
`ifdef RTC
		&& !cs_rtc
`endif
`ifdef TRNG
		&& !cs_trng
`endif
`ifdef AUDIO
		&& !cs_audio
`endif
		;
	// Unconditional, like cs_uart0 below and unlike the `ifdef-guarded
	// decodes above -- rtl/gpio.v is always instantiated. It used to
	// be guarded by `DEBUG, which was universal so the guard never
	// fired; that define is gone (see rtl/gpio.v's header on why a
	// block software PROBES must not be one of the things that can be
	// missing).
	wire cs_gpio = ((wbm_adr & 32'hf000_0000) == 32'he000_0000);
	// Unconditional, unlike the `ifdef-guarded decodes above. Without
	// `UART0 this window is answered by rtl/uart_null.v instead of by
	// uart_top -- see the instantiation below for why it must be
	// answered by SOMETHING.
	//
	// UART1 and the ESP32 registers (ULX3S) share nibble 0xF but sit
	// above 0x100/0x200/0x300, so where they exist this window narrows
	// to bits 8-9 == 0 (the same idea as USB port 0/1 using bit 5).
	// Without them the whole-nibble decode is what every other board
	// has always had.
`ifdef UART1
	wire cs_uart0 = ((wbm_adr & 32'hf000_0300) == 32'hf000_0000);
	wire cs_uart1 = ((wbm_adr & 32'hf000_0300) == 32'hf000_0100);
`else
	wire cs_uart0 = ((wbm_adr & 32'hf000_0000) == 32'hf000_0000);
`endif
`ifdef ESP32_LINK
	wire cs_esp32ctl = ((wbm_adr & 32'hf000_0300) == 32'hf000_0200);
	wire cs_esp32rx = ((wbm_adr & 32'hf000_0300) == 32'hf000_0300);
`endif

	// Both muxes below are AND-OR, not priority chains. Every cs_* is
	// one-hot by construction (distinct top nibbles; usb0/usb1 split on
	// bit 5; the 0x7 region sub-decodes on bits [10:8] with cs_csrs
	// excluding the others), so priority buys nothing and costs a mux
	// level per peripheral -- a chain that grew with every slave added
	// and became the long pole on the 48MHz bus. AND-OR maps to a
	// balanced OR tree instead: ~3 LUT levels regardless of slave count.
	// Nothing selected still yields zero, as before.
	assign wbm_dat_i =
		({32{cs_bram}} & wbs_bram_dat_o) |
		({32{cs_mtu}} & wbs_mtu_dat_o) |
`ifdef MEM_SRAM
		({32{cs_sram}} & wbs_sram_dat_o) |
`elsif MEM_SDRAM
		({32{cs_sdram}} & wbs_sdram_dat_o) |
`elsif MEM_QQSPI
		({32{cs_qqspi}} & wbs_qqspi_dat_o) |
`endif
`ifdef MEM_VRAM
`ifdef GPU_RASTER
		({32{cs_vram}} & wbm_cpu_arb0_dat_i) |
`else
		({32{cs_vram}} & wbs_vram_dat_o) |
`endif
`endif
`ifdef MEM_ROM
		({32{cs_rom}} & wbs_rom_dat_o) |
`endif
		({32{cs_gpio}} & wbs_gpio_dat_o) |
		({32{cs_uart0}} & wbs_uart0_dat_o) |
`ifdef UART1
		({32{cs_uart1}} & wbs_uart1_dat_o) |
`endif
`ifdef ESP32_LINK
		({32{cs_esp32ctl}} & wbs_esp32ctl_dat_o) |
		({32{cs_esp32rx}} & wbs_esp32rx_dat_o) |
`endif
`ifdef SPI_SDCARD
		({32{cs_spisdcard}} & wbs_spisdcard_dat_o) |
`endif
`ifdef USB_HID
		({32{cs_usb0}} & wbs_usb0_dat_o) |
		({32{cs_usb1}} & wbs_usb1_dat_o) |
`endif
`ifdef GPU_RASTER
		({32{cs_gpu}} & wbs_gpu_dat_o) |
`endif
`ifdef GPU_BLIT
		({32{cs_gpu_blit}} & wbs_gpu_blit_dat_o) |
`endif
`ifdef MEM_GLYPH
		({32{cs_glyph}} & wbs_glyph_dat_o) |
`endif
`ifdef SPI_ETH
		({32{cs_spieth}} & wbs_spieth_dat_o) |
`endif
`ifdef ETH_RMII
		({32{cs_ethmac}} & wbs_ethmac_dat_o) |
`endif
		({32{cs_socctl}} & wbs_socctl_dat_o) |
`ifdef RTC
		({32{cs_rtc}} & wbs_rtc_dat_o) |
`endif
`ifdef TRNG
		({32{cs_trng}} & wbs_trng_dat_o) |
`endif
`ifdef AUDIO
		({32{cs_audio}} & wbs_audio_dat_o) |
`endif
		({32{cs_csrs}} & wbs_csrs_dat_o) |
`ifdef ICACHE
`endif
		32'd0;

	wire wbs_bram_ack_o;
	wire wbs_mtu_ack_o;
	wire wbs_sram_ack_o;
	wire wbs_sdram_ack_o;
	wire wbs_qqspi_ack_o;
	wire wbs_vram_ack_o;
	wire wbs_rom_ack_o;
	wire wbs_gpio_ack_o;
	wire wbs_uart0_ack_o;
	// Declared here rather than inside `ifdef UART0 below, because
	// cpu_irq[4] reads it unconditionally. It used to be a `reg` in
	// that block, which left it an implicit undriven net on a build
	// without a UART -- and an implicit net connected to an interrupt
	// input is not a failure anything reports.
	wire wbs_uart0_int;
`ifdef UART1
	wire wbs_uart1_ack_o;
`endif
`ifdef ESP32_LINK
	wire wbs_esp32ctl_ack_o;
	wire wbs_esp32rx_ack_o;
`endif
	wire wbs_spisdcard_ack_o;
	wire wbs_usb0_ack_o;
	wire wbs_usb1_ack_o;
	wire wbs_gpu_ack_o;
	wire wbs_gpu_blit_ack_o;
	wire wbs_glyph_ack_o;
	wire wbs_spieth_ack_o;
	wire wbs_ethmac_ack_o;
	wire wbs_csrs_ack_o;
	wire wbs_socctl_ack_o;
`ifdef RTC
	wire wbs_rtc_ack_o;
`endif
`ifdef TRNG
	wire wbs_trng_ack_o;
`endif
`ifdef AUDIO
	wire wbs_audio_ack_o;
	wire wbs_audio_int;
	// rtl/audio_mixer.v's sample fetches -- third master on the main
	// bus, see the arbiter instantiation below.
	wire [31:0] wbm_audio_adr;
	wire [31:0] wbm_audio_dat_o;
	wire [31:0] wbm_audio_dat_i;
	wire wbm_audio_we;
	wire [3:0] wbm_audio_sel;
	wire wbm_audio_stb;
	wire wbm_audio_cyc;
	wire wbm_audio_ack;
`endif
`ifdef ICACHE
`endif

	assign wbm_ack =
		(cs_bram & wbs_bram_ack_o) |
		(cs_mtu & wbs_mtu_ack_o) |
`ifdef MEM_SRAM
		(cs_sram & wbs_sram_ack_o) |
`elsif MEM_SDRAM
		(cs_sdram & wbs_sdram_ack_o) |
`elsif MEM_QQSPI
		(cs_qqspi & wbs_qqspi_ack_o) |
`endif
`ifdef MEM_VRAM
`ifdef GPU_RASTER
		(cs_vram & wbm_cpu_arb0_ack) |
`else
		(cs_vram & wbs_vram_ack_o) |
`endif
`endif
`ifdef MEM_ROM
		(cs_rom & wbs_rom_ack_o) |
`endif
		(cs_gpio & wbs_gpio_ack_o) |
		(cs_uart0 & wbs_uart0_ack_o) |
`ifdef UART1
		(cs_uart1 & wbs_uart1_ack_o) |
`endif
`ifdef ESP32_LINK
		(cs_esp32ctl & wbs_esp32ctl_ack_o) |
		(cs_esp32rx & wbs_esp32rx_ack_o) |
`endif
`ifdef SPI_SDCARD
		(cs_spisdcard & wbs_spisdcard_ack_o) |
`endif
`ifdef USB_HID
		(cs_usb0 & wbs_usb0_ack_o) |
		(cs_usb1 & wbs_usb1_ack_o) |
`endif
`ifdef GPU_RASTER
		(cs_gpu & wbs_gpu_ack_o) |
`endif
`ifdef GPU_BLIT
		(cs_gpu_blit & wbs_gpu_blit_ack_o) |
`endif
`ifdef MEM_GLYPH
		(cs_glyph & wbs_glyph_ack_o) |
`endif
`ifdef SPI_ETH
		(cs_spieth & wbs_spieth_ack_o) |
`endif
`ifdef ETH_RMII
		(cs_ethmac & wbs_ethmac_ack_o) |
`endif
		(cs_socctl & wbs_socctl_ack_o) |
`ifdef RTC
		(cs_rtc & wbs_rtc_ack_o) |
`endif
`ifdef TRNG
		(cs_trng & wbs_trng_ack_o) |
`endif
`ifdef AUDIO
		(cs_audio & wbs_audio_ack_o) |
`endif
		(cs_csrs & wbs_csrs_ack_o) |
`ifdef ICACHE
`endif
		1'b0;

	// WISHBONE MASTER: CPU

   wire cpu_trap;
   reg [31:0] cpu_irq = 0;

	wire [31:0] wbm_cpu_adr;
	wire [31:0] wbm_cpu_dat_o;
	wire [31:0] wbm_cpu_dat_i;
	wire [3:0] wbm_cpu_sel;
	wire wbm_cpu_we;
	wire wbm_cpu_stb;
	wire wbm_cpu_ack;
	wire wbm_cpu_cyc;

	localparam BRAM_WORDS = 2048;

`ifdef CPU_ZEITLOS32

	// EXPERIMENTAL: rtl/cpu/zeitlos32, selected by `CPU_ZEITLOS32 in
	// rtl/boards.vh. Port-for-port compatible with the picorv32_wb
	// instantiation in the `else branch below -- same wishbone
	// signals, same irq input, same mem_instr for rtl/cache.v, and the
	// same custom interrupt ABI, so sw/bios/boot_picorv32.S and
	// sw/bios/custom_ops.S are untouched. Nothing else in this file
	// knows which core is present.
	//
	// See docs/zeitlos32.md and rtl/cpu/zeitlos32/tests/.

	zeitlos32_wb #(
		.STACKADDR(BRAM_WORDS * 4),      // end of BRAM
		.PROGADDR_RESET(32'h0000_0000),
		.PROGADDR_IRQ(32'h0000_0010),
		// rv32im -- the same `CPU_MUL/`CPU_MUL_FAST/`CPU_DIV switches
		// the picorv32 branch below uses. FAST_MUL selects the DSP
		// multiplier over the sequential one; unlike picorv32 the two
		// are not mutually exclusive parameters, so ENABLE_MUL stays
		// set either way and FAST_MUL just picks the implementation.
`ifdef CPU_MUL_FAST
		.ENABLE_MUL(1),
		.FAST_MUL(1),
`elsif CPU_MUL
		.ENABLE_MUL(1),
		.FAST_MUL(0),
`else
		.ENABLE_MUL(0),
		.FAST_MUL(0),
`endif
`ifdef CPU_DIV
		.ENABLE_DIV(1),
`else
		.ENABLE_DIV(0),
`endif
		.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1111_0110_1111)
	)
	wbm_cpu0_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wbm_adr_o(wbm_cpu_adr),
		.mtu_base(mtu_base),
		.wbm_dat_o(wbm_cpu_dat_o),
		.wbm_dat_i(wbm_cpu_dat_i),
		.wbm_we_o(wbm_cpu_we),
		.wbm_sel_o(wbm_cpu_sel),
		.wbm_stb_o(wbm_cpu_stb),
		.wbm_ack_i(wbm_cpu_ack),
		.wbm_cyc_o(wbm_cpu_cyc),
		.trap(cpu_trap),
		.irq(cpu_irq),
		.eoi(),
		.mem_instr(wbm_cpu_instr)
	);

`else

	picorv32_wb #(
      .STACKADDR(BRAM_WORDS * 4),      // end of BRAM
      .PROGADDR_RESET(32'h0000_0000),
      .PROGADDR_IRQ(32'h0000_0010),
      .BARREL_SHIFTER(1),
      .COMPRESSED_ISA(0),
      // rv32im -- see rtl/boards.vh's CPU_MUL/CPU_MUL_FAST/CPU_DIV.
      // ENABLE_MUL and ENABLE_FAST_MUL are mutually exclusive inside
      // picorv32 (the generate block prefers FAST), so CPU_MUL_FAST
      // selects the DSP multiplier and plain CPU_MUL the sequential
      // one; defining both is fine and means "fast".
`ifdef CPU_MUL_FAST
      .ENABLE_MUL(0),
      .ENABLE_FAST_MUL(1),
`elsif CPU_MUL
      .ENABLE_MUL(1),
      .ENABLE_FAST_MUL(0),
`else
      .ENABLE_MUL(0),
      .ENABLE_FAST_MUL(0),
`endif
`ifdef CPU_DIV
      .ENABLE_DIV(1),
`else
      .ENABLE_DIV(0),
`endif
      .ENABLE_IRQ(1),
      .ENABLE_IRQ_TIMER(0),
      .ENABLE_IRQ_QREGS(1),
		.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1111_0110_1111)
	)
	wbm_cpu0_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wbm_adr_o(wbm_cpu_adr),
		.wbm_dat_o(wbm_cpu_dat_o),
		.wbm_dat_i(wbm_cpu_dat_i),
		.wbm_we_o(wbm_cpu_we),
		.wbm_sel_o(wbm_cpu_sel),
		.wbm_stb_o(wbm_cpu_stb),
		.wbm_ack_i(wbm_cpu_ack),
		.wbm_cyc_o(wbm_cpu_cyc),
		.trap(cpu_trap),
		.irq(cpu_irq),
		// picorv32 already produces this; it was simply never wired up
		// before. rtl/cache.v uses it to cache instruction fetches ONLY,
		// which is what keeps data coherency out of the picture entirely.
		.mem_instr(wbm_cpu_instr)
	);

`endif

	// WISHBONE SLAVE: MTU (Memory Translation Unit)
	wire wbm_cyc_mtu = cs_mtu && wbm_cyc;

	// Where the translation happens depends on the CPU -- see the
	// ON_BUS parameter in rtl/mtu.v. zeitlos32_wb applies it itself,
	// from mtu_base, on its own address register; picorv32_wb is the
	// vendored wrapper and is left alone, so for it the MTU translates
	// on the bus as it always has.
`ifdef CPU_ZEITLOS32
	localparam MTU_ON_BUS = 0;
`else
	localparam MTU_ON_BUS = 1;
`endif

	wb_mtu #(
		.ON_BUS(MTU_ON_BUS)
	) mtu_i (
		.clk_i(wbm_clk),
		.rst_i(wbm_rst),
		.addr_in(wbm_cpu_adr),
		.addr_out(wbm_cpu_padr),
		.base_o(mtu_base),
		.cfg_adr_i(wbm_cpu_adr_sel),
		.cfg_dat_i(wbm_dat_o),
		.cfg_dat_o(wbs_mtu_dat_o),
		.cfg_sel_i(wbm_sel),
		.cfg_we_i(wbm_we),
		.cfg_stb_i(wbm_stb),
		.cfg_cyc_i(wbm_cyc_mtu),
		.cfg_ack_o(wbs_mtu_ack_o)
    );

	// CPU controls the main bus (will share with DMA controller)
	//
	// With ICACHE the instruction cache sits in this path: the CPU
	// talks to the cache, and the cache drives the bus. It needs to own
	// wbm_adr rather than just observe it, because during a line fill
	// it addresses words the CPU never asked for. Every wbm_* signal
	// still has exactly one driver either way.
`ifdef ICACHE

// Boards define ICACHE_FAST_HIT to choose between a combinational
// 1-cycle hit acknowledge (1) and a registered 2-cycle one (0); see
// rtl/cache.v's FAST_HIT parameter. Default to the fast path when a
// board says nothing, so older board blocks keep building.
`ifndef ICACHE_FAST_HIT
`define ICACHE_FAST_HIT 1
`endif

	wire cache_cfg_hit;

	wb_icache #(
		.CACHE_KB(`ICACHE_KB),
		.LINE_WORDS(`ICACHE_LINE_WORDS),
		.FAST_HIT(`ICACHE_FAST_HIT),
		.CFG_BASE(32'h7000_0100)
	) icache_i (
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),

		// upstream: the CPU, at its translated physical address
		.c_adr_i(wbm_cpu_padr),
		.c_dat_i(wbm_cpu_dat_o),
		.c_dat_o(wbm_cpu_dat_i),
		.c_we_i(wbm_cpu_we),
		.c_sel_i(wbm_cpu_sel),
		.c_stb_i(wbm_cpu_stb),
		.c_cyc_i(wbm_cpu_cyc),
		.c_instr_i(wbm_cpu_instr),
		.c_ack_o(wbm_cpu_ack),

		// downstream: the main wishbone bus, via wb_arbiter_main below
		.m_adr_o(wbc_adr),
		.m_dat_o(wbc_dat_o),
		.m_dat_i(wbc_dat_i),
		.m_we_o(wbc_we),
		.m_sel_o(wbc_sel),
		.m_stb_o(wbc_stb),
		.m_cyc_o(wbc_cyc),
		.m_ack_i(wbc_ack),

		// Control/status registers are answered INSIDE the cache,
		// from the CPU address, upstream of wb_arbiter_main -- they
		// are not a slave on the main bus. See rtl/cache.v: as a bus
		// master, routing its own registers through the arbiter meant
		// waiting on a transaction only it could answer.
		.c_cfg_hit(cache_cfg_hit)
	);

`else

	// wbm_cpu_padr, not wbm_cpu_adr: the MTU's translated output, which
	// is what this bus has always carried -- the MTU simply used to
	// drive wbm_adr directly instead of going through a named wire.
	assign wbc_adr = wbm_cpu_padr;
	assign wbc_dat_o = wbm_cpu_dat_o;
	assign wbm_cpu_dat_i = wbc_dat_i;
	assign wbc_sel = wbm_cpu_sel;
	assign wbc_we = wbm_cpu_we;
	assign wbc_stb = wbm_cpu_stb;
	assign wbm_cpu_ack = wbc_ack;
	assign wbc_cyc = wbm_cpu_cyc;

`endif

	// MAIN BUS ARBITER (CPU vs blitter source reads)
	//
	// See rtl/arbiter_main.v for the round-robin policy and why the
	// grant
	// is held for a whole transaction. That last part is what makes
	// this safe to drop in front of the CPU: a CPU transaction is
	// never preempted half way, so every cs_* decode stays coherent
	// for the whole of it, and nothing downstream can observe the bus
	// changing owner mid-cycle.
	//
	// Deadlock: the blitter needs BOTH this bus (source reads) and the
	// VRAM bus behind wb_arbiter_vram (framebuffer writes), and each
	// arbiter only releases a grant when its winner drops cyc. If the
	// blitter ever held one while waiting for the other, and the CPU
	// held the opposite, the machine would lock solid. It cannot: the
	// blitter's state machine keeps its two master ports strictly
	// alternating, never asserting both, and rtl/gpu/bench/tb_memblit.v
	// asserts exactly that on every cycle of every test.

`ifdef GPU_BLIT

	wb_arbiter_main marb0_i (
		.clk(wbm_clk),
		.rst(wbm_rst),

		// Master 0: CPU (through the instruction cache, if built)
		.m0_adr_i(wbc_adr),
		.m0_dat_i(wbc_dat_o),
		.m0_dat_o(wbc_dat_i),
		.m0_we_i(wbc_we),
		.m0_sel_i(wbc_sel),
		.m0_stb_i(wbc_stb),
		.m0_cyc_i(wbc_cyc),
		.m0_ack_o(wbc_ack),

		// Master 1: blitter source reads
		.m1_adr_i(wbm_blitsrc_adr),
		.m1_dat_i(32'h0),
		.m1_dat_o(wbm_blitsrc_dat_i),
		.m1_we_i(wbm_blitsrc_we),
		.m1_sel_i(wbm_blitsrc_sel),
		.m1_stb_i(wbm_blitsrc_stb),
		.m1_cyc_i(wbm_blitsrc_cyc),
		.m1_ack_o(wbm_blitsrc_ack),

		// Master 2: audio mixer sample fetches. Tied off on a board
		// without `AUDIO rather than left dangling -- an unconnected
		// cyc/stb input would be x in simulation and whatever the
		// synthesizer felt like in hardware, and this arbiter grants
		// on cyc && stb.
`ifdef AUDIO
		.m2_adr_i(wbm_audio_adr),
		.m2_dat_i(wbm_audio_dat_o),
		.m2_dat_o(wbm_audio_dat_i),
		.m2_we_i(wbm_audio_we),
		.m2_sel_i(wbm_audio_sel),
		.m2_stb_i(wbm_audio_stb),
		.m2_cyc_i(wbm_audio_cyc),
		.m2_ack_o(wbm_audio_ack),
`else
		.m2_adr_i(32'h0),
		.m2_dat_i(32'h0),
		.m2_dat_o(),
		.m2_we_i(1'b0),
		.m2_sel_i(4'h0),
		.m2_stb_i(1'b0),
		.m2_cyc_i(1'b0),
		.m2_ack_o(),
`endif

		.s_adr_o(wbm_adr),
		.s_dat_o(wbm_dat_o),
		.s_dat_i(wbm_dat_i),
		.s_we_o(wbm_we),
		.s_sel_o(wbm_sel),
		.s_stb_o(wbm_stb),
		.s_cyc_o(wbm_cyc),
		.s_ack_i(wbm_ack),

		.master(marb_master)
	);

`else

	// No blitter in this bitstream, so no second master -- wire the
	// CPU straight through and the main bus behaves exactly as it did
	// before the arbiter existed.
	assign wbm_adr = wbc_adr;
	assign wbm_dat_o = wbc_dat_o;
	assign wbc_dat_i = wbm_dat_i;
	assign wbm_sel = wbc_sel;
	assign wbm_we = wbc_we;
	assign wbm_stb = wbc_stb;
	assign wbc_ack = wbm_ack;
	assign wbm_cyc = wbc_cyc;

	assign wbm_blitsrc_dat_i = 32'h0;
	assign wbm_blitsrc_ack = 1'b0;
	assign marb_master = 2'b00;

`endif

	// WISHBONE MASTER: GPU Rasterizer
`ifdef GPU_RASTER
	wire [31:0] wbm_gpu_adr;
	wire [31:0] wbm_gpu_dat_i;
	wire [31:0] wbm_gpu_dat_o;
	wire [3:0] wbm_gpu_sel;
	wire wbm_gpu_we;
	wire wbm_gpu_stb;
	wire wbm_gpu_ack;
	wire wbm_gpu_cyc;

	wire wbm_cyc_gpu = cs_gpu && wbm_cyc;

	gpu_raster_wb #() wbm_gpu0_i
	(
 		.clk(wbm_clk),
 		.rst(wbm_rst),

		// slave interface for command input
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_gpu_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_cyc_i(wbm_cyc_gpu),
		.wb_ack_o(wbs_gpu_ack_o),

		// master interface to VRAM
		.m_adr_o(wbm_gpu_adr),
		.m_dat_i(wbm_gpu_dat_i),
		.m_dat_o(wbm_gpu_dat_o),
		.m_cyc_o(wbm_gpu_cyc),
		.m_stb_o(wbm_gpu_stb),
		.m_we_o(wbm_gpu_we),
		.m_sel_o(wbm_gpu_sel),
		.m_ack_i(wbm_gpu_ack),
//		.dbg(DBG)
	);
`endif

	// glyph memory wires: always declared (even if MEM_GLYPH isn't
	// built) so gpu_blit_wb's glyph_addr_o/glyph_data_i ports always
	// have something connected -- see the tie-off below for why this
	// matters.
	wire [11:0] wbm_blit_glyph_addr;
	wire [7:0]  wbm_blit_glyph_data;

`ifdef MEM_GLYPH
	// glyph memory: CPU loads font data via the wishbone slave port
	// (cs_glyph, above); the blitter reads it back via a direct,
	// non-wishbone port (glyph_addr/glyph_data below) -- no bus
	// arbitration, since nothing else ever needs to touch it.
	glyph_mem #(.ADDR_WIDTH(12)) glyph0_i
	(
		.clk(wbm_clk),

		.wb_cyc_i(cs_glyph && wbm_cyc),
		.wb_stb_i(wbm_stb),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_ack_o(wbs_glyph_ack_o),
		.wb_dat_o(wbs_glyph_dat_o),

		.blit_addr(wbm_blit_glyph_addr),
		.blit_data(wbm_blit_glyph_data)
	);
`else
	// no glyph memory built -- tie the blitter's read port to a fixed
	// value instead of leaving it floating. gpu_blit_wb's glyph mode
	// is only ever entered if software explicitly sets CTRL_GLYPH,
	// which it has no reason to do in a build without MEM_GLYPH, but
	// an undriven input is still worth avoiding for its own sake
	// (X-propagation in simulation, and just generally not something
	// you want floating on real silicon).
	assign wbm_blit_glyph_data = 8'h00;
`endif

`ifdef GPU_BLIT
	wire [31:0] wbm_gpu_blit_adr;
	wire [31:0] wbm_gpu_blit_dat_i;
	wire [31:0] wbm_gpu_blit_dat_o;
	wire [3:0] wbm_gpu_blit_sel;
	wire wbm_gpu_blit_we;
	wire wbm_gpu_blit_stb;
	wire wbm_gpu_blit_ack;
	wire wbm_gpu_blit_cyc;

	// Add chip select for blitter configuration
	wire wbm_cyc_gpu_blit = cs_gpu_blit && wbm_cyc;

	gpu_blit_wb #() wbm_blit0_i
	(
		.clk(wbm_clk),
		.rst(wbm_rst),

		// slave interface for configuration
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_gpu_blit_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_cyc_i(wbm_cyc_gpu_blit),
		.wb_ack_o(wbs_gpu_blit_ack_o),

		// master interface to VRAM
		.m_adr_o(wbm_gpu_blit_adr),
		.m_dat_i(wbm_gpu_blit_dat_i),
		.m_dat_o(wbm_gpu_blit_dat_o),
		.m_cyc_o(wbm_gpu_blit_cyc),
		.m_stb_o(wbm_gpu_blit_stb),
		.m_we_o(wbm_gpu_blit_we),
		.m_sel_o(wbm_gpu_blit_sel),
		.m_ack_i(wbm_gpu_blit_ack),

		// master interface to MAIN MEMORY (memory copy source reads).
		// Idle in every other mode -- see rtl/gpu/gpu_blit.v.
		.s_adr_o(wbm_blitsrc_adr),
		.s_dat_i(wbm_blitsrc_dat_i),
		.s_cyc_o(wbm_blitsrc_cyc),
		.s_stb_o(wbm_blitsrc_stb),
		.s_we_o(wbm_blitsrc_we),
		.s_sel_o(wbm_blitsrc_sel),
		.s_ack_i(wbm_blitsrc_ack),

		// always connected now (see the wire declaration/tie-off
		// above) -- no longer conditional on MEM_GLYPH
		.glyph_addr_o(wbm_blit_glyph_addr),
		.glyph_data_i(wbm_blit_glyph_data),

//		.busy(blit_busy)
	);

`endif

`ifdef ARBITER

	// WISHBONE ARBITER
	wire [31:0] wbm_cpu_arb0_dat_i;
	wire wbm_cpu_arb0_ack;

	wire wbm_cpu_arb0_cyc = cs_vram && wbm_cpu_cyc;

   wb_arbiter_vram #() varb0_i
   (
      .clk(wbm_clk),
      .rst(wbm_rst),
   
      // Master 0 (CPU)
      .m0_adr_i(wbm_cpu_adr),
      .m0_dat_i(wbm_cpu_dat_o),
      .m0_dat_o(wbm_cpu_arb0_dat_i), 
      .m0_we_i(wbm_cpu_we),
      .m0_sel_i(wbm_cpu_sel),
      .m0_stb_i(wbm_cpu_stb),
      .m0_cyc_i(wbm_cpu_arb0_cyc),
      .m0_ack_o(wbm_cpu_arb0_ack),

`ifdef GPU_RASTER
      // Master 1 (GPU rasterizer)
      .m1_adr_i(wbm_gpu_adr),
      .m1_dat_i(wbm_gpu_dat_o),
      .m1_dat_o(wbm_gpu_dat_i),
      .m1_we_i(wbm_gpu_we),
      .m1_sel_i(wbm_gpu_sel),
      .m1_stb_i(wbm_gpu_stb),
      .m1_cyc_i(wbm_gpu_cyc),
      .m1_ack_o(wbm_gpu_ack),
`endif

`ifdef GPU_BLIT
      // Master 2 (GPU blitter)
      .m2_adr_i(wbm_gpu_blit_adr),
      .m2_dat_i(wbm_gpu_blit_dat_o),
      .m2_dat_o(wbm_gpu_blit_dat_i),
      .m2_we_i(wbm_gpu_blit_we),
      .m2_sel_i(wbm_gpu_blit_sel),
      .m2_stb_i(wbm_gpu_blit_stb),
      .m2_cyc_i(wbm_gpu_blit_cyc),
      .m2_ack_o(wbm_gpu_blit_ack),
`endif

      // Selected master
      .s_adr_o(wbm_vram_adr), 
      .s_dat_i(wbs_vram_dat_o),
      .s_dat_o(wbm_vram_dat_o),
      .s_we_o(wbm_vram_we),
      .s_sel_o(wbm_vram_sel),
      .s_stb_o(wbm_vram_stb),
      .s_cyc_o(wbm_vram_cyc),
      .s_ack_i(wbs_vram_ack_o),
      .master(varb_master)
   );
   
`else

	assign wbm_vram_adr = wbm_cpu_adr;
	assign wbm_vram_dat_o = wbm_cpu_dat_o;
	assign wbm_vram_dat_i = wbm_cpu_dat_i;
	assign wbm_vram_sel = wbm_cpu_sel;
	assign wbm_vram_we = wbm_cpu_we;
	assign wbm_vram_stb = wbm_cpu_stb;
	assign wbm_vram_cyc = wbm_cpu_cyc;

	assign varb_master = 0;

`endif

	// WISHBONE SLAVE: BLOCK RAM (BIOS)

	wire wbm_cyc_bram = cs_bram && wbm_cyc;

	bram_wb #() wbs_bram0_i
	(
      .wb_clk_i(wbm_clk),
      .wb_rst_i(wbm_rst),
      .wb_adr_i(wbm_adr_sel_word),
      .wb_dat_i(wbm_dat_o),
      .wb_dat_o(wbs_bram_dat_o),
      .wb_we_i(wbm_we),
      .wb_sel_i(wbm_sel),
      .wb_stb_i(wbm_stb),
      .wb_ack_o(wbs_bram_ack_o),
      .wb_cyc_i(wbm_cyc_bram),
	);

	// WISHBONE SLAVE: SRAM (MAIN MEMORY)
`ifdef MEM_SRAM
	wire wbm_cyc_sram = cs_sram && wbm_cyc;

	sram_wb #() wbs_sram_i
	(
      .wb_clk_i(wbm_clk),
      .wb_rst_i(wbm_rst),
      .wb_adr_i(wbm_adr_sel_word),
      .wb_dat_i(wbm_dat_o),
      .wb_dat_o(wbs_sram_dat_o),
      .wb_we_i(wbm_we),
      .wb_sel_i(wbm_sel),
      .wb_stb_i(wbm_stb),
      .wb_ack_o(wbs_sram_ack_o),
      .wb_cyc_i(wbm_cyc_sram),
		.sram_d(SRAM_D),
		.sram_a(SRAM_A),
		.sram_cs({SRAM1_CE, SRAM0_CE}),
		.sram_oe({SRAM1_OE, SRAM0_OE}),
		.sram_we({SRAM1_WE, SRAM0_WE}),
		.sram_ub({SRAM1_UB, SRAM0_UB}),
		.sram_lb({SRAM1_LB, SRAM0_LB}),
	);
`endif

	// WISHBONE SLAVE: SDRAM (MAIN MEMORY)
`ifdef MEM_SDRAM

	wire wbm_cyc_sdram = cs_sdram && wbm_cyc;

	sdram_wb #(
		.SDRAM_CLK_FREQ(SYSCLK / 1_000_000)
	) sdram_i (
      .wb_clk_i(wbm_clk),
      .wb_rst_i(wbm_rst),
      .wb_adr_i({ wbm_adr_sel_word, 2'b00 }),
      .wb_dat_i(wbm_dat_o),
      .wb_dat_o(wbs_sdram_dat_o),
      .wb_we_i(wbm_we),
      .wb_sel_i(wbm_sel),
      .wb_stb_i(wbm_stb),
      .wb_ack_o(wbs_sdram_ack_o),
      .wb_cyc_i(wbm_cyc_sdram),
		.sdram_clk(sdram_clock),
		.sdram_cke(sdram_cke),
		.sdram_csn(sdram_cs_n),
		.sdram_rasn(sdram_ras_n),
		.sdram_casn(sdram_cas_n),
		.sdram_wen(sdram_we_n),
		.sdram_addr(sdram_a),
		.sdram_ba(sdram_ba),
		.sdram_dq(sdram_dq),
		.sdram_dqm(sdram_dm),
	);
`endif

	// WISHBONE SLAVE: DUAL-PORT VRAM (FRAMEBUFFER) [DEDICATED BUS]
`ifdef MEM_VRAM
	reg [15:0] gb_adr;
	reg [31:0] gb_dat;

	vram_wb #() wbs_vram_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i(wbm_vram_adr_sel_word),
		.wb_dat_i(wbm_vram_dat_o),
		.wb_dat_o(wbs_vram_dat_o),
		.wb_we_i(wbm_vram_we),
		.wb_sel_i(wbm_vram_sel),
		.wb_stb_i(wbm_vram_stb),
		.wb_ack_o(wbs_vram_ack_o),
		.wb_cyc_i(wbm_vram_cyc),
		.gb_adr_i(gb_adr),
		.gb_dat_o(gb_dat),
	);
`endif

	// WISHBONE SLAVE: QUAD QUAD SPI MODULE
`ifdef MEM_QQSPI
	wire wbm_cyc_qqspi = cs_qqspi && wbm_cyc;

	qqspi_wb #() wbs_qqspi_i
	(
      .wb_clk_i(wbm_clk),
      .wb_rst_i(wbm_rst),
      .wb_adr_i(wbm_adr_sel_word),
      .wb_dat_i(wbm_dat_o),
      .wb_dat_o(wbs_qqspi_dat_o),
      .wb_we_i(wbm_we),
      .wb_sel_i(wbm_sel),
      .wb_stb_i(wbm_stb),
      .wb_ack_o(wbs_qqspi_ack_o),
      .wb_cyc_i(wbm_cyc_qqspi),
   	.cen(QQSPI_SS),
`ifndef MEM_QQSPI_SINGLE
   	.cs({QQSPI_CS1, QQSPI_CS0}),
`endif
   	.sclk(QQSPI_SCK),
		.sio0_si_mosi(QQSPI_MOSI),
		.sio1_so_miso(QQSPI_MISO),
		.sio2(QQSPI_SIO2),
		.sio3(QQSPI_SIO3),
	);
`endif

	// WISHBONE SLAVE: READ-ONLY MEMORY (FLASH/MMOD)
`ifdef MEM_ROM
	wire wbm_cyc_rom = cs_rom && wbm_cyc;

`ifdef FPGA_ECP5
	wire CSPI_SCK;
	USRMCLK usrmclk0 (.USRMCLKI(CSPI_SCK), .USRMCLKTS(1'b0));
`endif

	spiflashro_wb #() wbs_rom0_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i({ wbm_adr_sel_word, 2'b00 }),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_rom_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_rom_ack_o),
		.wb_cyc_i(wbm_cyc_rom),
		.ss(CSPI_SS_FLASH),
		.sck(CSPI_SCK),
		.mosi(CSPI_MOSI),
		.miso(CSPI_MISO),
	);
`endif

	// WISHBONE SLAVE: GPIO + BOARD LEDS
	//
	// Always instantiated -- see rtl/gpio.v's header. NPORTS is the
	// count derived from `GPIO_PORT0..3 at the top of this file; on a
	// board with none it is 0 and this is the LED block rtl/debug.v
	// used to be, with the port register file optimised away.
	wire wbm_cyc_gpio = cs_gpio && wbm_cyc;

	wire [63:0] gpio_dir;
	wire [63:0] gpio_out;
	wire [63:0] gpio_in;

	gpio_wb #(
		.NPORTS(GPIO_NPORTS)
	) wbs_gpio0_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_gpio_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_gpio_ack_o),
		.wb_cyc_i(wbm_cyc_gpio),
		.led(LED_B),
`ifdef LED_DEBUG
		.leds(DBG),
`endif
		.gpio_dir_o(gpio_dir),
		.gpio_out_o(gpio_out),
		.gpio_in_i(gpio_in)
	);

	// -- GPIO tri-state buffers --
	//
	// These live HERE rather than inside gpio.v because this is the
	// file that knows about board pins, and because it keeps gpio.v a
	// plain register file with no `z in it -- which is what makes it
	// simulatable in rtl/tb/tb_gpio.v without a pad model.
	//
	// ONE DRIVER PER PIN, and that is the whole reason this is written
	// as eight separate single-bit assigns per port rather than
	// anything cleverer: a vector-wide conditional cannot express
	// per-bit high-Z (`dir ? out : 8'bz` turns the WHOLE byte off when
	// any bit of dir is 0 under some tools' interpretation), and a
	// second continuous assign to the same net is a multiply-driven
	// net that yosys resolves to X in simulation and to whatever it
	// feels like in hardware.
	//
	// A lane with no port built is tied to zero rather than left
	// floating: an undriven input to gpio.v's synchroniser would be an
	// implicit net feeding flops, which is not something anything
	// reports -- the same class of bug as the wbs_uart0_int wire
	// further down. Tying it low also lets synthesis prove those
	// synchroniser flops constant and remove them.
`ifdef GPIO_PORT0
	assign GPIO0[0] = gpio_dir[0] ? gpio_out[0] : 1'bz;
	assign GPIO0[1] = gpio_dir[1] ? gpio_out[1] : 1'bz;
	assign GPIO0[2] = gpio_dir[2] ? gpio_out[2] : 1'bz;
	assign GPIO0[3] = gpio_dir[3] ? gpio_out[3] : 1'bz;
`ifdef GPIO_PORT0_NARROW
	// The upper half has no pins. Tied low rather than left dangling
	// for the same reason an unbuilt port is: an implicit net feeding
	// gpio.v's synchroniser is not something anything reports.
	assign gpio_in[7:0] = { 4'h0, GPIO0 };
`else
	assign GPIO0[4] = gpio_dir[4] ? gpio_out[4] : 1'bz;
	assign GPIO0[5] = gpio_dir[5] ? gpio_out[5] : 1'bz;
	assign GPIO0[6] = gpio_dir[6] ? gpio_out[6] : 1'bz;
	assign GPIO0[7] = gpio_dir[7] ? gpio_out[7] : 1'bz;
	assign gpio_in[7:0] = GPIO0;
`endif
`else
	assign gpio_in[7:0] = 8'h00;
`endif

`ifdef GPIO_PORT1
	assign GPIO1[0] = gpio_dir[8] ? gpio_out[8] : 1'bz;
	assign GPIO1[1] = gpio_dir[9] ? gpio_out[9] : 1'bz;
	assign GPIO1[2] = gpio_dir[10] ? gpio_out[10] : 1'bz;
	assign GPIO1[3] = gpio_dir[11] ? gpio_out[11] : 1'bz;
	assign GPIO1[4] = gpio_dir[12] ? gpio_out[12] : 1'bz;
	assign GPIO1[5] = gpio_dir[13] ? gpio_out[13] : 1'bz;
	assign GPIO1[6] = gpio_dir[14] ? gpio_out[14] : 1'bz;
	assign GPIO1[7] = gpio_dir[15] ? gpio_out[15] : 1'bz;
	assign gpio_in[15:8] = GPIO1;
`else
	assign gpio_in[15:8] = 8'h00;
`endif

`ifdef GPIO_PORT2
	assign GPIO2[0] = gpio_dir[16] ? gpio_out[16] : 1'bz;
	assign GPIO2[1] = gpio_dir[17] ? gpio_out[17] : 1'bz;
	assign GPIO2[2] = gpio_dir[18] ? gpio_out[18] : 1'bz;
	assign GPIO2[3] = gpio_dir[19] ? gpio_out[19] : 1'bz;
	assign GPIO2[4] = gpio_dir[20] ? gpio_out[20] : 1'bz;
	assign GPIO2[5] = gpio_dir[21] ? gpio_out[21] : 1'bz;
	assign GPIO2[6] = gpio_dir[22] ? gpio_out[22] : 1'bz;
	assign GPIO2[7] = gpio_dir[23] ? gpio_out[23] : 1'bz;
	assign gpio_in[23:16] = GPIO2;
`else
	assign gpio_in[23:16] = 8'h00;
`endif

`ifdef GPIO_PORT3
	assign GPIO3[0] = gpio_dir[24] ? gpio_out[24] : 1'bz;
	assign GPIO3[1] = gpio_dir[25] ? gpio_out[25] : 1'bz;
	assign GPIO3[2] = gpio_dir[26] ? gpio_out[26] : 1'bz;
	assign GPIO3[3] = gpio_dir[27] ? gpio_out[27] : 1'bz;
	assign GPIO3[4] = gpio_dir[28] ? gpio_out[28] : 1'bz;
	assign GPIO3[5] = gpio_dir[29] ? gpio_out[29] : 1'bz;
	assign GPIO3[6] = gpio_dir[30] ? gpio_out[30] : 1'bz;
	assign GPIO3[7] = gpio_dir[31] ? gpio_out[31] : 1'bz;
	assign gpio_in[31:24] = GPIO3;
`else
	assign gpio_in[31:24] = 8'h00;
`endif

	// Ports 4-7 are reserved in gpio.v's register map but have no pins
	// on any board here. Tied low so the synchroniser folds away; see
	// the port declarations at the top of this file for what adding
	// one involves.
	assign gpio_in[63:32] = 32'h0000_0000;

	// WISHBONE SLAVE: UART0
	//
	// Three mutually exclusive things can answer this window, and the
	// software half of the tree cannot tell them apart -- which is
	// the whole design. sw/bios/bios.c, sw/os/uart.c and sw/os/sh.c
	// reach the console through sw/common/zeitlos.h's reg_uart0_*
	// macros and are identical on all three:
	//
	//   `USB_CDC   rtl/usb_cdc_uart.v -- a real console, on the
	//              USB-C socket, with a 16550 register map in front
	//              of a CDC-ACM device. No PMOD, no USB-UART dongle.
	//   `UART0     rtl/ext/uart16550 -- a real console, on PMOD pins.
	//   neither    rtl/uart_null.v -- a UART-shaped hole that acks
	//              and discards, so a board with no console at all
	//              still boots.
	//
	// rtl/boards.vh `undef's `UART0 when `USB_CDC is set, so the
	// first two cannot both be built and the UART0_TX/UART0_RX pins
	// cannot be declared with nothing driving them.
`ifdef USB_CDC
	wire wbm_cyc_uart0 = cs_uart0 && wbm_cyc;
	wire wbm_stb_uart0 = cs_uart0 && wbm_stb;

	wire usb_cdc_configured;

	usb_cdc_uart #(
		.VENDORID(`USB_CDC_VID),
		.PRODUCTID(`USB_CDC_PID),
		.MAXPACKETSIZE(`USB_CDC_MPS),
		.STALL_CYCLES(`USB_CDC_STALL_CYCLES)
	) wbs_uart0_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_uart0_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb_uart0),
		.wb_cyc_i(wbm_cyc_uart0),
		.wb_ack_o(wbs_uart0_ack_o),
		.int_o(wbs_uart0_int),
		.usb_dp(usb_ufp_dp),
		.usb_dn(usb_ufp_dm),
		.usb_pu(usb_ufp_pull),
		.configured_o(usb_cdc_configured)
	);
`elsif UART0
	wire wbm_cyc_uart0 = cs_uart0 && wbm_cyc;
	wire wbm_stb_uart0 = cs_uart0 && wbm_stb;

	uart_top #() wbs_uart0_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_uart0_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb_uart0),
		.wb_ack_o(wbs_uart0_ack_o),
		.wb_cyc_i(wbm_cyc_uart0),
		.stx_pad_o(UART0_TX),
		.srx_pad_i(UART0_RX),
		.cts_pad_i(1'b1),
		.dsr_pad_i(1'b1),
		.ri_pad_i(1'b1),
		.dcd_pad_i(1'b1),
		.int_o(wbs_uart0_int)
	);
`else
	// No `UART0. The window is still decoded and still acked -- by
	// rtl/uart_null.v, which reports a transmitter that is always ready
	// and a receiver that never has data.
	//
	// This branch is not a nicety. Before it existed, leaving `UART0
	// out meant cs_uart0 vanished from the ack mux, the mux fell
	// through to 1'b0, and the read in sw/bios/bios.c's putchar()
	//
	//     while ((reg_uart0_lsr & 0x20) == 0);
	//
	// never completed -- so the CPU stalled on the first character of
	// the boot banner, before anything reached a screen. Every other
	// optional block here degrades to "acks, reads zero"; this makes
	// the UART do the same.
	//
	// Software needs no changes to cope with it, which is the point:
	// the alternative was a Z_FEATURE_UART0 check in the BIOS, the
	// kernel console, the shell and uart.c's ISR, all of them dead
	// code on every board that has a UART. Software that wants to
	// TELL the user there is no console rather than merely survive
	// should still check that bit (sw/common/zsoc.h, bit 12) -- it is
	// clear on a build using this.
	uart_null wbs_uart0_null_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_uart0_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(cs_uart0 && wbm_stb),
		.wb_cyc_i(cs_uart0 && wbm_cyc),
		.wb_ack_o(wbs_uart0_ack_o),
		.int_o(wbs_uart0_int)
	);
`endif

	// WISHBONE SLAVE: UART1 (ESP32 data plane on ULX3S GPIO16/17)
`ifdef UART1
	wire wbs_uart1_int;
	wire wbm_cyc_uart1 = cs_uart1 && wbm_cyc;
	wire wbm_stb_uart1 = cs_uart1 && wbm_stb;

	uart_top #() wbs_uart1_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_uart1_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb_uart1),
		.wb_ack_o(wbs_uart1_ack_o),
		.wb_cyc_i(wbm_cyc_uart1),
		.stx_pad_o(UART1_TX),
		.srx_pad_i(UART1_RX),
		.cts_pad_i(1'b1),
		.dsr_pad_i(1'b1),
		.ri_pad_i(1'b1),
		.dcd_pad_i(1'b1),
		.int_o(wbs_uart1_int)
	);
`endif

	// ESP32 EN / GPIO0 -- software-controlled reset. Default held in
	// reset (en=0) so the module does not fight the SD bus at boot.
`ifdef ESP32_LINK
	reg wifi_en_r;
	reg wifi_gpio0_r;
	assign wifi_en = wifi_en_r;
	assign wifi_gpio0 = wifi_gpio0_r;

	reg [31:0] esp32ctl_dat;
	reg esp32ctl_ack;
	assign wbs_esp32ctl_dat_o = esp32ctl_dat;
	assign wbs_esp32ctl_ack_o = esp32ctl_ack;

	wire wbm_cyc_esp32ctl = cs_esp32ctl && wbm_cyc;

	always @(posedge wbm_clk) begin
		esp32ctl_ack <= 0;
		if (wbm_rst) begin
			wifi_en_r <= 1'b0;
			wifi_gpio0_r <= 1'b1;
		end else if (wbm_cyc_esp32ctl && wbm_stb && !esp32ctl_ack) begin
			esp32ctl_ack <= 1;
			if (wbm_we) begin
				wifi_en_r <= wbm_dat_o[0];
				wifi_gpio0_r <= wbm_dat_o[1];
			end else begin
				esp32ctl_dat <= {30'b0, wifi_gpio0_r, wifi_en_r};
			end
		end
	end

	// 2 KiB block-RAM receive FIFO on the same UART1 RX pin (see
	// rtl/esp32_rxfifo.v): the 16550's 16 bytes are not enough for a
	// polled, time-sliced reader at 1 Mbaud.
	esp32_rxfifo #(.CLK_PER_BIT(48), .DEPTH_BITS(11)) wbs_esp32rx_i (
		.clk(wbm_clk),
		.rst(wbm_rst),
		.rx(UART1_RX),
		.wb_adr_i(wbm_adr),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_esp32rx_dat_o),
		.wb_we_i(wbm_we),
		.wb_stb_i(wbm_stb && cs_esp32rx),
		.wb_cyc_i(wbm_cyc && cs_esp32rx),
		.wb_ack_o(wbs_esp32rx_ack_o)
	);
`endif

	// WISHBONE SLAVE: HARDWARE SPI MASTER FOR SDCARD
`ifdef SPI_SDCARD
	wire wbm_cyc_spisdcard = cs_spisdcard && wbm_cyc;

	// spisd_wb, not spibb_wb: SCLK is generated in gateware rather
	// than by the CPU toggling pins. See rtl/spisd.v -- the bit-banged
	// version made the SPI clock rate a function of compiler codegen,
	// which broke when the toolchain changed and would break again
	// with an instruction cache enabled.
	spim_wb #(.DEFAULT_DIV(8'd59)) wbs_spisd0_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_spisdcard_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_spisdcard_ack_o),
		.wb_cyc_i(wbm_cyc_spisdcard),
		.spi_cs_n(SD_SS),
		.spi_miso(SD_MISO),
		.spi_mosi(SD_MOSI),
		.spi_sck(SD_SCK),
		.spi_int(1'b1)		// sdcards have no interrupt line
	);
`endif

	// WISHBONE SLAVE: HARDWARE SPI MASTER FOR ETH (ENC28J60)
	//
	// Was bit-banged (rtl/spibb_eth.v) and is not any more -- see
	// rtl/spim.v, which replaced both spibb variants. The old name
	// survived here longer than the old module did.
`ifdef SPI_ETH
	wire wbm_cyc_spieth = cs_spieth && wbm_cyc;

	// spim_wb, same module as the sdcard above. The ENC28J60 needs no
	// slow init phase -- it takes full speed from reset -- so its
	// divider starts fast rather than at 400kHz. See rtl/spim.v.
	spim_wb #(.DEFAULT_DIV(8'd1)) wbs_spieth0_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_spieth_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_spieth_ack_o),
		.wb_cyc_i(wbm_cyc_spieth),
		.spi_cs_n(ETH_SS),
		.spi_miso(ETH_MISO),
		.spi_mosi(ETH_MOSI),
		.spi_sck(ETH_SCLK),
		.spi_int(ETH_INT)
	);

	// The ENC28J60's INT pin is ACTIVE LOW, so it inverts into the
	// same active-high interrupt line the RMII MAC drives. Software
	// then sees one Z_IRQ_ETH regardless of which MAC the board has,
	// which is the point -- sw/apps/net already abstracts over the two
	// and should not have to learn the difference here.
	//
	// This is the wire spim.v's own comment says would be "strictly
	// better than a timer": the pin was already routed and readable in
	// STATUS bit 2, it simply was not connected to anything that could
	// wake a blocked process.
	// Active low, and SYNCHRONISED before the edge detector above --
	// this is an asynchronous pin from another chip, and an
	// unsynchronised signal feeding an edge detector produces spurious
	// pulses on metastability, which for an interrupt means a storm
	// that appears at random.
	reg eth_int_s0, eth_int_s1;
	always @(posedge wbm_clk) begin
		eth_int_s0 <= ~ETH_INT;
		eth_int_s1 <= eth_int_s0;
	end
	assign eth_rx_ready = eth_int_s1;
`endif

	// WISHBONE SLAVE: RMII ETHERNET MAC (tested with LAN8720A)
`ifdef ETH_RMII
	wire wbm_cyc_ethmac = cs_ethmac && wbm_cyc;

`ifdef ETH_RMII_DRIVE_REFCLK
   wire eth_refclk = clk50mhz;
	assign ETH_REFCLK = eth_refclk;
`else
   wire eth_refclk = ETH_REFCLK;
`endif

	ethmac_rmii_wb #() wbs_ethmac0_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_ethmac_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_ethmac_ack_o),
		.wb_cyc_i(wbm_cyc_ethmac),
		.eth_refclk(eth_refclk),
		.eth_rxd(ETH_RXD),
		.eth_txd(ETH_TXD),
		.eth_tx_en(ETH_TX_EN),
		.eth_crs_dv(ETH_CRS_DV),
		.eth_rst_n(ETH_RST_N),
		.eth_int_o(eth_rx_ready)
	);
`endif

	// WISHBONE SLAVE: CSRs (rtl/csrs.v) -- always instantiated, no
	// `ifdef guard, on every board regardless of what else is built
	// in -- see csrs.v's own header comment for why. MEM_MB/FEATURES
	// come straight from this file's own `MEM/CSR_FEATURES above.
	wire wbm_cyc_csrs = cs_csrs && wbm_cyc;

	// WISHBONE SLAVE: SOC CONTROL (writable global config)
	//
	// Always instantiated, no `ifdef -- same reasoning as csrs.v below.
	// cursor_busy goes straight to rtl/gpu/gpu_cursor.v; it is declared
	// unconditionally so the wire exists even on boards built without
	// GPU_CURSOR, where it simply goes nowhere.
	wire socctl_cursor_busy;

	// Virtual phosphor mode -- socctl.v holds it, rtl/gpu/gpu_video.v
	// consumes it. Declared unconditionally for the same reason
	// socctl_cursor_busy above is.
	wire [1:0] socctl_video_mode;

	// `GPU_AMBER / `GPU_GREEN / `GPU_PAPER (rtl/boards.vh) used to be
	// read directly by gpu_video.v and hard-wired the colour at
	// synthesis. They now choose only the POWER-ON DEFAULT, and
	// software can change it afterwards at any time.
	//
	// This lives here rather than in socctl.v so that socctl stays a
	// generic register block: board configuration is this file's job,
	// and boards.vh is included here. socctl gets a number.
	//
	// Order matters if more than one is somehow defined -- first match
	// wins, and there is no "both" to express. That is a build-file
	// mistake rather than a state worth encoding.
	localparam [1:0] VIDEO_MODE_DEFAULT =
`ifdef GPU_AMBER
		2'd1;
`elsif GPU_GREEN
		2'd2;
`elsif GPU_PAPER
		2'd3;
`else
		2'd0;
`endif

	// -- game mode plumbing --
	//
	// socctl.v holds the configuration and gpu_video.v consumes it;
	// these wires are the path between them. Declared unconditionally,
	// exactly like socctl_cursor_busy and socctl_video_mode above, so
	// socctl's port list never changes with the board -- on a board
	// without `GPU they simply reach nothing, and the status wires
	// coming back are tied off below.
	wire socctl_view_load;
	wire socctl_game_en;
	wire socctl_game_wrap;
	wire [9:0] socctl_view_x;
	wire [9:0] socctl_view_y;

	// Availability is `GAME AND `GPU, not `GAME alone. A board with
	// game mode built but no video hardware has nothing to scan out
	// with, and reporting the mode as available there would be a lie
	// software cannot check any other way -- so the and happens here,
	// once, and socctl is simply told the answer. Same arrangement as
	// VIDEO_MODE_DEFAULT directly above: the board defines live in
	// this file, socctl gets a number.
`ifdef GAME
`ifdef GPU
	localparam GAME_AVAILABLE = 1'b1;
`else
	localparam GAME_AVAILABLE = 1'b0;
`endif
`else
	localparam GAME_AVAILABLE = 1'b0;
`endif

	// Scanout status coming back the other way. Zero on a board with
	// no `GPU, which is the honest answer -- there is no scanout, so
	// there are no frames and there is no vertical blanking. A stuck
	// counter would be worse than a zero one: software waiting for it
	// to change would wait forever, whereas software that reads zero
	// twice can at least conclude nothing is happening.
	wire [15:0] gpu_frame_ctr;
	wire gpu_in_vblank;
`ifndef GPU
	assign gpu_frame_ctr = 16'd0;
	assign gpu_in_vblank = 1'b0;
`endif

	socctl_wb #(
		.VIDEO_MODE_RESET(VIDEO_MODE_DEFAULT),
		.GAME_AVAIL(GAME_AVAILABLE)
	) socctl_i (
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		// Masked to this block's own window, NOT the raw word address.
		// socctl lives at 0x7000_02xx, so wbm_adr_sel_word is 0x80 for
		// its first register, not 0 -- passing it straight through
		// means no case ever matches, writes vanish and MAGIC reads
		// back as zero. csrs.v gets away with the raw value only
		// because its window starts at offset 0. Same masking as the
		// icache block above.
		//
		// THREE bits, not two. socctl had four registers and needed
		// only [1:0]; GAME/VIEW/FRAME take it to six, so the mask has
		// to widen or words 4 and 5 alias back onto 0 and 1 -- which
		// would mean a VIEW write silently overwrote CTRL and turned
		// the mouse cursor into a Z every time the viewport moved.
		// Eight words is the whole of 0x7000_0200..0x7000_021c, well
		// inside socctl's 256-byte window, so there is room to widen
		// again if a seventh register ever turns up.
		.wb_adr_i({ 29'b0, wbm_adr_sel_word[2:0] }),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_socctl_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_socctl_ack_o),
		.wb_cyc_i(wbm_cyc_socctl),
		.cursor_busy(socctl_cursor_busy),
		.video_mode(socctl_video_mode),
		.view_load(socctl_view_load),
		.game_en(socctl_game_en),
		.game_wrap(socctl_game_wrap),
		.view_x(socctl_view_x),
		.view_y(socctl_view_y),
		.frame_ctr(gpu_frame_ctr),
		.in_vblank(gpu_in_vblank)
	);

	csrs_wb #(
		.MEM_MB(`MEM),
		.FEATURES(CSR_FEATURES),
		.FEATURES2(CSR_FEATURES2)
	) wbs_csrs0_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_csrs_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_csrs_ack_o),
		.wb_cyc_i(wbm_cyc_csrs),
	);

	// WISHBONE SLAVE: RTC (rtl/rtc.v) -- wall-clock seconds since the
	// Unix epoch, plus a 1/1024s fraction.
	//
	// Optional, `RTC in rtl/boards.vh, which defines it at the
	// universal level so every board gets one by default -- it needs
	// no pins and no board support, so there is no board-specific
	// reason to want it or not. Without it, csrs.v absorbs this
	// window (see the cs_csrs comment above), reads return 0, the
	// FEATURE bit is clear and software correctly concludes there is
	// no clock here.
	//
	// NOT the same thing as rtc_ctr further up this file, despite the
	// name they share, and NOT affected by this define. That one is
	// the ~732Hz KTIMER divider and counts ticks since boot; this one
	// has an epoch and answers what the date is. See rtc.v's own
	// header comment.
	//
	// wb_adr_i is masked to this block's own window rather than being
	// the raw word address, exactly like the socctl and icache blocks
	// above -- the RTC lives at 0x7000_03xx, so wbm_adr_sel_word is
	// 0xC0 for its first register, not 0. Passing it straight through
	// would mean no case ever matches: writes would vanish and MAGIC
	// would read back zero. Three bits, not two, because this block
	// has six registers.
	//
	// CLK_HZ is SYSCLK (48MHz on every board in this lineup), which
	// the prescaler divides by 1024 exactly -- see rtc.v.
`ifdef RTC
	rtc_wb #(
		.CLK_HZ(SYSCLK)
	) rtc_i (
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i({ 29'b0, wbm_adr_sel_word[2:0] }),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_rtc_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_rtc_ack_o),
		.wb_cyc_i(wbm_cyc_rtc)
	);
`endif

	// WISHBONE SLAVE: TRNG
	//
	// Defaults are trng.v's own -- eight oscillators, 13 stages in the
	// shortest, sampled every 256 cycles. Only CLK_HZ is passed, and
	// only so the block can advertise a correct RATE; nothing here
	// depends on the system clock otherwise.
`ifdef TRNG
	trng_wb #(
		.CLK_HZ(SYSCLK)
	) trng_i (
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i({ 29'b0, wbm_adr_sel_word[2:0] }),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_trng_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_trng_ack_o),
		.wb_cyc_i(wbm_cyc_trng)
	);
`endif

	// WISHBONE SLAVE: AUDIO (rtl/audio.v)
	//
	// Optional, `AUDIO in rtl/boards.vh -- unlike `RTC and `TRNG this
	// is per-board rather than universal, because it needs pins and a
	// DAC on the other end of them. Without it csrs.v absorbs the
	// 0x7000_05xx window (see the cs_csrs comment above), reads return
	// 0, the FEATURE bit is clear, and z_audio_present()
	// (sw/common/zaudio.h) correctly answers false.
	//
	// SIX address bits, not three as rtc.v and trng.v use. The block
	// has eight registers today and the phase-3 hardware mixer adds
	// per-channel state; six covers the whole 256-byte window, so this
	// decode never has to be revisited. Masking at all is not optional
	// -- at 0x7000_05xx the raw wbm_adr_sel_word is 0x140 for register
	// 0, and passing it through unmasked means no case ever matches:
	// writes vanish and MAGIC reads back zero, with no error anywhere.
	//
	// FORMATS tells software which DAC is actually wired here, which it
	// cannot otherwise know -- the register interface is identical
	// either way. Bit 2 is reserved for the optical S/PDIF transmitter
	// on Sergei; nothing sets it yet.
`ifdef AUDIO
	audio_wb #(
		.DEPTH_LOG2(`AUDIO_FIFO_LOG2),
		.FORMATS(AUDIO_FORMATS),
		.CLK_HZ(SYSCLK),
		.RATE_RESET(`AUDIO_RATE_RESET),
		.CTRL_RESET(`AUDIO_CTRL_RESET)
	) audio_i (
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i({ 26'b0, wbm_adr_sel_word[5:0] }),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_audio_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_audio_ack_o),
		.wb_cyc_i(wbm_cyc_audio),
		.int_o(wbs_audio_int),

		// Only the ports this board has pins for are connected. The
		// other serialiser reaches no output and yosys prunes it --
		// which is why rtl/audio_out.v has no `ifdef or generate block
		// choosing between the two formats. See docs/audio.md for the
		// measured per-board cost, which differs because of this.
`ifdef AUDIO_SD
		.AUDIO_L(AUDIO_L),
		.AUDIO_R(AUDIO_R),
`else
		.AUDIO_L(),
		.AUDIO_R(),
`endif
`ifdef AUDIO_PT8211
		.AUD_BCK(AUD_BCK),
		.AUD_WS(AUD_WS),
		.AUD_DIN(AUD_DIN),
`else
		.AUD_BCK(),
		.AUD_WS(),
		.AUD_DIN(),
`endif
`ifdef AUDIO_SPDIF
		.AUD_OPTICAL(AUD_OPTICAL),
`else
		.AUD_OPTICAL(),
`endif

		.mx_adr_o(wbm_audio_adr),
		.mx_dat_o(wbm_audio_dat_o),
		.mx_dat_i(wbm_audio_dat_i),
		.mx_we_o(wbm_audio_we),
		.mx_sel_o(wbm_audio_sel),
		.mx_stb_o(wbm_audio_stb),
		.mx_cyc_o(wbm_audio_cyc),
		.mx_ack_i(wbm_audio_ack)
	);
`endif

	// WISHBONE SLAVE: USB HID
`ifdef USB_HID
	reg wbs_usb0_int;
	wire wbm_cyc_usb0 = cs_usb0 && wbm_cyc;

	wire [1:0] wbs_usb0_typ;
	wire [9:0] wbs_usb0_curs_x, wbs_usb0_curs_y;

	// Port 0: Obst/Lakritz onboard USB; ULX3S maps this to US2
	// (see boards/ulx3s.lpf). Port 1 is the second physical socket
	// (J1 on ULX3S, for an optional Dual USB Host PMOD).

	usb_hid_wb #(.SENS_SHIFT(USB_HID_SENS)) wbs_usb0_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_usb0_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_usb0_ack_o),
		.wb_cyc_i(wbm_cyc_usb0),
		.int_o(wbs_usb0_int),
		.usb_clk(clk12mhz),
		.usb_dm(usb_host_dm[0]),
		.usb_dp(usb_host_dp[0]),
		.curs_x(wbs_usb0_curs_x),
		.curs_y(wbs_usb0_curs_y),
		.typ(wbs_usb0_typ),
	);

	// second port -- see boards/*.lpf's usb_host_dp[1]/usb_host_dm[1]
	// (Obst, Lakritz both wire a second USB host port). Identical
	// instance, own register slot (cs_usb1, bit4 of the address --
	// see the cs_usb0/cs_usb1 comment above), own interrupt bit
	// (cpu_irq[6] below). Which physical device ends up here isn't
	// fixed at the hardware level at all -- software (sw/os/hid.c,
	// sw/apps/wm/wm.c) reads both instances' own `typ` and decides.
	reg wbs_usb1_int;
	wire wbm_cyc_usb1 = cs_usb1 && wbm_cyc;

	wire [1:0] wbs_usb1_typ;
	wire [9:0] wbs_usb1_curs_x, wbs_usb1_curs_y;

	usb_hid_wb #(.SENS_SHIFT(USB_HID_SENS)) wbs_usb1_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_usb1_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_usb1_ack_o),
		.wb_cyc_i(wbm_cyc_usb1),
		.int_o(wbs_usb1_int),
		.usb_clk(clk12mhz),
		.usb_dm(usb_host_dm[1]),
		.usb_dp(usb_host_dp[1]),
		.curs_x(wbs_usb1_curs_x),
		.curs_y(wbs_usb1_curs_y),
		.typ(wbs_usb1_typ),
	);
`endif

	// GPU: Video Generator
`ifdef GPU
	wire [9:0] gpu_x;
	wire [9:0] gpu_y;
	wire gpu_pixel;

	assign gpu_pixel =
`ifdef GPU_CURSOR
		gpu_curs_pixel;
`else
		0;
`endif

	// -- scanout timing --
	//
	// Three parameter sets, one build. VGA/DDMI is 640x480@60 with the
	// framebuffer 1:1 against the signal; composite is 320x240 spread
	// four pixel clocks per source pixel. All three run from the SAME
	// 25.2MHz pclk -- composite needs no new PLL output, which is most
	// of why it is cheap. See docs/composite.md for the derivation.
	//
	// The horizontal numbers are in PIXEL CLOCKS, not source pixels, so
	// h_disp is 1280 for composite (320 x 4) rather than 320. That is
	// what lets one timing generator serve both: the divisor lives in
	// H_DIV_BASE and the counters never need to know about it.
	//
	// The slack between the nominal porches and the exact line length
	// is split between front and back porch rather than dumped on one,
	// so the 1280-clock image sits centred in the active window instead
	// of hard against its left edge.
`ifdef GPU_COMPOSITE
`ifdef GPU_COMPOSITE_PAL
	// PAL 288p: 1613 clk/line = 64.0079us (+0.012%), 312 lines = 50.07Hz
	localparam [10:0] VID_H_DISP = 11'd1280, VID_H_FP = 11'd57;
	localparam [10:0] VID_H_PW   = 11'd118,  VID_H_BP = 11'd158;
	localparam [10:0] VID_V_DISP = 11'd240,  VID_V_FP = 11'd25;
	localparam [10:0] VID_V_PW   = 11'd3,    VID_V_BP = 11'd44;
`else
	// NTSC 240p: 1602 clk/line = 63.5714us (+0.025%), 262 lines = 60.04Hz
	localparam [10:0] VID_H_DISP = 11'd1280, VID_H_FP = 11'd65;
	localparam [10:0] VID_H_PW   = 11'd118,  VID_H_BP = 11'd139;
	localparam [10:0] VID_V_DISP = 11'd240,  VID_V_FP = 11'd3;
	localparam [10:0] VID_V_PW   = 11'd3,    VID_V_BP = 11'd16;
`endif
	localparam [2:0] VID_H_DIV = 3'd4;
	localparam VID_FIXED_VP = 1'b1;
`else
	// VESA DMT 640x480@60 -- unchanged from every previous bitstream.
	localparam [10:0] VID_H_DISP = 11'd640, VID_H_FP = 11'd16;
	localparam [10:0] VID_H_PW   = 11'd96,  VID_H_BP = 11'd48;
	localparam [10:0] VID_V_DISP = 11'd480, VID_V_FP = 11'd10;
	localparam [10:0] VID_V_PW   = 11'd2,   VID_V_BP = 11'd33;
	localparam [2:0] VID_H_DIV = 3'd1;
	localparam VID_FIXED_VP = 1'b0;
`endif

	gpu_video #(
		.h_disp(VID_H_DISP), .h_front_porch(VID_H_FP),
		.h_pulse_width(VID_H_PW), .h_back_porch(VID_H_BP),
		.h_line(VID_H_FP + VID_H_PW + VID_H_BP + VID_H_DISP),
		.v_disp(VID_V_DISP), .v_front_porch(VID_V_FP),
		.v_pulse_width(VID_V_PW), .v_back_porch(VID_V_BP),
		.v_frame(VID_V_FP + VID_V_PW + VID_V_BP + VID_V_DISP),
		.H_DIV_BASE(VID_H_DIV),
		.FIXED_VIEWPORT(VID_FIXED_VP)
	) gpu_video_i
	(
		.clk(wbm_clk),
		.pclk(clk25_2mhz),
		.bclk(clk126mhz),
		.resetn(~wbm_rst),
		.pixel(gpu_pixel),
		.video_mode(socctl_video_mode),
		.view_load(socctl_view_load),
		// GAME_AVAILABLE is already `GAME && `GPU, and socctl has
		// already gated its own enable bit with it -- so this is belt
		// and braces. It is cheap belt and braces: yosys sees a
		// constant 0 on a board without `GAME and folds the whole
		// game-mode datapath out of gpu_video (the row adder, the
		// wrap comparator, the doubling phase flop), leaving the
		// scanout path bit-for-bit what it was before this feature
		// existed. A board that opts out pays nothing.
		.game_en(socctl_game_en && GAME_AVAILABLE),
		.game_wrap(socctl_game_wrap),
		.view_x(socctl_view_x),
		.view_y(socctl_view_y),
		.frame_ctr(gpu_frame_ctr),
		.in_vblank(gpu_in_vblank),
		// x and y are FRAMEBUFFER coordinates now, in both modes --
		// see gpu_video.v's header. In desktop mode that is the same
		// thing as the screen coordinate they used to be, so nothing
		// downstream changed; in game mode it is what lets
		// gpu_cursor.v below stay completely unmodified and still
		// draw the pointer in the right place, pixel-doubled.
		.x(gpu_x),
		.y(gpu_y),
		.gb_adr_o(gb_adr),
		.gb_dat_i(gb_dat),
`ifdef GPU_COMPOSITE
		// Composite REPLACES the VGA and DDMI connections rather than
		// joining them -- see boards.vh's own note on why the two
		// cannot share one timing generator. Written as an `ifdef/
		// `else here rather than trusting a board author to comment
		// out `GPU_VGA as well: a board that defined both would
		// otherwise build, drive VGA pins with 15.7kHz sync, and fail
		// in a way that looks like broken hardware.
		.dac(COMP_DAC),
`else
`ifdef GPU_VGA
		.red(VGA_R),
		.green(VGA_G),
		.blue(VGA_B),
		.hsync(VGA_HS),
		.vsync(VGA_VS),
`endif
`ifdef GPU_DDMI
		.dvi_p({ DDMI_CK_P, DDMI_D2_P, DDMI_D1_P, DDMI_D0_P }),
`endif
`endif
	);

`endif

	// GPU: Hardware Cursor
`ifdef GPU_CURSOR
	wire [9:0] gpu_curs_x;
	wire [9:0] gpu_curs_y;
	wire gpu_curs_pixel;

	// the sprite has one position to render, but there are now two
	// independent USB HID ports, either of which might currently be
	// the mouse (see the usb_hid_wb instances above, and sw/os/hid.c/
	// sw/apps/wm/wm.c on the software side, which make the same
	// decision for click hit-testing) -- so pick whichever instance's
	// own `typ` currently says "mouse" (2), preferring port 0 if
	// (unusually) both do. An instance that isn't currently a mouse
	// never updates its own curs_x/curs_y (usb_hid_wb only moves them
	// on a report while typ==2 -- see rtl/usb_hid.v), so if neither
	// port is a mouse this just holds whatever port 0 last had
	// (0,0 after reset), same as the single-port behavior before this.
`ifdef USB_HID
	assign gpu_curs_x = (wbs_usb0_typ == 2'd2) ? wbs_usb0_curs_x :
		(wbs_usb1_typ == 2'd2) ? wbs_usb1_curs_x : wbs_usb0_curs_x;
	assign gpu_curs_y = (wbs_usb0_typ == 2'd2) ? wbs_usb0_curs_y :
		(wbs_usb1_typ == 2'd2) ? wbs_usb1_curs_y : wbs_usb0_curs_y;
`endif

	gpu_cursor #() gpu_cursor_i
	(
		.pclk(clk25_2mhz),
		.pixel(gpu_curs_pixel),
		.gpu_x(gpu_x),
		.gpu_y(gpu_y),
		.curs_x(gpu_curs_x),
		.curs_y(gpu_curs_y),
		.curs_alt(socctl_cursor_busy),
	);
`endif

endmodule
