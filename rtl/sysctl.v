/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * System Controller (top)
 *
 */

`include "boards.vh"

localparam SYSCLK = 48_000_000;

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
	output QQSPI_CS1, QQSPI_CS0, QQSPI_SS,
	output QQSPI_SCK,
	inout QQSPI_MOSI, QQSPI_MISO, QQSPI_SIO2, QQSPI_SIO3,
`endif

`ifdef SPI_SDCARD
   output SD_SS,
   inout SD_MISO,
   output SD_MOSI,
   output SD_SCK,
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

	wire clk125mhz;
	wire clk100mhz;
	wire clk75mhz;
	wire clk50mhz;
	wire clk48mhz = CLK_48;
	wire clk12mhz;

	wire sys_clk = clk48mhz;

`ifdef ECP5

	wire pll_locked = pll0_locked && pll1_locked;
	wire pll0_locked;
	wire pll1_locked;

	pll0 #() pll0_i (
		.clkin(clk48mhz),
		.clkout0(clk100mhz),
		.clkout1(clk75mhz),
		.clkout2(clk50mhz),
		.clkout3(clk12mhz),
		.locked(pll0_locked)
	);

	pll1 #() pll1_i (
		.clkin(clk48mhz),
		.clkout0(clk125mhz),
		.locked(pll1_locked)
	);

`elsif GATEMATE

	wire pll_locked = pll0_locked && pll1_locked && pll2_locked;
	wire pll0_locked;
	wire pll1_locked;
	wire pll2_locked;

   CC_PLL #(
      .REF_CLK(48.0),      // reference input in MHz
      .OUT_CLK(125.0),     // pll output frequency in MHz
      .PERF_MD("ECONOMY"), // LOWPOWER, ECONOMY, SPEED
      .LOW_JITTER(1),      // 0: disable, 1: enable low jitter mode
      .CI_FILTER_CONST(2), // optional CI filter constant
      .CP_FILTER_CONST(4)  // optional CP filter constant
   ) pll_inst0 (
      .CLK_REF(CLK_48), .CLK_FEEDBACK(1'b0), .USR_CLK_REF(1'b0),
      .USR_LOCKED_STDY_RST(1'b0),
		.USR_PLL_LOCKED(pll0_locked),
      .CLK0(clk125mhz),
   );

   CC_PLL #(
      .REF_CLK(48.0),      // reference input in MHz
      .OUT_CLK(75.0),      // pll output frequency in MHz
      .PERF_MD("ECONOMY"), // LOWPOWER, ECONOMY, SPEED
      .LOW_JITTER(1),      // 0: disable, 1: enable low jitter mode
      .CI_FILTER_CONST(2), // optional CI filter constant
      .CP_FILTER_CONST(4)  // optional CP filter constant
   ) pll_inst1 (
      .CLK_REF(CLK_48), .CLK_FEEDBACK(1'b0), .USR_CLK_REF(1'b0),
      .USR_LOCKED_STDY_RST(1'b0),
		.USR_PLL_LOCKED(pll1_locked),
      .CLK0(clk75mhz),
   );

   CC_PLL #(
      .REF_CLK(48.0),      // reference input in MHz
      .OUT_CLK(12.0),      // pll output frequency in MHz
      .PERF_MD("ECONOMY"), // LOWPOWER, ECONOMY, SPEED
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
	wire [31:0] wbs_gpu_dat_o;
	wire [31:0] wbs_gpu_blit_dat_o;

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
	wire cs_usb0 = ((wbm_adr & 32'hf000_0000) == 32'hc000_0000);
`endif
`ifdef GPU_RASTER
	wire cs_gpu = ((wbm_adr & 32'hf000_0000) == 32'ha000_0000);
`endif
`ifdef GPU_BLIT
	wire cs_gpu_blit = ((wbm_adr & 32'hf000_0000) == 32'hd000_0000);
`endif
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
`endif
`ifdef GPU_RASTER
		cs_gpu ? wbs_gpu_dat_o :
`endif
`ifdef GPU_BLIT
		cs_gpu_blit ? wbs_gpu_blit_dat_o :
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
	wire wbs_gpu_ack_o;
	wire wbs_gpu_blit_ack_o;

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
`endif
`ifdef GPU_RASTER
		cs_gpu ? wbs_gpu_ack_o :
`endif
`ifdef GPU_BLIT
		cs_gpu_blit ? wbs_gpu_blit_ack_o :
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

	picorv32_wb #(
      .STACKADDR(BRAM_WORDS * 4),      // end of BRAM
      .PROGADDR_RESET(32'h0000_0000),
      .PROGADDR_IRQ(32'h0000_0010),
      .BARREL_SHIFTER(1),
      .COMPRESSED_ISA(0),
      .ENABLE_MUL(0),
      .ENABLE_DIV(0),
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
		.irq(cpu_irq)
	);

	// WISHBONE SLAVE: MTU (Memory Translation Unit)
	wire wbm_cyc_mtu = cs_mtu && wbm_cyc;

	wb_mtu mtu_i (
		.clk_i(wbm_clk),
		.rst_i(wbm_rst),
		.addr_in(wbm_cpu_adr),
		.addr_out(wbm_adr),
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
	//assign wbm_adr = wbm_cpu_adr;
	assign wbm_dat_o = wbm_cpu_dat_o;
	assign wbm_cpu_dat_i = wbm_dat_i;
	assign wbm_sel = wbm_cpu_sel;
	assign wbm_we = wbm_cpu_we;
	assign wbm_stb = wbm_cpu_stb;
	assign wbm_cpu_ack = wbm_ack;
	assign wbm_cyc = wbm_cpu_cyc;

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

//		.busy(blit_busy)
	);

`endif

`ifdef ARBITER

	// WISHBONE ARBITER
	wire [31:0] wbm_cpu_arb0_dat_i;
	wire wbm_cpu_arb0_ack;

	wire wbm_cpu_arb0_cyc = cs_vram && wbm_cpu_cyc;

   wb_arbiter #() arb0_i
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
   	.cs({QQSPI_CS1, QQSPI_CS0}),
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

	// WISHBONE SLAVE: SPI BIT-BANG INTERFACE FOR SDCARD
`ifdef SPI_SDCARD
	wire wbm_cyc_spisdcard = cs_spisdcard && wbm_cyc;

	spibb_wb #() wbs_spibb0_i
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
		.sd_ss(SD_SS),
		.sd_miso(SD_MISO),
		.sd_mosi(SD_MOSI),
		.sd_sck(SD_SCK)
	);
`endif

	// WISHBONE SLAVE: USB HID
`ifdef USB_HID
	reg wbs_usb0_int;
	wire wbm_cyc_usb0 = cs_usb0 && wbm_cyc;

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
		.curs_x(gpu_curs_x),
		.curs_y(gpu_curs_y),
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
		.pclk(clk75mhz),
		.bclk(clk125mhz),
		.resetn(~wbm_rst),
		.pixel(gpu_pixel),
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

	gpu_cursor #() gpu_cursor_i
	(
		.pclk(clk75mhz),
		.pixel(gpu_curs_pixel),
		.gpu_x(gpu_x),
		.gpu_y(gpu_y),
		.curs_x(gpu_curs_x),
		.curs_y(gpu_curs_y),
	);
`endif

endmodule
