/*
 * Zeitlos SOC -- wb_cache_one's statistics registers with STAT_BRAM=1.
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Random loads, stores and fetches to main memory, interleaved with
 * reads and clears of the statistics registers. Every value read must
 * lie between the matching stat_* counter (kept in simulation, and
 * checked against the RAM on every update by wb_cache_one itself) as
 * it was when the read was taken and as it is when it is answered.
 * Prints "STAT PASS n reads" or the first failure.
 *
 *   iverilog -g2005 -o t rtl/tb/tb_cache_one_stat.v rtl/cache_one.v && vvp t
 */
`timescale 1ns / 1ps
module tb_cache_one_stat;
	reg clk = 0, rst = 1;
	always #5 clk = ~clk;
	reg         c_req = 0, c_we = 0, c_instr = 0;
	reg  [31:0] c_adr = 0, c_wdat = 0;
	reg   [3:0] c_sel = 4'hf;
	wire        c_rdy, c_ack;
	wire [31:0] c_rdat;
	wire [31:0] m_adr, m_dato; wire m_we, m_stb, m_cyc; wire [3:0] m_sel; wire [2:0] m_cti;
	reg m_ack = 0; reg [31:0] m_dati = 0;

	wb_cache_one #(.KB(1), .LINE_WORDS(4), .WBUF_DEPTH(2), .BURST(0), .SNOOP(1), .STAT_BRAM(1)) dut (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.c_req(c_req), .c_adr(c_adr), .c_we(c_we), .c_sel(c_sel), .c_wdat(c_wdat),
		.c_instr(c_instr), .c_rdy(c_rdy), .c_ack(c_ack), .c_rdat(c_rdat),
		.m_adr_o(m_adr), .m_dat_o(m_dato), .m_dat_i(m_dati), .m_we_o(m_we), .m_sel_o(m_sel),
		.m_stb_o(m_stb), .m_cyc_o(m_cyc), .m_cti_o(m_cti), .m_bte_o(), .m_ack_i(m_ack),
		.m_instr_o(), .snoop_stb_i(1'b0), .snoop_adr_i(32'h0), .c_cfg_hit(),
		.mpu_iveto(1'b0), .mpu_veto(1'b0), .mpu_reg(1'b0), .mpu_dat(32'h0));

	integer seed = 7;
	always @(posedge clk) begin
		m_ack <= 0;
		if (m_cyc && m_stb && !m_ack && ({$random(seed)} % 3 == 0)) begin
			m_ack <= 1; m_dati <= m_adr ^ 32'h5a5a_0000;
		end
	end

	function [31:0] shadow(input [3:0] r);
		case (r)
			4'd1: shadow = dut.stat_i_hits;  4'd2: shadow = dut.stat_i_misses;
			4'd5: shadow = dut.stat_d_hits;  4'd6: shadow = dut.stat_d_misses;
			4'd8: shadow = dut.stat_loads;   4'd9: shadow = dut.stat_stores;
			4'd10: shadow = dut.stat_stall;  default: shadow = dut.stat_wbfull;
		endcase
	endfunction

	integer i, k, nreads = 0, bad = 0;
	reg [3:0] r; reg [31:0] lo, hi, v; reg isrd, iscfg;
	reg [3:0] regs [0:7];
	initial begin
		regs[0]=1; regs[1]=2; regs[2]=5; regs[3]=6; regs[4]=8; regs[5]=9; regs[6]=10; regs[7]=12;
		repeat (4) @(posedge clk); rst = 0;
		// enable the D side and the write buffer (FEAT: reg 4)
		@(negedge clk); c_req = 1; c_we = 1; c_instr = 0; c_adr = 32'h7000_0110; c_wdat = 32'h5;
		@(posedge clk); while (!c_rdy) @(posedge clk);
		@(negedge clk); c_req = 0; while (!c_ack) @(negedge clk);
		for (i = 0; i < 4000 && !bad; i = i + 1) begin
			@(negedge clk);
			k = {$random(seed)} % 10;
			iscfg = (k >= 8); isrd = (k == 8);
			r = regs[{$random(seed)} % 8];
			c_req = 1;
			c_instr = !iscfg && (k < 4);
			c_we = iscfg ? !isrd : (k >= 6);
			c_adr = iscfg ? (32'h7000_0100 | {r, 2'b00}) : (32'h4000_0000 | (({$random(seed)} % 512) << 2));
			c_wdat = $random(seed);
			if (iscfg && !isrd && ({$random(seed)} % 4 != 0)) begin
				// most writes: a harmless register (FEAT, unchanged) instead of a clear
				c_adr = 32'h7000_0110; c_wdat = 32'h5;
			end
			@(posedge clk); while (!c_rdy) @(posedge clk);
			lo = shadow(r);
			@(negedge clk); c_req = 0;
			while (!c_ack) @(negedge clk);
			if (iscfg && isrd) begin
				hi = shadow(r); v = c_rdat; nreads = nreads + 1;
				if (v < lo || v > hi) begin
					$display("STAT FAIL reg %0d read %0d, counter %0d..%0d", r, v, lo, hi);
					bad = 1;
				end
			end
		end
		if (!bad) $display("STAT PASS %0d reads", nreads);
		$finish;
	end
endmodule
