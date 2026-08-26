/*
 * zeitlos32 -- boot the REAL BIOS against the REAL 16550.
 *
 * Not a replacement for rtl/tb/tb_soc.v; a cut-down version of it
 * with only the parts the BIOS touches before it tries to reach
 * flash: BRAM at 0x00000000 preloaded from sw/bios/bios.hex, the
 * opencores UART at 0xf0000000, and rtl/sysctl.v's RTC tick on
 * cpu_irq[3].
 *
 * The point is the interrupt ABI. The directed tests in prog/ check
 * it against a handler this repository wrote for the purpose;
 * sw/bios/boot_picorv32.S is the handler that actually has to work,
 * and it does things the tests do not -- 32 stores and 32 loads
 * across a q-register save, a C call, and a retirq through a q0 that
 * was round-tripped through memory. Any disagreement about the ABI
 * shows up here and nowhere else.
 *
 * The UART's interrupt line is wired to cpu_irq[4] exactly as
 * rtl/sysctl.v wires it -- level, and the one bit LATCHED_IRQ leaves
 * out.
 */

`timescale 1ns / 1ps

module tb_bios;

	parameter TIMEOUT = 600000;

	reg clk = 0;
	reg rst = 1;

	always #5 clk = ~clk;

	wire [31:0] adr, dat_o, sel_dummy;
	reg  [31:0] dat_i;
	wire [3:0]  sel;
	wire        we, stb, cyc, mem_instr, cpu_trap;
	reg         ack;
	reg  [31:0] cpu_irq;

	// ------------------------------------------------------------

	zeitlos32_wb #(
		.PROGADDR_RESET(32'h0000_0000),
		.PROGADDR_IRQ(32'h0000_0010),
		.STACKADDR(32'h0000_2000),
		.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1111_1110_1111),
		.ENABLE_MUL(1),
		.ENABLE_DIV(1),
		.FAST_MUL(1)
	) cpu (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.wbm_adr_o(adr), .wbm_dat_o(dat_o), .wbm_dat_i(dat_i),
		.wbm_we_o(we), .wbm_sel_o(sel), .wbm_stb_o(stb),
		.wbm_ack_i(ack), .wbm_cyc_o(cyc),
		.mem_instr(mem_instr), .irq(cpu_irq), .eoi(), .trap(cpu_trap)
	);

	// ------------------------------------------------------------
	// address decode, same shape as rtl/sysctl.v
	// ------------------------------------------------------------

	wire cs_uart = ((adr & 32'hf000_0000) == 32'hf000_0000);
	wire cs_bram = (adr[31:28] == 4'h0);
	wire cs_main = (adr[31:28] == 4'h4);		// main memory (no flash here)

	// Everything else -- LEDs, MTU, CSRs, VRAM, GPU, flash -- is
	// acked and reads as zero. This model is about the CPU and the
	// UART; the rest of the map just has to not hang the bus, which
	// on real hardware it does not because those slaves exist.
	wire cs_stub = !(cs_uart || cs_bram || cs_main);

	wire [25:0] adr_word = adr[27:2];

	// ------------------------------------------------------------
	// BRAM: the BIOS
	// ------------------------------------------------------------

	reg [31:0] bram [0:2047];
	reg [31:0] bram_do;
	reg        bram_ack;

	always @(posedge clk) begin
		bram_ack <= 0;
		if (cs_bram && cyc && stb && !bram_ack) begin
			if (we) begin
				if (sel[0]) bram[adr_word[10:0]][7:0]   <= dat_o[7:0];
				if (sel[1]) bram[adr_word[10:0]][15:8]  <= dat_o[15:8];
				if (sel[2]) bram[adr_word[10:0]][23:16] <= dat_o[23:16];
				if (sel[3]) bram[adr_word[10:0]][31:24] <= dat_o[31:24];
			end else
				bram_do <= bram[adr_word[10:0]];
			bram_ack <= 1;
		end
	end

	// ------------------------------------------------------------
	// main memory, so load_zeitlos()'s copy loop has a destination
	// ------------------------------------------------------------

	reg [31:0] main [0:65535];
	reg [31:0] main_do;
	reg        main_ack;

	always @(posedge clk) begin
		main_ack <= 0;
		if (cs_main && cyc && stb && !main_ack) begin
			if (we) begin
				if (sel[0]) main[adr_word[15:0]][7:0]   <= dat_o[7:0];
				if (sel[1]) main[adr_word[15:0]][15:8]  <= dat_o[15:8];
				if (sel[2]) main[adr_word[15:0]][23:16] <= dat_o[23:16];
				if (sel[3]) main[adr_word[15:0]][31:24] <= dat_o[31:24];
			end else
				main_do <= main[adr_word[15:0]];
			main_ack <= 1;
		end
	end

	// ------------------------------------------------------------
	// the real 16550
	// ------------------------------------------------------------

	wire [31:0] uart_do;
	wire        uart_ack;
	wire        uart_int;
	wire        uart_tx;

	uart_top uart_i (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.wb_adr_i(adr_word),
		.wb_dat_i(dat_o), .wb_dat_o(uart_do),
		.wb_we_i(we), .wb_sel_i(sel),
		.wb_stb_i(cs_uart && stb), .wb_cyc_i(cs_uart && cyc),
		.wb_ack_o(uart_ack),
		.stx_pad_o(uart_tx), .srx_pad_i(1'b1),
		.cts_pad_i(1'b1), .dsr_pad_i(1'b1),
		.ri_pad_i(1'b1), .dcd_pad_i(1'b1),
		.int_o(uart_int)
	);

	reg stub_ack;
	always @(posedge clk) begin
		stub_ack <= 0;
		if (cs_stub && cyc && stb && !stub_ack) stub_ack <= 1;
	end

	always @* begin
		dat_i = cs_uart ? uart_do : cs_main ? main_do :
		        cs_bram ? bram_do : 32'd0;
		ack   = cs_uart ? uart_ack : cs_main ? main_ack :
		        cs_bram ? bram_ack : stub_ack;
	end

	// ------------------------------------------------------------
	// interrupts, as rtl/sysctl.v wires them
	// ------------------------------------------------------------

	reg [15:0] rtc_ctr;

	always @(posedge clk) begin
		cpu_irq <= 32'd0;
		rtc_ctr <= rtc_ctr + 1;
		if (rtc_ctr[9:0] == 0) cpu_irq[3] <= 1'b1;	// one-cycle tick
		cpu_irq[4] <= uart_int;					// level
	end

	// ------------------------------------------------------------
	// decode the serial line so we see what the BIOS actually says
	// ------------------------------------------------------------

	integer bittime;
	integer i;
	reg [7:0] rxb;

	initial begin
		// BIOS sets divisor 3 at 48MHz -> 1 Mbaud. This model runs at
		// 100MHz, so the effective bit time is what the UART's own
		// divisor produces: 16 clocks per baud tick * 3.
		bittime = 16 * 3 * 10;		// ns per bit at 10ns/clk
		forever begin
			@(negedge uart_tx);
			#(bittime * 1.5);
			for (i = 0; i < 8; i = i + 1) begin
				rxb[i] = uart_tx;
				#bittime;
			end
			$write("%c", rxb);
			$fflush;
		end
	end

	// ------------------------------------------------------------

	integer cycles;

	initial begin
		cycles = 0;
		rtc_ctr = 0;
		cpu_irq = 0;
		for (i = 0; i < 2048; i = i + 1) bram[i] = 0;
		for (i = 0; i < 65536; i = i + 1) main[i] = 0;
		$readmemh("../../../../sw/bios/bios.hex", bram);
		if ($test$plusargs("vcd")) begin
			$dumpfile("tb_bios.vcd");
			$dumpvars(0, tb_bios);
		end
		repeat (8) @(posedge clk);
		rst = 0;
	end

	always @(posedge clk) if (!rst) begin
		cycles <= cycles + 1;
		if (cpu_trap) begin
			$display("\n*** TRAP at pc=%08x instr=%08x after %0d cycles",
				cpu.pc, cpu.instr, cycles);
			$finish;
		end
		if (cycles % 50000 == 0)
			$display("[%0d] pc=%08x state=%0d pending=%08x mask=%08x active=%b",
				cycles, cpu.pc, cpu.state, cpu.irq_pending, cpu.irq_mask,
				cpu.irq_active);
		if (cycles > TIMEOUT) begin
			$display("\n*** timeout after %0d cycles, pc=%08x state=%0d irq_pending=%08x irq_mask=%08x irq_active=%b",
				cycles, cpu.pc, cpu.state, cpu.irq_pending, cpu.irq_mask,
				cpu.irq_active);
			$finish;
		end
	end

endmodule
