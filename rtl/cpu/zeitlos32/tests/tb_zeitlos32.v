/*
 * Zeitlos SOC -- zeitlos32 test harness
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Runs one compiled test program against the core and reports
 * pass/fail. Every program in prog/ uses the same three conventions:
 *
 *   store to 0xe000_0000   test finished. Value 0 means every check
 *                          passed; anything else is the id of the
 *                          check that failed.
 *   store to 0xf000_0000   write a byte to the console (this is the
 *                          real 16550 data register address, so a
 *                          test can use the same address the BIOS
 *                          does)
 *   store to 0xe000_0010   pulse the given irq lines for exactly one
 *                          cycle -- this is how rtl/sysctl.v's
 *                          irq_timer behaves, and it is the case
 *                          LATCHED_IRQ exists for
 *   store to 0xe000_0014   HOLD the given irq lines until written
 *                          again -- this is how the 16550's interrupt
 *                          output behaves, and it is why IRQ 4 is the
 *                          one line rtl/sysctl.v does not latch
 *
 * Memory is modelled on rtl/mem/bram.v: a registered ack one cycle
 * after stb, held high for exactly one cycle. MEM_LAT adds further
 * wait states on top of that, which is how the SDRAM and PSRAM boards
 * actually behave -- run the suite at a couple of different latencies
 * and the core's bus handshake gets exercised properly rather than
 * only ever seeing its best case.
 *
 * MEM_RANDLAT makes the wait-state count vary per access, uniformly in
 * 0..MEM_LAT. That is the more realistic model since rtl/arbiter_main.v
 * arrived: the CPU now shares the main bus with the blitter's source
 * reads, so how long an access takes depends on what the blitter is
 * doing at the time. A master that only ever meets a CONSTANT latency
 * can hide a handshake bug -- an off-by-one in a wait state counter,
 * or state that is only correct because every access took the same
 * number of cycles as the last one. Varying it removes that cover.
 *
 * Run:
 *   iverilog -g2005 -o tb tb_zeitlos32.v ../zeitlos32.v ../zeitlos32_muldiv.v
 *   ./tb +hex=prog/t_alu.hex
 *
 * or just `make` in this directory.
 */

