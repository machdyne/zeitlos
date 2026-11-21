/*
 * Zeitlos SOC -- zeitlos32ps test harness
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Runs one compiled test program against zeitlos32ps and reports
 * pass/fail. The programs are zeitlos32's (rtl/cpu/zeitlos32/tests/
 * prog), unchanged, with the same conventions:
 *
 *   store to 0xe000_0000   finished: 0 passed, else the failing check
 *   store to 0xf000_0000   a byte to the console
 *   store to 0xe000_0010   pulse the given irq lines for one cycle
 *   store to 0xe000_0014   hold the given irq lines until written again
 *
 * The memory is a model of ONE rtl/cache_split.v-style port: a request
 * is taken on req && rdy; ack comes LAT cycles after the cycle it was
 * taken in (LAT 0: the next cycle, a cache hit), and rdy is high again
 * in the ack cycle. LAT is fixed, or random per request (RANDLAT:
 * 0..LAT), and with BUSY the port also drops rdy at random. Stores
 * land in memory when acknowledged.
 */

`timescale 1ns / 1ps

module tb_zeitlos32ps;

	parameter MEM_WORDS = 16384;
	parameter LAT       = 0;
	parameter RANDLAT   = 0;
	parameter BUSY      = 0;
	parameter SEED      = 12345;
	parameter TIMEOUT   = 2000000;
	parameter FAST_MUL  = 0;
	parameter ROT       = 0;
	parameter TRACE     = 0;

	reg clk, rst;

	wire        m_req, m_we, m_instr, m_rdy, m_ack;
	wire [31:0] m_adr, m_wdat, m_rdat;
	wire  [3:0] m_sel;
	wire        cpu_trap;

	reg  [31:0] irq_pulse;
	reg  [31:0] irq_level;
	wire [31:0] cpu_irq = irq_pulse | irq_level;

	reg  [31:0] mem [0:MEM_WORDS-1];

	zeitlos32ps #(
		.PROGADDR_RESET(32'h0000_0000),
		.PROGADDR_IRQ(32'h0000_0010),
		.STACKADDR(32'h0001_0000),
		.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1111_1110_1111),
		.ENABLE_MUL(1),
		.ENABLE_DIV(1),
		.FAST_MUL(FAST_MUL),
		.ENABLE_ROT(ROT)
	) cpu (
		.clk(clk), .rst(rst),
		.m_req(m_req), .m_adr(m_adr), .m_we(m_we), .m_sel(m_sel),
		.m_wdat(m_wdat), .m_instr(m_instr),
		.m_rdy(m_rdy), .m_ack(m_ack), .m_rdat(m_rdat),
		.mtu_base(32'h0),
		.irq(cpu_irq), .eoi(), .trap(cpu_trap)
	);

	initial clk = 0;
	always #5 clk = ~clk;

	integer seed, cycles, finished, exitcode, i;
	reg [1023:0] hexfile;

	reg        p;                    // a request is pending
	integer    c;                    // cycles until its ack
	reg [31:0] a, dd;
	reg        w;
	reg  [3:0] s;
	reg        busy;

	assign m_ack  = p && (c == 0);
	assign m_rdy  = !busy && (!p || m_ack);
	assign m_rdat = (a[31:28] == 4'he || a[31:28] == 4'hf) ? 32'd0 :
	                mem[a[31:2] % MEM_WORDS];

	always @(posedge clk) begin
		if (rst) begin
			p <= 1'b0; c <= 0; busy <= 1'b0;
			irq_pulse <= 32'd0; irq_level <= 32'd0;
		end else begin
			irq_pulse <= 32'd0;
			busy <= BUSY && (($random(seed) & 7) == 0);
			if (m_ack && w) begin
				if (a[31:28] == 4'he) begin
					if (a[7:0] == 8'h00) begin
						exitcode = dd;
						finished = 1;
					end else if (a[7:0] == 8'h10) begin
						irq_pulse <= dd;
					end else if (a[7:0] == 8'h14) begin
						irq_level <= dd;
					end
				end else if (a[31:28] == 4'hf) begin
					if (s[0]) $write("%c", dd[7:0]);
				end else begin
					if (s[0]) mem[a[31:2] % MEM_WORDS][7:0]   <= dd[7:0];
					if (s[1]) mem[a[31:2] % MEM_WORDS][15:8]  <= dd[15:8];
					if (s[2]) mem[a[31:2] % MEM_WORDS][23:16] <= dd[23:16];
					if (s[3]) mem[a[31:2] % MEM_WORDS][31:24] <= dd[31:24];
				end
			end
			if (p && c != 0) c <= c - 1;
			if (m_req && m_rdy) begin
				p <= 1'b1;
				a <= m_adr; w <= m_we; s <= m_sel; dd <= m_wdat;
				c <= RANDLAT ? ({$random(seed)} % (LAT + 1)) : LAT;
			end else if (m_ack) begin
				p <= 1'b0;
			end
		end
	end

	always @(posedge clk) begin
		if (!rst && TRACE && cpu.w_wr)
			$display("W %08x x%0d %08x", cpu.w_pc, cpu.w_rd, cpu.w_val);
	end

	initial begin
		seed = SEED;
		finished = 0;
		exitcode = 0;
		cycles = 0;
		rst = 1;
		for (i = 0; i < MEM_WORDS; i = i + 1) mem[i] = 32'd0;
		if (!$value$plusargs("hex=%s", hexfile)) begin
			$display("FAIL: no +hex=<file> given");
			$finish;
		end
		$readmemh(hexfile, mem);
		if ($test$plusargs("vcd")) begin
			$dumpfile("tb_zeitlos32ps.vcd");
			$dumpvars(0, tb_zeitlos32ps);
		end
		repeat (4) @(posedge clk);
		rst = 0;
	end

	always @(posedge clk) begin
		if (!rst) begin
			cycles <= cycles + 1;
			if (finished) begin
				if (exitcode == 0)
					$display("PASS  (%0d cycles, %0d instructions)",
						cycles, cpu.count_instr[31:0]);
				else
					$display("FAIL  check %0d  (%0d cycles, pc in X=%08x)",
						exitcode, cycles, cpu.x_pc);
				$finish;
			end
			if (cpu_trap) begin
				$display("FAIL  cpu trapped, X pc=%08x instr=%08x (%0d cycles)",
					cpu.x_pc, cpu.x_instr, cycles);
				$finish;
			end
			if (cycles > TIMEOUT) begin
				$display("FAIL  timeout after %0d cycles, X pc=%08x valid=%0d",
					cycles, cpu.x_pc, cpu.x_valid);
				$finish;
			end
		end
	end

endmodule
