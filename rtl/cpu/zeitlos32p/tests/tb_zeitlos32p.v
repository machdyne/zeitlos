/*
 * Zeitlos SOC -- zeitlos32p test harness
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Runs one compiled test program against zeitlos32p and reports
 * pass/fail. The programs are zeitlos32's (rtl/cpu/zeitlos32/tests/
 * prog), unchanged, with the same conventions:
 *
 *   store to 0xe000_0000   finished: 0 passed, else the failing check
 *   store to 0xf000_0000   a byte to the console
 *   store to 0xe000_0010   pulse the given irq lines for one cycle
 *   store to 0xe000_0014   hold the given irq lines until written again
 *
 * The memory is a model of rtl/cache_split.v's two ports, not a
 * Wishbone bus: a request is taken on req && rdy; ack comes LAT cycles
 * after the cycle it was taken in (LAT 0: the next cycle, a cache hit),
 * and rdy is high again in the ack cycle. Each port has its own LAT,
 * fixed (I_LAT, D_LAT) or random per request (RANDLAT: 0..I_LAT,
 * 0..D_LAT), and with BUSY set each port also drops rdy at random for
 * no reason at all, as the cache does while it flushes or applies a
 * snoop. Stores land in memory when acknowledged.
 *
 * TRACE=1 prints every register write (pc, rd, value) as W makes it;
 * the lockstep bench (tb_lockstep.v) compares those with zeitlos32's.
 */

