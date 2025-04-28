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

);

	// BOARD LEDS

`ifdef LED_RGB
	assign LED_R = !cpu_trap;
	assign LED_G = ~(|cpu_irq);
`endif

/*
	assign DBG[0] = wbs_uart0_ack_o;
	assign DBG[1] = 0;
	assign DBG[2] = 0;
	assign DBG[3] = 0;
	assign DBG[4] = 0;
	assign DBG[5] = 0;
	assign DBG[6] = 0;
	assign DBG[7] = 0;
*/

/*
	assign DBG[0] = wbs_bram_dat_o[0];
	assign DBG[1] = wbs_bram_dat_o[1];
	assign DBG[2] = wbs_bram_dat_o[2];
	assign DBG[3] = wbs_bram_dat_o[3];
	assign DBG[4] = wbs_bram_dat_o[4];
	assign DBG[5] = wbs_bram_dat_o[5];
	assign DBG[6] = wbs_bram_dat_o[6];
	assign DBG[7] = wbs_bram_dat_o[7];
*/

/*
	assign DBG[0] = wbm_adr_sel_word[0];
	assign DBG[1] = wbm_adr_sel_word[1];
	assign DBG[2] = wbm_adr_sel_word[2];
	assign DBG[3] = wbm_adr_sel_word[3];
	assign DBG[4] = wbm_adr_sel_word[4];
	assign DBG[5] = wbm_adr_sel_word[5];
	assign DBG[6] = wbm_adr_sel_word[6];
	assign DBG[7] = wbm_adr_sel_word[7];
*/

	// CLOCK

/*
	wire clk100mhz;
	wire clk75mhz;
	wire clk50mhz;
	wire clk12mhz;

	reg pll_locked;
	pll0 #() ecp5_pll0 (
		.clkin(CLK_48),
		.clkout0(clk100mhz),
		.clkout1(clk75mhz),
		.clkout2(clk50mhz),
		.clkout3(clk12mhz),
		.locked(pll_locked)
	);
	wire sys_clk = clk12mhz;
*/

	wire pll_locked = 1;
	wire sys_clk = CLK_48;

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
	reg [20:0] rtc_ctr;

	always @(posedge sys_clk) begin
		rtc_ctr <= rtc_ctr + 1;
	end

	// INTERRUPTS

	reg irq_timer;

	always @* begin
		cpu_irq = 0;
		cpu_irq[3] = irq_timer;
	end

	always @(posedge sys_clk) begin
		irq_timer <= 0;
		if (rtc_ctr == 0) irq_timer <= 1;
	end

	// WISHBONE BUS

	wire wbm_clk = sys_clk;
	wire wbm_rst = !sys_rstn;

	wire [27:0] wbm_adr_sel = (wbm_adr & 32'h0fff_ffff);
	wire [25:0] wbm_adr_sel_word = wbm_adr_sel[27:2];

	wire [31:0] wbs_bram_dat_o;
	wire [31:0] wbs_sram_dat_o;
	wire [31:0] wbs_sdram_dat_o;
	wire [31:0] wbs_qqspi_dat_o;
	wire [31:0] wbs_debug_dat_o;
	wire [31:0] wbs_uart0_dat_o;

	wire cs_bram = (wbm_adr < 8192);
`ifdef MEM_SRAM
	wire cs_sram = ((wbm_adr & 32'hf000_0000) == 32'h4000_0000);
`elsif MEM_SDRAM
	wire cs_sdram = ((wbm_adr & 32'hf000_0000) == 32'h4000_0000);
`elsif MEM_QQSPI
	wire cs_qqspi = ((wbm_adr & 32'hf000_0000) == 32'h4000_0000);
`endif
	wire cs_debug = ((wbm_adr & 32'hf000_0000) == 32'he000_0000);
	wire cs_uart0 = ((wbm_adr & 32'hf000_0000) == 32'hf000_0000);

	assign wbm_dat_i =
		cs_bram ? wbs_bram_dat_o :
`ifdef MEM_SRAM
		cs_sram ? wbs_sram_dat_o :
`elsif MEM_SDRAM
		cs_sdram ? wbs_sdram_dat_o :
`elsif MEM_QQSPI
		cs_qqspi ? wbs_qqspi_dat_o :
`endif
		cs_debug ? wbs_debug_dat_o :
		cs_uart0 ? wbs_uart0_dat_o :
		32'hzzzz_zzzz;

	wire wbs_bram_ack_o;
	wire wbs_sram_ack_o;
	wire wbs_sdram_ack_o;
	wire wbs_qqspi_ack_o;
	wire wbs_debug_ack_o;
	wire wbs_uart0_ack_o;

	assign wbm_ack =
		cs_bram ? wbs_bram_ack_o :
`ifdef MEM_SRAM
		cs_sram ? wbs_sram_ack_o :
`elsif MEM_SDRAM
		cs_sdram ? wbs_sdram_ack_o :
`elsif MEM_QQSPI
		cs_qqspi ? wbs_qqspi_ack_o :
`endif
		cs_debug ? wbs_debug_ack_o :
		cs_uart0 ? wbs_uart0_ack_o :
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
      .ENABLE_IRQ_QREGS(1)
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

	// WISHBONE MASTER: DMA CONTROLLER
/*
	wire [31:0] wbm_dma_adr;
	wire [31:0] wbm_dma_dat_o;
	reg [31:0] wbm_dma_dat_i;
	wire [3:0] wbm_dma_sel;
	wire wbm_dma_we;
	wire wbm_dma_stb;
	wire wbm_dma_ack;
	wire wbm_dma_cyc;

	dma #() dma_i
	(
		.wb_clk_i(sys_clk),
		.wb_rst_i(sys_rst),
		.wbm_adr_o(wbm_dma_adr),
		.wbm_dat_o(wbm_dma_dat_o),
		.wbm_dat_i(wbm_dma_dat_i),
		.wbm_we_o(wbm_dma_we),
		.wbm_sel_o(wbm_dma_sel),
		.wbm_stb_o(wbm_dma_stb),
		.wbm_ack_i(wbm_dma_ack),
		.wbm_cyc_o(wbm_dma_cyc),
	);
*/

	// WISHBONE ARBITER (always CPU for now)

	wire [31:0] wbm_adr;
	wire [31:0] wbm_dat_o;
	wire [31:0] wbm_dat_i;
	wire [3:0] wbm_sel;
	wire wbm_we;
	wire wbm_stb;
	wire wbm_ack;
	wire wbm_cyc;

	assign wbm_adr = wbm_cpu_adr;
	assign wbm_dat_o = wbm_cpu_dat_o;
	assign wbm_dat_i = wbm_cpu_dat_i;
	assign wbm_sel = wbm_cpu_sel;
	assign wbm_we = wbm_cpu_we;
	assign wbm_stb = wbm_cpu_stb;
	assign wbm_ack = wbm_cpu_ack;
	assign wbm_cyc = wbm_cpu_cyc;

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

	// WISHBONE SLAVE: LED DEBUG INTERFACE

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

	// WISHBONE SLAVE: UART0

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

endmodule
