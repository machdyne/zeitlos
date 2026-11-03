/*
 * Zeitlos SOC -- zeitlos32p under random interrupts, against zeitlos32
 *
 * Built from tb_mpu_lockstep.v (the same two machines, MPUs disabled)
 * for gen_stress.py --irq programs. Machine B (zeitlos32p on the real
 * split cache) gets interrupt pulses at random on line 3, so its
 * handler runs at arbitrary points: in load-use stalls, with both
 * memory slots full, during refetches. Machine A (zeitlos32) gets
 * none. The handler only counts (at COUNTER) and returns, so when both
 * finish their memories must be identical except for that word; the
 * program dumps every register into memory before finishing. Prints
 * "SAME n interrupts" or the first difference.
 *
 * (Below: the original header of tb_mpu_lockstep.v.)
 *
 * Zeitlos SOC -- zeitlos32p + mpu_split against zeitlos32 + wb_mpu
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Two machines run the same program (prog/t_mpu.S):
 *
 *   A  zeitlos32_wb -> wb_mpu (rtl/mpu.v, XLATE_ON_BUS 0, as
 *      rtl/sysctl.v builds it for zeitlos32) -> memory
 *   B  zeitlos32p -> wb_mpu_split (rtl/mpu_split.v) + wb_cache_split
 *      (rtl/cache_split.v) -> memory, as rtl/sysctl.v builds it for
 *      CPU_ZEITLOS32P
 *
 * Both translate through the MTU with the base at 0x4000_8000 and the
 * MPU sees the same base. Each memory has its own random latency. The
 * memory is 64KB, aliased into nibbles 0 and 4; other nibbles read 0
 * and ignore writes, except 0xE (the test port) and 0xF (a console).
 *
 * Every register write and every store that reaches memory is
 * recorded and the two lists compared, entry by entry: blocked stores
 * must be missing from both, blocked loads must read 0 on both, and
 * the fault registers -- which the program reads back after each
 * scenario -- must hold the same addresses, PCs, reasons and counts.
 * Writes to nibble 7 (the cache's registers, which B's cache answers
 * itself and A's memory sinks) are left out of the store lists.
 *
 * Prints "MATCH n writes, m stores" or the first mismatch.
 */