`timescale 1ns / 1ps

module tb_zeitlos32p;

	parameter MEM_WORDS = 16384;
	parameter I_LAT     = 0;
	parameter D_LAT     = 0;
	parameter RANDLAT   = 0;
	parameter BUSY      = 0;
	parameter SEED      = 12345;
	parameter TIMEOUT   = 2000000;
	parameter FAST_MUL  = 0;
	parameter TRACE     = 0;
	parameter BTB_BITS  = 6;

	reg clk, rst;

	wire        i_req;
	wire [31:0] i_adr;
	wire        i_rdy;
	wire        i_ack;
	wire [31:0] i_dat;
	wire        d_req;
	wire [31:0] d_adr;
	wire        d_we;
	wire  [3:0] d_sel;
	wire [31:0] d_wdat;
	wire        d_rdy;
	wire        d_ack;
	wire [31:0] d_rdat;
	wire        cpu_trap;

	reg  [31:0] irq_pulse;
	reg  [31:0] irq_level;
	wire [31:0] cpu_irq = irq_pulse | irq_level;

	reg  [31:0] mem [0:MEM_WORDS-1];

	zeitlos32p #(
		.PROGADDR_RESET(32'h0000_0000),
		.PROGADDR_IRQ(32'h0000_0010),
		.STACKADDR(32'h0001_0000),
		.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1111_1110_1111),
		.BTB_BITS(BTB_BITS),
		.ENABLE_MUL(1),
		.ENABLE_DIV(1),
		.FAST_MUL(FAST_MUL)
	) cpu (
		.clk(clk), .rst(rst),
		.i_req(i_req), .i_adr(i_adr), .i_rdy(i_rdy), .i_ack(i_ack), .i_dat(i_dat),
		.d_req(d_req), .d_adr(d_adr), .d_we(d_we), .d_sel(d_sel), .d_wdat(d_wdat),
		.d_rdy(d_rdy), .d_ack(d_ack), .d_rdat(d_rdat),
		.mtu_base(32'h0),
		.irq(cpu_irq), .eoi(), .trap(cpu_trap)
	);

	initial clk = 0;
	always #5 clk = ~clk;

	integer seed, cycles, finished, exitcode, i;
	reg [1023:0] hexfile;

	// -- the two ports ---------------------------------------------

	reg        ip, dp;               // a request is pending
	integer    ic, dc;               // cycles until its ack
	reg [31:0] ia, da;               // its address
	reg        dw;                   // ...is a store
	reg  [3:0] ds;
	reg [31:0] dd;
	reg        ibusy, dbusy;

	assign i_ack = ip && (ic == 0);
	assign d_ack = dp && (dc == 0);
	assign i_rdy = !ibusy && (!ip || i_ack);
	assign d_rdy = !dbusy && (!dp || d_ack);
	assign i_dat = mem[ia[31:2] % MEM_WORDS];
	assign d_rdat = (da[31:28] == 4'he || da[31:28] == 4'hf) ? 32'd0 :
	                mem[da[31:2] % MEM_WORDS];

	always @(posedge clk) begin
		if (rst) begin
			ip <= 1'b0; dp <= 1'b0; ic <= 0; dc <= 0;
			ibusy <= 1'b0; dbusy <= 1'b0;
			irq_pulse <= 32'd0; irq_level <= 32'd0;
		end else begin
			irq_pulse <= 32'd0;
			ibusy <= BUSY && (($random(seed) & 7) == 0);
			dbusy <= BUSY && (($random(seed) & 7) == 0);

			// I
			if (ip && ic != 0) ic <= ic - 1;
			if (i_req && i_rdy) begin
				ip <= 1'b1;
				ia <= i_adr;
				ic <= RANDLAT ? ({$random(seed)} % (I_LAT + 1)) : I_LAT;
			end else if (i_ack) begin
				ip <= 1'b0;
			end

			// D: a store lands when it is acknowledged
			if (d_ack && dw) begin
				if (da[31:28] == 4'he) begin
					if (da[7:0] == 8'h00) begin
						exitcode = dd;
						finished = 1;
					end else if (da[7:0] == 8'h10) begin
						irq_pulse <= dd;
					end else if (da[7:0] == 8'h14) begin
						irq_level <= dd;
					end
				end else if (da[31:28] == 4'hf) begin
					if (ds[0]) $write("%c", dd[7:0]);
				end else begin
					if (ds[0]) mem[da[31:2] % MEM_WORDS][7:0]   <= dd[7:0];
					if (ds[1]) mem[da[31:2] % MEM_WORDS][15:8]  <= dd[15:8];
					if (ds[2]) mem[da[31:2] % MEM_WORDS][23:16] <= dd[23:16];
					if (ds[3]) mem[da[31:2] % MEM_WORDS][31:24] <= dd[31:24];
				end
			end
			if (dp && dc != 0) dc <= dc - 1;
			if (d_req && d_rdy) begin
				dp <= 1'b1;
				da <= d_adr; dw <= d_we; ds <= d_sel; dd <= d_wdat;
				dc <= RANDLAT ? ({$random(seed)} % (D_LAT + 1)) : D_LAT;
			end else if (d_ack) begin
				dp <= 1'b0;
			end
		end
	end

	// -- trace -------------------------------------------------------

	always @(posedge clk) begin
		if (!rst && TRACE && cpu.w_valid && cpu.w_we && cpu.w_rd != 0)
			$display("W %08x x%0d %08x", cpu.w_pc, cpu.w_rd, cpu.w_val);
	end

	// -- coverage: the memory stage's less common paths --------------

	// and the rule they rest on: every answer is for M's access
	integer cv_s, cv_issm, cv_iss_s, cv_xnt;
	initial begin cv_s = 0; cv_issm = 0; cv_iss_s = 0; cv_xnt = 0; end
	always @(posedge clk) if (!rst) begin
		if (cpu.x_taken && !cpu.x_adv) begin
			$display("FAIL  X's own D request taken but X stays (pc in X=%08x)", cpu.ex_pc);
			$finish;
		end
		if (d_ack && !(cpu.m_valid && cpu.m_dop && cpu.m_iss)) begin
			$display("FAIL  D answer with no access outstanding in M (pc in X=%08x)", cpu.ex_pc);
			$finish;
		end
		if (cpu.s_valid) cv_s = cv_s + 1;
		if (cpu.iss_m && d_rdy) cv_issm = cv_issm + 1;
		if (cpu.iss_s && d_rdy) cv_iss_s = cv_iss_s + 1;
		if (cpu.iss_x && !d_rdy) cv_xnt = cv_xnt + 1;
	end

	// -- run ---------------------------------------------------------

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
			$dumpfile("tb_zeitlos32p.vcd");
			$dumpvars(0, tb_zeitlos32p);
		end
		repeat (4) @(posedge clk);
		rst = 0;
	end

	always @(posedge clk) begin
		if (!rst) begin
			cycles <= cycles + 1;
			if (finished) begin
				if ($test$plusargs("cov"))
					$display("cov   S busy %0d, sent from M %0d, from S %0d, X not taken %0d",
						cv_s, cv_issm, cv_iss_s, cv_xnt);
				if (exitcode == 0)
					$display("PASS  (%0d cycles, %0d instructions)",
						cycles, cpu.count_instr[31:0]);
				else
					$display("FAIL  check %0d  (%0d cycles, pc in X=%08x)",
						exitcode, cycles, cpu.ex_pc);
				$finish;
			end
			if (cpu_trap) begin
				$display("FAIL  cpu trapped, X pc=%08x instr=%08x (%0d cycles)",
					cpu.ex_pc, cpu.ex_instr, cycles);
				$finish;
			end
			if (cycles > TIMEOUT) begin
				$display("FAIL  timeout after %0d cycles, X pc=%08x valid=%0d",
					cycles, cpu.ex_pc, cpu.ex_valid);
				$finish;
			end
		end
	end

endmodule
