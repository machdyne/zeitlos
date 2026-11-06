/*
 * tb_console_ddr3.v -- tb_console_bus.v on the real DDR3 controller
 * (rtl/mem/ddr3_ctrl.v, through rtl/tb/ddr3_phy_sim.v to
 * rtl/tb/ddr3_model.v), as sergei_ml2_console has it: the fetch reads
 * a word at a time (no SDRAM_BURST). The picture is written through the
 * bus first (no back door into the DDR3 model), PIC_LINES lines of it,
 * and the scanout shows those lines over and over.
 *
 *   iverilog -g2012 -o /tmp/tb_cd rtl/tb/tb_console_ddr3.v \
 *       rtl/tb/ddr3_model.v rtl/tb/ddr3_phy_sim.v rtl/mem/ddr3_ctrl.v \
 *       rtl/arbiter_main.v rtl/arbiter_gfx.v rtl/gpu/gpu_ckfetch.v
 *   vvp /tmp/tb_cd
 *
 * -P tb_console_ddr3.WC=1: the controller's write combining
 * (`DDR3_WCOMBINE, docs/ddr3.md); -P tb_console_ddr3.CPU=0: the CPU-like
 * master sits out, so the writer's count is its own rate.
 *
 * What tb_console_bus.v says follows, for SDRAM.
 *
 * tb_console_bus.v -- the console's main-memory traffic on the real SDRAM
 * controller (`VRAM_SDRAM; docs/profiles.md, "Lakritz").
 *
 * rtl/mem/sdram_kianv.v (BURST=1) on rtl/tb/sdram_model.v behind
 * rtl/arbiter_main.v, with rtl/arbiter_gfx.v on its third port, exactly
 * as rtl/sysctl.v wires them (CTI muxed from the CPU's port or the gfx
 * port's winner):
 *
 *   main m0  a CPU-like master: random single reads and writes (random
 *            byte lanes) and four-word burst reads, as the cache's line
 *            fills, all checked against a reference
 *   gfx  m0  rtl/gpu/gpu_ckfetch.v reading a linear picture, its
 *            graphics port read as gpu_video reads it (each line's four
 *            segments, 80 words, a line every LINE cycles) and every
 *            word checked against the picture
 *   gfx  m2  a T2-like writer: single writes as fast as it is let,
 *            elsewhere, checked at the end
 *
 * Fails on: a master that stops making progress (the bus hung), a word
 * read wrong by anyone, a line the fetch had not ready after the first,
 * and any protocol error the SDRAM model detects.
 *
 *   iverilog -g2005 -o /tmp/tb_cb rtl/tb/tb_console_bus.v \
 *       rtl/mem/sdram_kianv.v rtl/tb/sdram_model.v rtl/arbiter_main.v \
 *       rtl/arbiter_gfx.v rtl/gpu/gpu_ckfetch.v && vvp /tmp/tb_cb
 */

