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

   output UART0_TX,
   input UART0_RX,

`ifdef LED_DEBUG
	output [7:0] DBG,
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
   input ETH_REFCLK,
   input [1:0] ETH_RXD,
   output [1:0] ETH_TXD,
   output ETH_TX_EN,
   input ETH_CRS_DV,
   output ETH_RST_N,
`endif

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

	// RTC
	reg [15:0] rtc_ctr;	// ~732hz

	always @(posedge sys_clk) begin
		rtc_ctr <= rtc_ctr + 1;
	end

	// INTERRUPTS

	reg irq_timer;

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
	wire marb_master;

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
	wire [31:0] wbs_debug_dat_o;
	wire [31:0] wbs_uart0_dat_o;
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
`ifdef ICACHE
	wire [31:0] wbs_cache_dat_o;
`endif

	wire cs_bram = (wbm_adr < 8192);
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
	wire cs_socctl = ((wbm_adr & 32'hf000_0300) == 32'h7000_0200);
	wire wbm_cyc_socctl = cs_socctl && wbm_cyc;
`ifdef ICACHE
	wire cs_cache = ((wbm_adr & 32'hf000_0300) == 32'h7000_0100);
	wire wbm_cyc_cache = cs_cache && wbm_cyc;
`endif
`ifdef RTC
	wire cs_rtc = ((wbm_adr & 32'hf000_0300) == 32'h7000_0300);
	wire wbm_cyc_rtc = cs_rtc && wbm_cyc;
`endif
	wire cs_csrs = ((wbm_adr & 32'hf000_0000) == 32'h7000_0000)
		&& !cs_socctl
`ifdef ICACHE
		&& !cs_cache
`endif
`ifdef RTC
		&& !cs_rtc
`endif
		;
`ifdef DEBUG
	wire cs_debug = ((wbm_adr & 32'hf000_0000) == 32'he000_0000);
`endif
`ifdef UART0
	wire cs_uart0 = ((wbm_adr & 32'hf000_0000) == 32'hf000_0000);
`endif

	assign wbm_dat_i =
		cs_bram ? wbs_bram_dat_o :
		cs_mtu ? wbs_mtu_dat_o :
`ifdef MEM_SRAM
		cs_sram ? wbs_sram_dat_o :
`elsif MEM_SDRAM
		cs_sdram ? wbs_sdram_dat_o :
`elsif MEM_QQSPI
		cs_qqspi ? wbs_qqspi_dat_o :
`endif
`ifdef MEM_VRAM
`ifdef GPU_RASTER
		cs_vram ? wbm_cpu_arb0_dat_i :
`else
		cs_vram ? wbs_vram_dat_o :
`endif
`endif
`ifdef MEM_ROM
		cs_rom ? wbs_rom_dat_o :
`endif
`ifdef DEBUG
		cs_debug ? wbs_debug_dat_o :
`endif
`ifdef UART0
		cs_uart0 ? wbs_uart0_dat_o :
`endif
`ifdef SPI_SDCARD
		cs_spisdcard ? wbs_spisdcard_dat_o :
`endif
`ifdef USB_HID
		cs_usb0 ? wbs_usb0_dat_o :
		cs_usb1 ? wbs_usb1_dat_o :
`endif
`ifdef GPU_RASTER
		cs_gpu ? wbs_gpu_dat_o :
`endif
`ifdef GPU_BLIT
		cs_gpu_blit ? wbs_gpu_blit_dat_o :
`endif
`ifdef MEM_GLYPH
		cs_glyph ? wbs_glyph_dat_o :
`endif
`ifdef SPI_ETH
		cs_spieth ? wbs_spieth_dat_o :
`endif
`ifdef ETH_RMII
		cs_ethmac ? wbs_ethmac_dat_o :
`endif
		cs_socctl ? wbs_socctl_dat_o :
`ifdef RTC
		cs_rtc ? wbs_rtc_dat_o :
`endif
		cs_csrs ? wbs_csrs_dat_o :
`ifdef ICACHE
		cs_cache ? wbs_cache_dat_o :
`endif
		32'hzzzz_zzzz;

	wire wbs_bram_ack_o;
	wire wbs_mtu_ack_o;
	wire wbs_sram_ack_o;
	wire wbs_sdram_ack_o;
	wire wbs_qqspi_ack_o;
	wire wbs_vram_ack_o;
	wire wbs_rom_ack_o;
	wire wbs_debug_ack_o;
	wire wbs_uart0_ack_o;
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
`ifdef ICACHE
	wire wbs_cache_ack_o;
`endif

	assign wbm_ack =
		cs_bram ? wbs_bram_ack_o :
		cs_mtu ? wbs_mtu_ack_o :
`ifdef MEM_SRAM
		cs_sram ? wbs_sram_ack_o :
`elsif MEM_SDRAM
		cs_sdram ? wbs_sdram_ack_o :
`elsif MEM_QQSPI
		cs_qqspi ? wbs_qqspi_ack_o :
`endif
`ifdef MEM_VRAM
`ifdef GPU_RASTER
		cs_vram ? wbm_cpu_arb0_ack :
`else
		cs_vram ? wbs_vram_ack_o :
`endif
`endif
`ifdef MEM_ROM
		cs_rom ? wbs_rom_ack_o :
`endif
`ifdef DEBUG
		cs_debug ? wbs_debug_ack_o :
`endif
`ifdef UART0
		cs_uart0 ? wbs_uart0_ack_o :
`endif
`ifdef SPI_SDCARD
		cs_spisdcard ? wbs_spisdcard_ack_o :
`endif
`ifdef USB_HID
		cs_usb0 ? wbs_usb0_ack_o :
		cs_usb1 ? wbs_usb1_ack_o :
`endif
`ifdef GPU_RASTER
		cs_gpu ? wbs_gpu_ack_o :
`endif
`ifdef GPU_BLIT
		cs_gpu_blit ? wbs_gpu_blit_ack_o :
`endif
`ifdef MEM_GLYPH
		cs_glyph ? wbs_glyph_ack_o :
`endif
`ifdef SPI_ETH
		cs_spieth ? wbs_spieth_ack_o :
`endif
`ifdef ETH_RMII
		cs_ethmac ? wbs_ethmac_ack_o :
`endif
		cs_socctl ? wbs_socctl_ack_o :
`ifdef RTC
		cs_rtc ? wbs_rtc_ack_o :
`endif
		cs_csrs ? wbs_csrs_ack_o :
`ifdef ICACHE
		cs_cache ? wbs_cache_ack_o :
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
		.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1111_1110_1111)
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
		.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1111_1110_1111)
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

	wb_mtu mtu_i (
		.clk_i(wbm_clk),
		.rst_i(wbm_rst),
		.addr_in(wbm_cpu_adr),
		.addr_out(wbm_cpu_padr),
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

	wb_icache #(
		.CACHE_KB(`ICACHE_KB),
		.LINE_WORDS(`ICACHE_LINE_WORDS)
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

		// control/status registers (0x7000_0100, see cs_cache above)
		.cfg_adr_i({ 30'b0, wbm_adr_sel_word[1:0] }),
		.cfg_dat_i(wbm_dat_o),
		.cfg_dat_o(wbs_cache_dat_o),
		.cfg_we_i(wbm_we),
		.cfg_stb_i(wbm_stb),
		.cfg_cyc_i(wbm_cyc_cache),
		.cfg_ack_o(wbs_cache_ack_o)
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
	assign marb_master = 1'b0;

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

	// WISHBONE SLAVE: LED DEBUG INTERFACE
`ifdef DEBUG
	wire wbm_cyc_debug = cs_debug && wbm_cyc;

	debug_wb #() wbs_debug0_i
	(
		.wb_clk_i(wbm_clk),
		.wb_rst_i(wbm_rst),
		.wb_adr_i(wbm_adr_sel_word),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_debug_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_debug_ack_o),
		.wb_cyc_i(wbm_cyc_debug),
		.led(LED_B),


`ifdef LED_DEBUG
		.leds(DBG)
`endif

	);
`endif

	// WISHBONE SLAVE: UART0
`ifdef UART0
	reg wbs_uart0_int;
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

	// WISHBONE SLAVE: SPI BIT-BANG INTERFACE FOR ETH (ENC28J60)
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
`endif

	// WISHBONE SLAVE: RMII ETHERNET MAC (LAN8720A, mozart_ml1 only)
`ifdef ETH_RMII
	wire wbm_cyc_ethmac = cs_ethmac && wbm_cyc;

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
		.eth_refclk(ETH_REFCLK),
		.eth_rxd(ETH_RXD),
		.eth_txd(ETH_TXD),
		.eth_tx_en(ETH_TX_EN),
		.eth_crs_dv(ETH_CRS_DV),
		.eth_rst_n(ETH_RST_N)
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

	socctl_wb #(
		.VIDEO_MODE_RESET(VIDEO_MODE_DEFAULT)
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
		.wb_adr_i({ 30'b0, wbm_adr_sel_word[1:0] }),
		.wb_dat_i(wbm_dat_o),
		.wb_dat_o(wbs_socctl_dat_o),
		.wb_we_i(wbm_we),
		.wb_sel_i(wbm_sel),
		.wb_stb_i(wbm_stb),
		.wb_ack_o(wbs_socctl_ack_o),
		.wb_cyc_i(wbm_cyc_socctl),
		.cursor_busy(socctl_cursor_busy),
		.video_mode(socctl_video_mode)
	);

	csrs_wb #(
		.MEM_MB(`MEM),
		.FEATURES(CSR_FEATURES)
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

	// WISHBONE SLAVE: USB HID
`ifdef USB_HID
	reg wbs_usb0_int;
	wire wbm_cyc_usb0 = cs_usb0 && wbm_cyc;

	wire [1:0] wbs_usb0_typ;
	wire [9:0] wbs_usb0_curs_x, wbs_usb0_curs_y;

	usb_hid_wb #() wbs_usb0_i
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

	usb_hid_wb #() wbs_usb1_i
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

	gpu_video #() gpu_video_i
	(
		.clk(wbm_clk),
		.pclk(clk25_2mhz),
		.bclk(clk126mhz),
		.resetn(~wbm_rst),
		.pixel(gpu_pixel),
		.video_mode(socctl_video_mode),
		.x(gpu_x),
		.y(gpu_y),
		.gb_adr_o(gb_adr),
		.gb_dat_i(gb_dat),
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