`timescale 1ns / 1ps

module tb_zeitlos32;

	parameter MEM_WORDS = 16384;		// 64KB
	parameter MEM_LAT   = 0;			// extra wait states per access
	parameter MEM_RANDLAT = 0;			// if set, vary it in 0..MEM_LAT
	// Model rtl/mem/sram.v instead of rtl/mem/bram.v. That controller's
	// ack state machine does not look at stb or cyc at all -- it
	// free-runs on a 3-cycle loop, so which cycle a request gets acked
	// in depends on the phase between the CPU's access pattern and a
	// counter it has no control over. Obst has no instruction cache,
	// so on that board EVERY fetch goes through this. Worth being
	// certain the handshake survives it.
	parameter MEM_SRAM = 0;
	// Model rtl/csrs.v: a purely COMBINATIONAL ack, asserted in the
	// same cycle as stb (assign wb_ack_o = wb_cyc_i && wb_stb_i). Every
	// other slave in this SOC registers its ack at least one cycle
	// later, so this is the fastest handshake the core will ever meet
	// and the one most likely to be untested. rtl/socctl.v and
	// rtl/mtu.v do the same.
	parameter MEM_COMB = 0;
	// Mix them. On Obst, k_soc_report()'s feature loop alternates
	// between rtl/csrs.v (combinational same-cycle ack) and instruction
	// fetches from rtl/mem/sram.v (free-running 3-cycle ack) on every
	// iteration. Every model above uses ONE slave timing for the whole
	// address space, so the transition between two very different ones
	// -- which is what the real machine does constantly -- was never
	// exercised. Here the MMIO regions ack combinationally and RAM
	// behaves like sram.v.
	parameter MEM_MIXED = 0;
	parameter SEED      = 12345;
	parameter TIMEOUT   = 2000000;
	parameter FAST_MUL  = 0;

	reg clk;
	reg rst;

	wire [31:0] wbm_adr;
	wire [31:0] wbm_dat_o;
	reg  [31:0] wbm_dat_i;
	wire        wbm_we;
	wire  [3:0] wbm_sel;
	wire        wbm_stb;
	reg         wbm_ack;
	wire        wbm_cyc;
	wire        mem_instr;
	wire        cpu_trap;

	reg  [31:0] irq_pulse;
	reg  [31:0] irq_level;
	wire [31:0] cpu_irq = irq_pulse | irq_level;

	reg  [31:0] mem [0:MEM_WORDS-1];

	// rtl/csrs.v drives both its ack AND its read data combinationally,
	// so the model has to do the same or it is testing something the
	// hardware never does.
	wire        is_mmio = (wbm_adr[31:28] == 4'he) || (wbm_adr[31:28] == 4'hf)
	                      || (wbm_adr[31:28] == 4'h7);
	wire        comb_sel = MEM_COMB || (MEM_MIXED && is_mmio);
	wire        comb_ack = wbm_cyc && wbm_stb;
	wire [31:0] comb_dat = mem[wbm_adr[31:2]];

	reg        sram_ack_r;
	reg        sram_pending;

	reg [1023:0] hexfile;
	integer      cycles;
	integer      i;
	integer      lat;
	integer      lat_target;
	integer      seed;
	integer      finished;
	integer      exitcode;
	integer      verbose;

	// ------------------------------------------------------------

	wire        cpu_ack = comb_sel ? comb_ack : wbm_ack;
	wire [31:0] cpu_dat = comb_sel ? comb_dat : wbm_dat_i;

	zeitlos32_wb #(
		.PROGADDR_RESET(32'h0000_0000),
		.PROGADDR_IRQ(32'h0000_0010),
		.STACKADDR(32'h0001_0000),
		// rtl/sysctl.v's value: everything latched except IRQ 4
		.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1111_1110_1111),
		.ENABLE_MUL(1),
		.ENABLE_DIV(1),
		.FAST_MUL(FAST_MUL)
	) cpu (
		.wb_clk_i(clk),
		.wb_rst_i(rst),
		.wbm_adr_o(wbm_adr),
		.mtu_base(32'h0),
		.wbm_dat_o(wbm_dat_o),
		.wbm_dat_i(cpu_dat),
		.wbm_we_o(wbm_we),
		.wbm_sel_o(wbm_sel),
		.wbm_stb_o(wbm_stb),
		.wbm_ack_i(cpu_ack),
		.wbm_cyc_o(wbm_cyc),
		.mem_instr(mem_instr),
		.irq(cpu_irq),
		.eoi(),
		.trap(cpu_trap)
	);

	// ------------------------------------------------------------
	// clock and reset
	// ------------------------------------------------------------

	initial clk = 0;
	always #5 clk = ~clk;			// 100MHz, arbitrary -- this is a
									// functional model, not a timing one

	initial begin
		verbose = 0;
		finished = 0;
		exitcode = 0;
		cycles = 0;
		lat = 0;
		seed = SEED;
		lat_target = MEM_LAT;
		rst = 1;
		irq_pulse = 32'd0;
		irq_level = 32'd0;
		wbm_ack = 0;
		wbm_dat_i = 32'd0;

		for (i = 0; i < MEM_WORDS; i = i + 1) mem[i] = 32'd0;

		if (!$value$plusargs("hex=%s", hexfile)) begin
			$display("FAIL: no +hex=<file> given");
			$finish;
		end
		$readmemh(hexfile, mem);

		if ($test$plusargs("verbose")) verbose = 1;
		if ($test$plusargs("vcd")) begin
			$dumpfile("tb_zeitlos32.vcd");
			$dumpvars(0, tb_zeitlos32);
		end

		repeat (8) @(posedge clk);
		rst = 0;
	end

	// ------------------------------------------------------------
	// wishbone slave: memory plus the three test ports
	//
	// Modelled on rtl/mem/bram.v -- ack is registered and the !ack
	// guard means it is high for exactly one cycle per access.
	// ------------------------------------------------------------

	// rtl/mem/sram.v's free-running ack, reproduced exactly
	always @(posedge clk) begin
		if (rst) begin
			sram_ack_r <= 1'b0;
			sram_pending <= 1'b0;
		end else begin
			if (sram_ack_r) sram_ack_r <= 1'b0;
			else if (sram_pending) begin
				sram_ack_r <= 1'b1;
				sram_pending <= 1'b0;
			end else sram_pending <= 1'b1;
		end
	end

	always @(posedge clk) begin
		if (rst) begin
			wbm_ack <= 0;
			lat <= 0;
			irq_pulse <= 32'd0;
			irq_level <= 32'd0;
		end else begin
			// Pick this access's latency on the cycle the request
			// appears, so it is stable for the whole handshake.
			if (MEM_RANDLAT && wbm_cyc && wbm_stb && !wbm_ack && lat == 0)
				lat_target = {$random(seed)} % (MEM_LAT + 1);
			wbm_ack <= 0;
			irq_pulse <= 32'd0;		// one cycle only
			if (wbm_cyc && wbm_stb && (comb_sel || !wbm_ack)) begin
				if (comb_sel ? 1'b0
				  : (MEM_SRAM || MEM_MIXED) ? !sram_ack_r
				             : (lat < (MEM_RANDLAT ? lat_target : MEM_LAT))) begin
					lat <= lat + 1;
				end else begin
					lat <= 0;
					wbm_ack <= 1;

					if (wbm_adr[31:28] == 4'he) begin

						// ---- test ports ----
						if (wbm_we) begin
							if (wbm_adr[7:0] == 8'h00) begin
								exitcode = wbm_dat_o;
								finished = 1;
							end else if (wbm_adr[7:0] == 8'h10) begin
								irq_pulse <= wbm_dat_o;
							end else if (wbm_adr[7:0] == 8'h14) begin
								irq_level <= wbm_dat_o;
							end
						end
						wbm_dat_i <= 32'd0;

					end else if (wbm_adr[31:28] == 4'hf) begin

						// ---- console ----
						if (wbm_we && wbm_sel[0]) $write("%c", wbm_dat_o[7:0]);
						wbm_dat_i <= 32'd0;

					end else begin

						// ---- ram ----
						if (wbm_we) begin
							if (wbm_sel[0]) mem[wbm_adr[31:2]][7:0]   <= wbm_dat_o[7:0];
							if (wbm_sel[1]) mem[wbm_adr[31:2]][15:8]  <= wbm_dat_o[15:8];
							if (wbm_sel[2]) mem[wbm_adr[31:2]][23:16] <= wbm_dat_o[23:16];
							if (wbm_sel[3]) mem[wbm_adr[31:2]][31:24] <= wbm_dat_o[31:24];
						end else begin
							wbm_dat_i <= mem[wbm_adr[31:2]];
						end

					end
				end
			end
		end
	end

	// ------------------------------------------------------------
	// end conditions
	// ------------------------------------------------------------

	always @(posedge clk) begin
		if (!rst) begin
			cycles <= cycles + 1;

			if (verbose && wbm_ack && mem_instr)
				$display("[%0d] fetch %08x -> %08x", cycles, wbm_adr, wbm_dat_i);

			if (finished) begin
				if (exitcode == 0) begin
					$display("PASS  (%0d cycles, %0d instructions)",
						cycles, cpu.count_instr[31:0]);
				end else begin
					$display("FAIL  check %0d  (%0d cycles, pc=%08x)",
						exitcode, cycles, cpu.pc);
				end
				$finish;
			end

			if (cpu_trap) begin
				$display("FAIL  cpu trapped at pc=%08x instr=%08x (%0d cycles)",
					cpu.pc, cpu.instr, cycles);
				$finish;
			end

			if (cycles > TIMEOUT) begin
				$display("FAIL  timeout after %0d cycles, pc=%08x state=%0d",
					cycles, cpu.pc, cpu.state);
				$finish;
			end
		end
	end

endmodule