`timescale 1ns / 1ps

module tb_irq_stress;

	parameter BTB_BITS  = 6;
	parameter MEM_WORDS = 16384;
	parameter LAT       = 8;
	parameter SEED      = 1;
	parameter TIMEOUT   = 2000000;
	parameter MAXLOG    = 20000;
	parameter [31:0] MTU = 32'h4000_8000;

	reg clk, rst;
	initial clk = 0;
	always #5 clk = ~clk;

	integer seed, cycles, i;
	reg [1023:0] hexfile;

	parameter IRQ_GAP = 40;          // mean cycles between pulses on B
	parameter [31:0] COUNTER = 32'h4000_8c00;
	reg [31:0] b_irq;
	integer nirq;
	always @(posedge clk) begin
		if (rst) begin b_irq <= 32'h0; nirq <= 0; end
		else begin
			b_irq <= 32'h0;
			if (!b_fin && ({$random(seed)} % IRQ_GAP) == 0) begin b_irq <= 32'h8; nirq <= nirq + 1; end
		end
	end

	// The MTU base, one register per machine, written by a store to
	// 0x9000_0000 when memory acknowledges it (as rtl/mtu.v's register
	// is written by the bus cycle). Starts at MTU.
	reg [31:0] a_mtu, b_mtu;

	// ---------------------------------------------------------------- A

	wire [31:0] a_adr, a_dato, a_dati; wire a_we, a_stb, a_cyc, a_instr;
	wire [3:0] a_sel; wire a_ack; wire a_trap;
	wire a_stb_m, a_cyc_m; reg a_ack_m; reg [31:0] a_dat_m;
	reg [31:0] a_mem [0:MEM_WORDS-1];
	reg a_fin; integer a_exit, a_lat, a_target, a_fcyc;

	zeitlos32_wb #(
		.PROGADDR_RESET(32'h0), .PROGADDR_IRQ(32'h10), .STACKADDR(32'h0001_0000),
		.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1111_1110_1111),
		.ENABLE_MUL(1), .ENABLE_DIV(1), .FAST_MUL(0)
	) ref_cpu (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.wbm_adr_o(a_adr), .mtu_base(a_mtu), .wbm_dat_o(a_dato), .wbm_dat_i(a_dati),
		.wbm_we_o(a_we), .wbm_sel_o(a_sel), .wbm_stb_o(a_stb), .wbm_ack_i(a_ack),
		.wbm_cyc_o(a_cyc), .mem_instr(a_instr), .irq(32'h0), .eoi(), .trap(a_trap)
	);

	wb_mpu #(.XLATE_ON_BUS(0)) a_mpu (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.c_adr_i(a_adr), .c_dat_i(a_dato), .c_we_i(a_we), .c_sel_i(a_sel),
		.c_stb_i(a_stb), .c_cyc_i(a_cyc), .c_instr_i(a_instr),
		.c_dat_o(a_dati), .c_ack_o(a_ack),
		.d_stb_o(a_stb_m), .d_cyc_o(a_cyc_m), .d_dat_i(a_dat_m), .d_ack_i(a_ack_m),
		.mtu_base_i(a_mtu), .irq_o()
	);

	always @(posedge clk) begin
		if (rst) begin
			a_ack_m <= 0; a_lat <= 0; a_target = 0; a_mtu <= MTU;
		end else begin
			a_ack_m <= 0;
			if (a_cyc_m && a_stb_m && !a_ack_m) begin
				if (a_lat == 0) a_target = {$random(seed)} % (LAT + 1);
				if (a_lat < a_target) a_lat <= a_lat + 1;
				else begin
					a_lat <= 0;
					a_ack_m <= 1;
					a_dat_m <= (a_adr[31:28] == 4'h0 || a_adr[31:28] == 4'h4) ?
						a_mem[a_adr[31:2] % MEM_WORDS] : 32'd0;
					if (a_we) begin
						if (a_adr[31:28] == 4'he) begin
							if (a_adr[7:0] == 8'h00 && !a_fin) begin a_fin = 1; a_exit = a_dato; a_fcyc = cycles; end
						end else if (a_adr == 32'h9000_0000) begin
							a_mtu <= a_dato;
						end else if (a_adr[31:28] == 4'h0 || a_adr[31:28] == 4'h4) begin
							if (a_sel[0]) a_mem[a_adr[31:2] % MEM_WORDS][7:0]   <= a_dato[7:0];
							if (a_sel[1]) a_mem[a_adr[31:2] % MEM_WORDS][15:8]  <= a_dato[15:8];
							if (a_sel[2]) a_mem[a_adr[31:2] % MEM_WORDS][23:16] <= a_dato[23:16];
							if (a_sel[3]) a_mem[a_adr[31:2] % MEM_WORDS][31:24] <= a_dato[31:24];
						end
					end
				end
			end
		end
	end

	// ---------------------------------------------------------------- B

	wire zi_req, zi_rdy, zi_ack, zd_req, zd_we, zd_rdy, zd_ack, b_trap;
	wire [31:0] zi_adr, zi_dat, zd_adr, zd_wdat, zd_rdat; wire [3:0] zd_sel;
	wire zd_priv, zi_priv, c_privreg, c_gate, c_own, fv_ev, m_en, m_enf;
	wire [31:0] zd_pc, c_pc, fv_adr, fv_pc; wire [3:0] fv_why;
	wire zi_veto, zd_veto, zd_reg; wire [31:0] zd_reg_dat;

	zeitlos32p #(
		.PROGADDR_RESET(32'h0), .PROGADDR_IRQ(32'h10), .STACKADDR(32'h0001_0000),
		.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1111_1110_1111),
		.BTB_BITS(BTB_BITS), .ENABLE_MUL(1), .ENABLE_DIV(1), .FAST_MUL(0), .ENABLE_MPU(1)
	) cpu (
		.clk(clk), .rst(rst),
		.i_req(zi_req), .i_adr(zi_adr), .i_rdy(zi_rdy), .i_ack(zi_ack), .i_dat(zi_dat),
		.d_req(zd_req), .d_adr(zd_adr), .d_we(zd_we), .d_sel(zd_sel), .d_wdat(zd_wdat),
		.d_rdy(zd_rdy), .d_ack(zd_ack), .d_rdat(zd_rdat),
		.mtu_base(b_mtu),
		.mpu_en(m_en), .mpu_enforce(m_enf),
		.chk_pc(c_pc), .chk_privreg(c_privreg), .chk_gate(c_gate), .chk_own(c_own),
		.d_priv(zd_priv), .d_pc(zd_pc), .i_priv(zi_priv),
		.fv_ev(fv_ev), .fv_adr(fv_adr), .fv_pc(fv_pc), .fv_why(fv_why),
		.irq(b_irq), .eoi(), .trap(b_trap)
	);

	wb_mpu_split b_mpu (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.d_req(zd_req), .d_adr(zd_adr), .d_we(zd_we), .d_sel(zd_sel),
		.d_wdat(zd_wdat), .d_priv(zd_priv), .d_pc(zd_pc), .d_rdy(zd_rdy),
		.d_veto(zd_veto), .d_reg(zd_reg), .d_reg_dat(zd_reg_dat),
		.i_adr(zi_adr), .i_priv(zi_priv), .i_veto(zi_veto),
		.chk_pc(c_pc), .chk_privreg(c_privreg), .chk_gate(c_gate), .chk_own(c_own),
		.fv_ev(fv_ev), .fv_adr(fv_adr), .fv_pc(fv_pc), .fv_why(fv_why),
		.en_o(m_en), .enforce_o(m_enf),
		.mtu_base_i(b_mtu), .irq_o()
	);

	wire [31:0] m_adr, m_dato; wire m_we, m_stb, m_cyc; wire [3:0] m_sel;
	reg m_ack; reg [31:0] m_dati;

	wb_cache_split #(
		.I_KB(1), .I_LINE_WORDS(4), .D_KB(1), .D_LINE_WORDS(4),
		.WBUF_DEPTH(2), .BURST(0), .SNOOP(1)
	) b_cache (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.i_req(zi_req), .i_adr(zi_adr), .i_rdy(zi_rdy), .i_ack(zi_ack), .i_dat(zi_dat),
		.d_req(zd_req), .d_adr(zd_adr), .d_we(zd_we), .d_sel(zd_sel), .d_wdat(zd_wdat),
		.d_rdy(zd_rdy), .d_ack(zd_ack), .d_rdat(zd_rdat),
		.i_mpu_veto(zi_veto), .d_mpu_veto(zd_veto), .d_mpu_reg(zd_reg),
		.d_mpu_dat(zd_reg_dat),
		.m_adr_o(m_adr), .m_dat_o(m_dato), .m_dat_i(m_dati), .m_we_o(m_we),
		.m_sel_o(m_sel), .m_stb_o(m_stb), .m_cyc_o(m_cyc), .m_cti_o(), .m_bte_o(),
		.m_ack_i(m_ack), .m_instr_o(),
		.snoop_stb_i(1'b0), .snoop_adr_i(32'h0), .d_cfg_hit()
	);

	reg [31:0] b_mem [0:MEM_WORDS-1];
	reg b_fin; integer b_exit, b_lat, b_target, b_fcyc;

	always @(posedge clk) begin
		if (rst) begin
			m_ack <= 0; b_lat <= 0; b_target = 0; b_mtu <= MTU;
		end else begin
			m_ack <= 0;
			if (m_cyc && m_stb && !m_ack) begin
				if (b_lat == 0) b_target = {$random(seed)} % (LAT + 1);
				if (b_lat < b_target) b_lat <= b_lat + 1;
				else begin
					b_lat <= 0;
					m_ack <= 1;
					m_dati <= (m_adr[31:28] == 4'h0 || m_adr[31:28] == 4'h4) ?
						b_mem[m_adr[31:2] % MEM_WORDS] : 32'd0;
					if (m_we) begin
						if (m_adr[31:28] == 4'he) begin
							if (m_adr[7:0] == 8'h00 && !b_fin) begin b_fin = 1; b_exit = m_dato; b_fcyc = cycles; end
						end else if (m_adr == 32'h9000_0000) begin
							b_mtu <= m_dato;
						end else if (m_adr[31:28] == 4'h0 || m_adr[31:28] == 4'h4) begin
							if (m_sel[0]) b_mem[m_adr[31:2] % MEM_WORDS][7:0]   <= m_dato[7:0];
							if (m_sel[1]) b_mem[m_adr[31:2] % MEM_WORDS][15:8]  <= m_dato[15:8];
							if (m_sel[2]) b_mem[m_adr[31:2] % MEM_WORDS][23:16] <= m_dato[23:16];
							if (m_sel[3]) b_mem[m_adr[31:2] % MEM_WORDS][31:24] <= m_dato[31:24];
						end
					end
				end
			end
		end
	end

	// ------------------------------------------------------------ the logs

	reg [36:0] a_w [0:MAXLOG-1];
	reg [36:0] b_w [0:MAXLOG-1];
	reg [31:0] b_wpc [0:MAXLOG-1];
	reg [67:0] a_s [0:MAXLOG-1];
	reg [67:0] b_s [0:MAXLOG-1];
	integer an, bn, asn, bsn;

	always @(negedge clk) begin
		if (!rst && cycles > 0 && !a_fin && ref_cpu.rf_we && ref_cpu.rf_idx != 0 && an < MAXLOG) begin
			a_w[an] = { ref_cpu.rf_idx, ref_cpu.rf_dat };
			an = an + 1;
		end
		if (!rst && !a_fin && a_cyc_m && a_stb_m && a_ack_m && a_we &&
		    a_adr[31:28] != 4'he && a_adr[31:28] != 4'h7 && asn < MAXLOG) begin
			a_s[asn] = { a_sel, a_adr, a_dato };
			asn = asn + 1;
		end
		if (!rst && !b_fin && cpu.w_wr && bn < MAXLOG) begin
			b_w[bn] = { cpu.w_rd, cpu.w_val };
			b_wpc[bn] = cpu.w_pc;
			bn = bn + 1;
		end
		if (!rst && !b_fin && m_cyc && m_stb && m_ack && m_we &&
		    m_adr[31:28] != 4'he && m_adr[31:28] != 4'h7 && bsn < MAXLOG) begin
			b_s[bsn] = { m_sel, m_adr, m_dato };
			bsn = bsn + 1;
		end
	end

	// Bus accesses to the peripheral-like nibbles 6 and B, per machine.
	// They have no architectural trace here (reads give 0, writes go
	// nowhere), so they are counted: a blocked access must not reach
	// memory on either machine, so the counts must agree.
	integer a_n6, a_nb, b_n6, b_nb;
	initial begin a_n6 = 0; a_nb = 0; b_n6 = 0; b_nb = 0; end
	always @(posedge clk) if (!rst) begin
		if (a_cyc_m && a_stb_m && a_ack_m && a_adr[31:28] == 4'h6) a_n6 = a_n6 + 1;
		if (a_cyc_m && a_stb_m && a_ack_m && a_adr[31:28] == 4'hb) a_nb = a_nb + 1;
		if (m_cyc && m_stb && m_ack && m_adr[31:28] == 4'h6) b_n6 = b_n6 + 1;
		if (m_cyc && m_stb && m_ack && m_adr[31:28] == 4'hb) b_nb = b_nb + 1;
	end

	// coverage: what B's MPU path actually did
	integer cv_dveto, cv_ireg, cv_iveto, cv_fv;
	initial begin cv_dveto = 0; cv_ireg = 0; cv_iveto = 0; cv_fv = 0; end
	always @(posedge clk) if (!rst) begin
		if (zd_req && zd_rdy && zd_veto) cv_dveto = cv_dveto + 1;
		if (zd_req && zd_rdy && zd_reg) cv_ireg = cv_ireg + 1;
		if (zi_req && zi_rdy && zi_veto) cv_iveto = cv_iveto + 1;
		if (fv_ev) cv_fv = cv_fv + 1;
	end

	integer n, bad;
	initial begin
		seed = SEED; cycles = 0; an = 0; bn = 0; asn = 0; bsn = 0;
		a_fin = 0; b_fin = 0; a_exit = 0; b_exit = 0;
		rst = 1;
		for (i = 0; i < MEM_WORDS; i = i + 1) begin a_mem[i] = 0; b_mem[i] = 0; end
		if (!$value$plusargs("hex=%s", hexfile)) begin
			$display("FAIL: no +hex=<file> given"); $finish;
		end
		$readmemh(hexfile, a_mem);
		$readmemh(hexfile, b_mem);
		repeat (4) @(posedge clk);
		rst = 0;
		while (!(a_fin && b_fin) && cycles < TIMEOUT && !a_trap && !b_trap) begin
			@(posedge clk);
			cycles = cycles + 1;
		end
		repeat (2) @(posedge clk);
		if (!(a_fin && b_fin)) begin
			$display("FAIL: did not finish (A %0d, B %0d, traps %0d %0d, B pc in X %08x)",
				a_fin, b_fin, a_trap, b_trap, cpu.ex_pc);
			$finish;
		end
		bad = 0;
		for (n = 0; n < MEM_WORDS && !bad; n = n + 1) begin
			if (n != COUNTER[15:2] && a_mem[n] !== b_mem[n]) begin
				$display("FAIL: memory word %08x: A %08x, B %08x (%0d interrupts on B)",
					32'h4000_0000 + 4 * n, a_mem[n], b_mem[n], nirq);
				bad = 1;
			end
		end
		if (!bad && a_exit == 0 && b_exit == 0)
			$display("SAME %0d interrupts on B, %0d taken (finished in %0d / %0d cycles)",
				nirq, b_mem[COUNTER[15:2]], a_fcyc, b_fcyc);
		else if (!bad)
			$display("FAIL: exit codes %0d / %0d", a_exit, b_exit);
		$finish;
	end

endmodule