`timescale 1ns / 1ps

module tb_console_ddr3;

	parameter LINE = 3050;          // cycles a game-mode line at 48 MHz
	parameter LINES = 100;
	parameter PIC_LINES = 24;       // lines of the picture written and shown
	parameter WC = 0;               // the controller's write combining
	parameter CPU = 1;              // 0: the CPU sits out (the writer's own rate)
	parameter CKL = 2;              // the fetch's lines ahead (gpu_ckfetch.v LINES)

	localparam [24:0] PIC = 25'h0_8000;     // the picture
	localparam [24:0] CPU_LO = 25'h0_0000;  // the CPU's own 16 KB
	localparam [24:0] T2_LO = 25'h2_0000;   // the writer's 16 KB

	localparam TCK_PS = 10417;              // 96 MHz DRAM clock
	reg ck = 0, clk = 0, rst = 1;
	initial forever #(TCK_PS / 2000.0) ck = ~ck;
	initial begin
		@(negedge ck);
		forever begin
			@(posedge ck); clk = 1'b1;
			@(posedge ck); clk = 1'b0;
		end
	end

	// ---------------------------------------------------------------
	// the DDR3
	// ---------------------------------------------------------------
	wire [31:0] s_adr, s_dat_o;
	wire [31:0] s_dat_i;
	wire        s_we, s_stb, s_cyc, s_ack;
	wire [3:0]  s_sel;
	wire [1:0]  master;
	wire [2:0]  gfx_cti;
	reg  [2:0]  c_cti;
	wire init_done;

	wire c_reset_n, c_cke, c_odt, c_cs_n, c_ras_n, c_cas_n, c_we_n;
	wire [15:0] c_a;
	wire [2:0] c_ba;
	wire c_wren, c_rden, c_rvalid;
	wire [127:0] c_wdata, c_rdata;
	wire [15:0] c_wmask;
	wire d_reset_n, d_cke, d_odt, d_cs_n, d_ras_n, d_cas_n, d_we_n;
	wire [15:0] d_a;
	wire [2:0] d_ba;
	wire [1:0] d_dm;
	wire [15:0] d_dq;
	wire [1:0] d_dqs;

	ddr3_ctrl #(.ROW_BITS(15), .COL_BITS(10), .BANK_BITS(3), .TRFC_NS(260),
		.CLK_MHZ(48), .INIT_DIV(1000), .WCOMBINE(WC)) dut (
		.clk_i(clk), .rst_i(rst),
		.wb_adr_i(s_adr[28:2]),
		.wb_dat_i(s_dat_o), .wb_dat_o(s_dat_i), .wb_we_i(s_we), .wb_sel_i(s_sel),
		.wb_stb_i(s_stb), .wb_cyc_i(s_cyc), .wb_ack_o(s_ack),
		.phy_ready_i(1'b1), .init_done_o(init_done), .rd_timeout_o(),
		.no_refresh_i(1'b0), .wr_noread_i(1'b0), .rdbuf_off_i(1'b0),
		.phy_reset_n_o(c_reset_n), .phy_cke_o(c_cke), .phy_odt_o(c_odt),
		.phy_cs_n_o(c_cs_n), .phy_ras_n_o(c_ras_n), .phy_cas_n_o(c_cas_n),
		.phy_we_n_o(c_we_n), .phy_a_o(c_a), .phy_ba_o(c_ba),
		.phy_wren_o(c_wren), .phy_wdata_o(c_wdata), .phy_wmask_o(c_wmask),
		.phy_rden_o(c_rden), .phy_rvalid_i(c_rvalid), .phy_rdata_i(c_rdata)
	);
	ddr3_phy_sim #(.TCK_PS(TCK_PS), .CL(6), .CWL(6)) phy (
		.clk_i(clk), .ck_i(ck), .rst_i(rst),
		.ctl_reset_n_i(c_reset_n), .ctl_cke_i(c_cke), .ctl_odt_i(c_odt),
		.ctl_cs_n_i(c_cs_n), .ctl_ras_n_i(c_ras_n), .ctl_cas_n_i(c_cas_n),
		.ctl_we_n_i(c_we_n), .ctl_a_i(c_a), .ctl_ba_i(c_ba),
		.ctl_wren_i(c_wren), .ctl_wdata_i(c_wdata), .ctl_wmask_i(c_wmask),
		.ctl_rden_i(c_rden), .ctl_rvalid_o(c_rvalid), .ctl_rdata_o(c_rdata),
		.ddr_reset_n(d_reset_n), .ddr_cke(d_cke), .ddr_odt(d_odt),
		.ddr_cs_n(d_cs_n), .ddr_ras_n(d_ras_n), .ddr_cas_n(d_cas_n),
		.ddr_we_n(d_we_n), .ddr_a(d_a), .ddr_ba(d_ba), .ddr_dm(d_dm),
		.ddr_dq(d_dq), .ddr_dqs(d_dqs)
	);
	ddr3_model #(.ROW_BITS(15), .COL_BITS(10), .BANK_BITS(3), .ROW_ALIAS(6),
		.TCK_PS(TCK_PS)) dram (
		.ck(ck), .reset_n(d_reset_n), .cke(d_cke), .cs_n(d_cs_n),
		.ras_n(d_ras_n), .cas_n(d_cas_n), .we_n(d_we_n), .a(d_a), .ba(d_ba),
		.dm(d_dm), .dq(d_dq), .dqs(d_dqs)
	);

	// ---------------------------------------------------------------
	// the CPU-like master
	// ---------------------------------------------------------------
	reg  [31:0] c_adr, c_dat;
	reg  [3:0]  c_sel;
	reg         c_we, c_stb, c_cyc;
	wire [31:0] c_q;
	wire        c_ack;

	// ---------------------------------------------------------------
	// the gfx port
	// ---------------------------------------------------------------
	wire [31:0] g_adr, g_dat_o, g_dat_i;
	wire [3:0]  g_sel;
	wire        g_we, g_stb, g_cyc, g_ack;
	wire [1:0]  gm;

	wb_arbiter_main marb (
		.clk(clk), .rst(rst),
		.m0_adr_i(c_adr), .m0_dat_i(c_dat), .m0_dat_o(c_q), .m0_we_i(c_we),
		.m0_sel_i(c_sel), .m0_stb_i(c_stb), .m0_cyc_i(c_cyc), .m0_ack_o(c_ack),
		.m1_adr_i(32'h0), .m1_dat_i(32'h0), .m1_dat_o(), .m1_we_i(1'b0),
		.m1_sel_i(4'h0), .m1_stb_i(1'b0), .m1_cyc_i(1'b0), .m1_ack_o(),
		.m2_adr_i(g_adr), .m2_dat_i(g_dat_o), .m2_dat_o(g_dat_i), .m2_we_i(g_we),
		.m2_sel_i(g_sel), .m2_stb_i(g_stb), .m2_cyc_i(g_cyc), .m2_ack_o(g_ack),
		.s_adr_o(s_adr), .s_dat_o(s_dat_o), .s_dat_i(s_dat_i), .s_we_o(s_we),
		.s_sel_o(s_sel), .s_stb_o(s_stb), .s_cyc_o(s_cyc), .s_ack_i(s_ack),
		.master(master)
	);

	// the fetch
	wire [31:0] f_adr, f_dat;
	wire [3:0]  f_sel;
	wire [2:0]  f_cti;
	wire        f_cyc, f_stb, f_we, f_ack;
	wire [15:0] misses, lines;
	reg  [9:0]  gb_row = 10'd0;
	reg  [14:0] gb_adr = 15'd0;
	wire [31:0] gb_dat;
	reg         en = 0;

	gpu_ckfetch #(.BURST(0), .VRAM_WORDS(4800), .LINES(CKL)) ckf (
		.clk(clk), .rst(rst), .en(en), .base(32'h4000_0000 | {7'd0, PIC}),
		.gb_row(gb_row), .gb_adr(gb_adr), .gb_dat(gb_dat), .vram_dat(32'h0),
		.m_adr_o(f_adr), .m_cyc_o(f_cyc), .m_stb_o(f_stb), .m_cti_o(f_cti),
		.m_sel_o(f_sel), .m_we_o(f_we), .m_dat_i(f_dat), .m_ack_i(f_ack),
		.misses(misses), .lines(lines)
	);

	// the T2-like writer
	reg  [31:0] w_adr = {7'd0, T2_LO}, w_dat = 32'h0;
	reg         w_cyc = 0, w_on = 0;
	wire        w_ack;
	integer     w_count = 0;

	wb_arbiter_gfx garb (
		.clk(clk), .rst(rst),
		.m0_adr_i(f_adr), .m0_dat_i(32'h0), .m0_dat_o(f_dat),
		.m0_we_i(f_we), .m0_sel_i(f_sel), .m0_cti_i(f_cti),
		.m0_stb_i(f_stb), .m0_cyc_i(f_cyc), .m0_ack_o(f_ack),
		.m1_adr_i(32'h0), .m1_dat_i(32'h0), .m1_dat_o(),
		.m1_we_i(1'b0), .m1_sel_i(4'h0), .m1_cti_i(3'b000),
		.m1_stb_i(1'b0), .m1_cyc_i(1'b0), .m1_ack_o(),
		.m2_adr_i(w_adr), .m2_dat_i(w_dat), .m2_dat_o(),
		.m2_we_i(1'b1), .m2_sel_i(4'hf), .m2_cti_i(3'b000),
		.m2_stb_i(w_stb), .m2_cyc_i(w_cyc), .m2_ack_o(w_ack),
		.s_adr_o(g_adr), .s_dat_o(g_dat_o), .s_dat_i(g_dat_i),
		.s_we_o(g_we), .s_sel_o(g_sel), .s_cti_o(gfx_cti),
		.s_stb_o(g_stb), .s_cyc_o(g_cyc), .s_ack_i(g_ack),
		.master(gm)
	);

	// WRUN words a grant, STB dropped between them, as gpu_t2.v's
	// posted-write queue does
	parameter WRUN = 8;
	reg w_stb = 0;
	integer w_run = 0;
	always @(posedge clk) begin
		if (!w_cyc) begin
			if (w_on) begin
				w_cyc <= 1'b1;
				w_stb <= 1'b1;
				w_run = 0;
				w_dat <= w_adr ^ 32'h5a5a_0000;
			end
		end else if (!w_stb) begin
			w_stb <= 1'b1;
			w_dat <= w_adr ^ 32'h5a5a_0000;
		end else if (w_ack) begin
			w_count = w_count + 1;
			w_run = w_run + 1;
			w_adr <= (w_adr[13:0] == 14'h3ffc) ? {7'd0, T2_LO} : (w_adr + 32'd4);
			w_stb <= 1'b0;
			if (w_run >= WRUN) w_cyc <= 1'b0;
		end
	end

	// ---------------------------------------------------------------
	// the reference
	// ---------------------------------------------------------------
	reg [31:0] pic [0:19199];
	reg [31:0] cref [0:4095];
	integer errors = 0;


	// one CPU transaction; stalls longer than `limit` fail
	integer stall;
	task cpu_cycle(input we, input [31:0] a, input [31:0] d, input [3:0] sel, output [31:0] q);
		begin
			@(posedge clk);
			c_adr <= a; c_dat <= d; c_sel <= sel; c_we <= we; c_cti <= 3'b000;
			c_cyc <= 1; c_stb <= 1;
			stall = 0;
			@(posedge clk);
			while (!c_ack) begin
				@(posedge clk);
				stall = stall + 1;
				if (stall > 4000) begin
					$display("FAIL: the CPU's %s at %h waited 4,000 cycles: the bus is hung",
						we ? "write" : "read", a);
					$display("RESULT: FAIL");
					$finish;
				end
			end
			q = c_q;
			c_cyc <= 0; c_stb <= 0; c_we <= 0;
		end
	endtask

	// a four-word line fill, as the cache does it
	reg [31:0] lq [0:3];
	integer bi;
	task cpu_burst(input [31:0] a);
		begin
			@(posedge clk);
			c_adr <= a; c_sel <= 4'h0; c_we <= 0; c_cti <= 3'b010;
			c_cyc <= 1; c_stb <= 1;
			bi = 0; stall = 0;
			while (bi < 4) begin
				@(posedge clk);
				stall = stall + 1;
				if (stall > 4000) begin
					$display("FAIL: the CPU's line fill at %h hung", a);
					$display("RESULT: FAIL");
					$finish;
				end
				if (c_ack) begin
					lq[bi] = c_q;
					bi = bi + 1;
					c_adr <= c_adr + 4;
					c_cti <= (bi == 3) ? 3'b111 : 3'b010;
					if (bi == 4) begin c_cyc <= 0; c_stb <= 0; c_cti <= 3'b000; end
				end
			end
		end
	endtask

	// ---------------------------------------------------------------
	// the scanout: gpu_video's reads of each line
	// ---------------------------------------------------------------
	integer L, s, wd, k, checked = 0, bad = 0, line_bad;
	reg [14:0] ra [0:1];
	reg        rv [0:1];
	reg [31:0] want_q [0:1];
	reg        scan_on = 0;
	integer    lines_shown = 0;

	initial begin : scan
		integer t, n;
		reg [14:0] a;
		reg [31:0] w;
		wait (scan_on);
		for (n = 0; n < LINES; n = n + 1) begin
			L = n % PIC_LINES;
			@(posedge clk) gb_row <= L;
			repeat (3) @(posedge clk);
			line_bad = 0;
			for (s = 0; s < 4; s = s + 1)
				for (wd = 0; wd < 20; wd = wd + 1) begin
					a = (L + 240 * s) * 20 + wd;
					@(posedge clk) gb_adr <= a;
					@(posedge clk);
					@(posedge clk);
					#1;
					w = pic[L * 80 + s * 20 + wd];
					if (gb_dat !== w) line_bad = line_bad + 1;
				end
			if (n >= 2 && L != 0) begin    /* after a wrap the line is late */
				checked = checked + 1;
				if (line_bad) begin
					bad = bad + 1;
					if (bad < 6) $display("FAIL: line %0d (shown %0d): %0d of 80 words wrong", L, n, line_bad);
				end
			end
			lines_shown = n + 1;
			repeat (LINE - 3 - 80 * 3) @(posedge clk);
		end
	end

	// ---------------------------------------------------------------
	// the run
	// ---------------------------------------------------------------
	integer i, op, ci, cbad = 0, ops = 0;
	reg [31:0] q, r, a32, d, m;
	reg [3:0] sl;
	integer mis0;

	initial begin
		c_cyc = 0; c_stb = 0; c_we = 0; c_cti = 0; c_sel = 0; c_adr = 0; c_dat = 0;
		r = 32'h1234_5678;
		for (i = 0; i < 19200; i = i + 1) begin
			r = r * 32'd1664525 + 32'd1013904223;
			pic[i] = r ^ (i << 7);
		end
		for (i = 0; i < 4096; i = i + 1) begin
			r = r * 32'd1664525 + 32'd1013904223;
			cref[i] = r;
		end
		repeat (20) @(posedge clk);
		rst = 0;
		wait (init_done);
		repeat (100) @(posedge clk);
		// the picture and the CPU's words, through the bus
		for (i = 0; i < PIC_LINES * 80; i = i + 1) cpu_cycle(1, {7'd0, PIC} + i * 4, pic[i], 4'hf, q);
		for (i = 0; i < 4096; i = i + 1) cpu_cycle(1, {7'd0, CPU_LO} + i * 4, cref[i], 4'hf, q);
		$display("written: %0d picture words, 4096 of the CPU's", PIC_LINES * 80);

		en = 1;
		w_on = 1;
		scan_on = 1;

		// the CPU, until the scanout has shown its lines
		while (lines_shown < LINES) begin
			r = r * 32'd1103515245 + 32'd12345;
			op = r[31:28] % 3;
			ci = r[13:2];
			a32 = {7'd0, CPU_LO} + ci * 4;
			ops = ops + 1;
			if (!CPU) begin
				@(posedge clk);
				ops = 0;
			end else if (op == 0) begin
				cpu_cycle(0, a32, 0, 4'hf, q);
				if (q !== cref[ci]) begin
					cbad = cbad + 1;
					if (cbad < 6) $display("FAIL: CPU read %h: %h, want %h", a32, q, cref[ci]);
				end
			end else if (op == 1) begin
				r = r * 32'd1103515245 + 32'd12345;
				d = r;
				sl = r[3:0] | 4'b0001;
				cpu_cycle(1, a32, d, sl, q);
				m = {{8{sl[3]}}, {8{sl[2]}}, {8{sl[1]}}, {8{sl[0]}}};
				cref[ci] = (cref[ci] & ~m) | (d & m);
			end else begin
				a32 = {a32[31:4], 4'h0};
				cpu_burst(a32);
				for (k = 0; k < 4; k = k + 1)
					if (lq[k] !== cref[a32[13:2] + k]) begin
						cbad = cbad + 1;
						if (cbad < 6) $display("FAIL: CPU line fill %h word %0d: %h, want %h",
							a32, k, lq[k], cref[a32[13:2] + k]);
					end
			end
		end
		w_on = 0;
		repeat (200) @(posedge clk);

		// the writer's words, read back by the CPU
		for (i = 0; i < 64; i = i + 1) begin
			a32 = {7'd0, T2_LO} + i * 4;
			cpu_cycle(0, a32, 0, 4'hf, q);
			if (w_count > 4096 && q !== (a32 ^ 32'h5a5a_0000)) begin
				cbad = cbad + 1;
				if (cbad < 6) $display("FAIL: the writer's word %h: %h", a32, q);
			end
		end

		$display("lines shown %0d, checked %0d, wrong %0d; fetched %0d, late %0d",
			lines_shown, checked, bad, lines, misses);
		$display("CPU: %0d operations, %0d wrong; writer: %0d writes",
			ops, cbad, w_count);
		// each wrap from line PIC_LINES - 1 to 0 is one line late (the
		// fetch expected line PIC_LINES next): allowed
		if (bad == 0 && cbad == 0 && misses <= 2 + LINES / PIC_LINES)
			$display("RESULT: PASS");
		else
			$display("RESULT: FAIL");
		$finish;
	end

endmodule
