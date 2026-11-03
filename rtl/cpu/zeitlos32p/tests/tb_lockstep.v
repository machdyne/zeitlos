/*
 * Zeitlos SOC -- zeitlos32p against zeitlos32, write by write
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Both cores run the same program, each with its own memory and its
 * own random latencies (zeitlos32 on a Wishbone model as in
 * ../../zeitlos32/tests/tb_zeitlos32.v, zeitlos32p on a model of
 * rtl/cache_split.v's two ports with random not-ready cycles as in
 * tb_zeitlos32p.v). Every register write each makes, in the order it
 * makes them, and every store, are recorded and compared entry by
 * entry. Writes are in program order on both cores, so the two lists
 * must be identical; the first difference is reported with the
 * instruction's address.
 *
 * zeitlos32 is the reference: it has run the whole test suite for
 * longer, and has no pipeline to get wrong. Programs that read the
 * cycle counter, or whose interrupts depend on timing, cannot be
 * compared this way -- their values differ by design -- and are left
 * to the directed suite.
 *
 * Prints "MATCH n writes, m stores" or the first mismatch.
 */

`timescale 1ns / 1ps

module tb_lockstep;

	parameter BTB_BITS  = 6;
	parameter MEM_WORDS = 16384;
	parameter LAT       = 12;
	parameter SEED      = 1;
	parameter TIMEOUT   = 3000000;
	parameter FAST_MUL  = 0;
	parameter MAXLOG    = 200000;

	reg clk, rst;
	initial clk = 0;
	always #5 clk = ~clk;

	integer seed, cycles, i;
	reg [1023:0] hexfile;

	// ---------------------------------------------------------- zeitlos32

	wire [31:0] a_adr, a_dato; reg [31:0] a_dati; wire a_we, a_stb, a_cyc;
	wire [3:0] a_sel; reg a_ack; wire a_trap;
	reg [31:0] a_mem [0:MEM_WORDS-1];
	reg a_fin; integer a_exit, a_lat, a_target, a_fcyc;

	zeitlos32_wb #(
		.PROGADDR_RESET(32'h0), .PROGADDR_IRQ(32'h10), .STACKADDR(32'h0001_0000),
		.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1111_1110_1111),
		.ENABLE_MUL(1), .ENABLE_DIV(1), .FAST_MUL(FAST_MUL)
	) ref_cpu (
		.wb_clk_i(clk), .wb_rst_i(rst),
		.wbm_adr_o(a_adr), .mtu_base(32'h0), .wbm_dat_o(a_dato), .wbm_dat_i(a_dati),
		.wbm_we_o(a_we), .wbm_sel_o(a_sel), .wbm_stb_o(a_stb), .wbm_ack_i(a_ack),
		.wbm_cyc_o(a_cyc), .mem_instr(), .irq(32'h0), .eoi(), .trap(a_trap)
	);

	always @(posedge clk) begin
		if (rst) begin
			a_ack <= 0; a_lat <= 0; a_target = 0;
		end else begin
			a_ack <= 0;
			if (a_cyc && a_stb && !a_ack) begin
				if (a_lat == 0) a_target = {$random(seed)} % (LAT + 1);
				if (a_lat < a_target) a_lat <= a_lat + 1;
				else begin
					a_lat <= 0;
					a_ack <= 1;
					a_dati <= (a_adr[31:28] >= 4'he) ? 32'd0 : a_mem[a_adr[31:2] % MEM_WORDS];
					if (a_we) begin
						if (a_adr[31:28] == 4'he) begin
							if (a_adr[7:0] == 8'h00 && !a_fin) begin a_fin = 1; a_exit = a_dato; a_fcyc = cycles; end
						end else if (a_adr[31:28] != 4'hf) begin
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

	// --------------------------------------------------------- zeitlos32p

	wire i_req, i_rdy, i_ack, d_req, d_we, d_rdy, d_ack, b_trap;
	wire [31:0] i_adr, i_dat, d_adr, d_wdat, d_rdat; wire [3:0] d_sel;
	reg [31:0] b_mem [0:MEM_WORDS-1];
	reg b_fin; integer b_exit, b_fcyc;

	zeitlos32p #(
		.PROGADDR_RESET(32'h0), .PROGADDR_IRQ(32'h10), .STACKADDR(32'h0001_0000),
		.LATCHED_IRQ(32'b1111_1111_1111_1111_1111_1111_1110_1111),
		.BTB_BITS(BTB_BITS), .ENABLE_MUL(1), .ENABLE_DIV(1), .FAST_MUL(FAST_MUL)
	) cpu (
		.clk(clk), .rst(rst),
		.i_req(i_req), .i_adr(i_adr), .i_rdy(i_rdy), .i_ack(i_ack), .i_dat(i_dat),
		.d_req(d_req), .d_adr(d_adr), .d_we(d_we), .d_sel(d_sel), .d_wdat(d_wdat),
		.d_rdy(d_rdy), .d_ack(d_ack), .d_rdat(d_rdat),
		.mtu_base(32'h0), .irq(32'h0), .eoi(), .trap(b_trap)
	);

	reg ip, dp, ibusy, dbusy, dw; integer ic, dc; reg [31:0] ia, da, dd; reg [3:0] ds;
	assign i_ack = ip && (ic == 0);
	assign d_ack = dp && (dc == 0);
	assign i_rdy = !ibusy && (!ip || i_ack);
	assign d_rdy = !dbusy && (!dp || d_ack);
	assign i_dat = b_mem[ia[31:2] % MEM_WORDS];
	assign d_rdat = (da[31:28] >= 4'he) ? 32'd0 : b_mem[da[31:2] % MEM_WORDS];

	always @(posedge clk) begin
		if (rst) begin
			ip <= 0; dp <= 0; ic <= 0; dc <= 0; ibusy <= 0; dbusy <= 0;
		end else begin
			ibusy <= (($random(seed) & 7) == 0);
			dbusy <= (($random(seed) & 7) == 0);
			if (ip && ic != 0) ic <= ic - 1;
			if (i_req && i_rdy) begin
				ip <= 1; ia <= i_adr; ic <= {$random(seed)} % (LAT + 1);
			end else if (i_ack) ip <= 0;
			if (d_ack && dw) begin
				if (da[31:28] == 4'he) begin
					if (da[7:0] == 8'h00 && !b_fin) begin b_fin = 1; b_exit = dd; b_fcyc = cycles; end
				end else if (da[31:28] != 4'hf) begin
					if (ds[0]) b_mem[da[31:2] % MEM_WORDS][7:0]   <= dd[7:0];
					if (ds[1]) b_mem[da[31:2] % MEM_WORDS][15:8]  <= dd[15:8];
					if (ds[2]) b_mem[da[31:2] % MEM_WORDS][23:16] <= dd[23:16];
					if (ds[3]) b_mem[da[31:2] % MEM_WORDS][31:24] <= dd[31:24];
				end
			end
			if (dp && dc != 0) dc <= dc - 1;
			if (d_req && d_rdy) begin
				dp <= 1; da <= d_adr; dw <= d_we; ds <= d_sel; dd <= d_wdat;
				dc <= {$random(seed)} % (LAT + 1);
			end else if (d_ack) dp <= 0;
		end
	end

	// ------------------------------------------------------------ the logs

	reg [36:0] a_w [0:MAXLOG-1];   // { rd, value }
	reg [36:0] b_w [0:MAXLOG-1];
	reg [31:0] a_wpc [0:MAXLOG-1];
	reg [31:0] b_wpc [0:MAXLOG-1];
	reg [67:0] a_s [0:MAXLOG-1];   // { sel, adr, data }
	reg [67:0] b_s [0:MAXLOG-1];
	integer an, bn, asn, bsn;

	// zeitlos32's single write port, as its always block left it this
	// edge. Not in the first cycle out of reset (zeitlos32's reset write
	// of x2 is still showing), and not stores to the test ports (the
	// finish store ends each log, and is seen a half cycle apart).
	always @(negedge clk) begin
		if (!rst && cycles > 0 && !a_fin && ref_cpu.rf_we && ref_cpu.rf_idx != 0 && an < MAXLOG) begin
			a_w[an] = { ref_cpu.rf_idx, ref_cpu.rf_dat };
			a_wpc[an] = ref_cpu.pc;
			an = an + 1;
		end
		if (!rst && !a_fin && a_cyc && a_stb && a_ack && a_we && a_adr[31:28] != 4'he && asn < MAXLOG) begin
			a_s[asn] = { a_sel, a_adr, a_dato };
			asn = asn + 1;
		end
		if (!rst && !b_fin && cpu.w_wr && bn < MAXLOG) begin
			b_w[bn] = { cpu.w_rd, cpu.w_val };
			b_wpc[bn] = cpu.w_pc;
			bn = bn + 1;
		end
		if (!rst && !b_fin && d_ack && dw && da[31:28] != 4'he && bsn < MAXLOG) begin
			b_s[bsn] = { ds, da, dd };
			bsn = bsn + 1;
		end
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
			$display("FAIL: did not finish (zeitlos32 %0d, zeitlos32p %0d, traps %0d %0d)",
				a_fin, b_fin, a_trap, b_trap);
			$finish;
		end
		bad = 0;
		// the finish store ends both logs, so they cover the same span
		if (an != bn) begin
			$display("FAIL: %0d register writes from zeitlos32, %0d from zeitlos32p", an, bn);
			bad = 1;
		end
		for (n = 0; n < an && n < bn && !bad; n = n + 1) begin
			if (a_w[n] !== b_w[n]) begin
				$display("FAIL: write %0d: zeitlos32 x%0d=%08x, zeitlos32p x%0d=%08x (zeitlos32p pc %08x)",
					n, a_w[n][36:32], a_w[n][31:0], b_w[n][36:32], b_w[n][31:0], b_wpc[n]);
				bad = 1;
			end
		end
		if (asn != bsn) begin
			$display("FAIL: %0d stores from zeitlos32, %0d from zeitlos32p", asn, bsn);
			bad = 1;
		end
		for (n = 0; n < asn && n < bsn && !bad; n = n + 1) begin
			if (a_s[n] !== b_s[n]) begin
				$display("FAIL: store %0d: zeitlos32 %08x<=%08x/%b, zeitlos32p %08x<=%08x/%b",
					n, a_s[n][63:32], a_s[n][31:0], a_s[n][67:64],
					b_s[n][63:32], b_s[n][31:0], b_s[n][67:64]);
				bad = 1;
			end
		end
		if ($test$plusargs("tail")) begin
			for (n = (an > 4 ? an - 4 : 0); n < an; n = n + 1)
				$display("  A w%0d x%0d=%08x pc %08x", n, a_w[n][36:32], a_w[n][31:0], a_wpc[n]);
			for (n = (bn > 4 ? bn - 4 : 0); n < bn; n = n + 1)
				$display("  B w%0d x%0d=%08x pc %08x", n, b_w[n][36:32], b_w[n][31:0], b_wpc[n]);
			for (n = (asn > 2 ? asn - 2 : 0); n < asn; n = n + 1)
				$display("  A s%0d %08x<=%08x", n, a_s[n][63:32], a_s[n][31:0]);
			for (n = (bsn > 2 ? bsn - 2 : 0); n < bsn; n = n + 1)
				$display("  B s%0d %08x<=%08x", n, b_s[n][63:32], b_s[n][31:0]);
		end
		if (!bad && a_exit == 0 && b_exit == 0)
			$display("MATCH %0d writes, %0d stores (finished in %0d cycles on zeitlos32, %0d on zeitlos32p)",
				an, asn, a_fcyc, b_fcyc);
		else if (!bad)
			$display("FAIL: exit codes %0d / %0d", a_exit, b_exit);
		$finish;
	end

endmodule
