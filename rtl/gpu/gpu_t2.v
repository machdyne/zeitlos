/*
 * Zeitlos SOC GPU
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * gpu_t2.v -- T2, the triangle engine that runs the whole frame
 * (docs/zgl_t2.md; the stream's format and the records: zgl_t2.h).
 *
 * The CPU streams a frame into the command FIFO: compiled display
 * lists, state, and triangles and lines it set up itself. The geometry
 * unit -- zeitlos32g, a small RV32IM core running sw/t2fw -- reads the
 * stream, transforms, lights, culls, clips and sets up, and writes a
 * record for each triangle and line into the record list. At the
 * frame's END it starts the raster, which draws the frame strip by
 * strip (16 rows) from the records, a pixel a clock with edge
 * functions, into two strip buffers; the output side writes each strip
 * into chunky VRAM behind the beam (CTRL.VSYNC) and clears it behind.
 *
 * -- Registers (word offsets) --
 *
 *   0 CTRL    rw  [0] WAIT_BEAM  [1] VSYNC
 *   1 STATUS  r   [0] busy  [27:16] FIFO words free (of 2,048; FIFO_LOG2)
 *   2 CMD     w   a stream word; the write waits while the FIFO is full
 *   3 VBASE   rw  chunky VRAM's byte address (LINEAR: the picture's,
 *                 in main memory)
 *   4 STRIPS  r   strips written out since RESET
 *   5 RESET   w   the FIFO emptied, the core restarted, the raster and
 *                 output stopped
 *   7 ID      r   0x5A545232 ("ZTR2")
 *
 * -- The geometry unit's registers (zeitlos32g I/O, sw/t2fw/io.c) --
 *
 *   0x20000 IN      r  the next stream word (waits for one)
 *   0x20004 STATUS  r  [0] the raster is drawing (the record list is its)
 *   0x20008 RX      w  the frame's columns x0 | x1 << 16
 *   0x2000c RY      w  its rows y0 | y1 << 16
 *   0x20010 CLEAR   w  its background index
 *   0x20014 GO      w  raster the list's first n words
 *   0x30000-        w  the record list
 *
 * Timing: every block RAM's output is registered before it is used,
 * and every multiplier's inputs and products are registered; the engine is meant to stay off
 * the SOC's critical path (65 MHz or more out of context).
 *
 * Plain Verilog-2005: no declarations inside blocks, one driver a signal.
 */

module gpu_t2_buf #(
	parameter WORDS = 960
) (
	input             clk,
	input      [10:0] a_addr,
	output reg [31:0] a_col,
	output reg [63:0] a_dep,
	input      [10:0] b_addr,
	input      [3:0]  b_col_we,
	input      [31:0] b_col_d,
	input      [3:0]  b_dep_we,
	input      [63:0] b_dep_d
);
	reg [31:0] col [0:WORDS-1];
	reg [63:0] dep [0:WORDS-1];

	always @(posedge clk) begin
		a_col <= col[a_addr];
		a_dep <= dep[a_addr];
	end

	always @(posedge clk) begin
		if (b_col_we[0]) col[b_addr][7:0]   <= b_col_d[7:0];
		if (b_col_we[1]) col[b_addr][15:8]  <= b_col_d[15:8];
		if (b_col_we[2]) col[b_addr][23:16] <= b_col_d[23:16];
		if (b_col_we[3]) col[b_addr][31:24] <= b_col_d[31:24];
	end

	always @(posedge clk) begin
		if (b_dep_we[0]) dep[b_addr][15:0]  <= b_dep_d[15:0];
		if (b_dep_we[1]) dep[b_addr][31:16] <= b_dep_d[31:16];
		if (b_dep_we[2]) dep[b_addr][47:32] <= b_dep_d[47:32];
		if (b_dep_we[3]) dep[b_addr][63:48] <= b_dep_d[63:48];
	end
endmodule

module gpu_t2_wb #(
	parameter FW_HEX = "sw/t2fw/t2fw.hex",
	parameter LIST_WORDS = 5632,         // T2_RECS * T2_REC_WORDS (zgl_t2.h)
	// two strip buffers (one drawn while the other goes out), or one
	// (the raster waits while a strip goes out: about 100k cycles more
	// a frame, for 9 DP16KD and about 400 LUT4 less)
	parameter TWO_BUFS = 0,
	// a strip's rows: 12 makes a buffer 960 words, which block RAM holds
	// without the output multiplexers 1,280 words (16 rows) need
	parameter STRIP_H = 12,
	// 1: the picture is LINEAR -- 320 bytes a line, line y at VBASE +
	// 320y -- rather than chunky VRAM's four segments a line in rows of
	// their own. `VRAM_SDRAM (docs/profiles.md, "Lakritz"): the picture
	// is in main memory, which the scanout reads a line at a time
	// (rtl/gpu/gpu_ckfetch.v), and software draws in it as it is.
	parameter LINEAR = 0,
	// Posted writes: the output side's words go into a queue of
	// 2^POST_LOG2 (block RAM: 1,024 is three DP16KD) and the queue to
	// the bus, so the raster goes on while main memory takes its time
	// (`VRAM_SDRAM: a write to SDRAM is several cycles, and a strip is
	// 960 words). STATUS's busy holds until the queue is empty, so a
	// frame is out when it says so. 0: none, each word waits for its
	// ack (VRAM answers the next cycle).
	parameter POST_LOG2 = 0,
	// the stream FIFO: 2^FIFO_LOG2 words (11: 2,048, four DP16KD; 9:
	// 512, one -- the CPU then waits on a full FIFO sooner)
	parameter FIFO_LOG2 = 11,
	// Textures (docs/zgl_t2.md, "Textures"): a texture memory of
	// 2^TEX_LOG2 texels (13: 8 KB, four DP16KD), the raster's TTRI
	// records drawn perspective-correct (a reciprocal table, one
	// DP16KD, and two multipliers), and the geometry core's bigger
	// program (TEX_FW_HEX: 16 KB of code, 8 KB of data, four DP16KD
	// more). 0: none -- CAPS says so, and zgl draws in colours.
	parameter TEX_LOG2 = 0,
	parameter TEX_FW_HEX = "sw/t2fw/t2fw_tex.hex",
	parameter RECIP_HEX = "rtl/gpu/t2_recip.hex"
) (
	input             clk,
	input             rst,

	// Wishbone slave (CPU)
	input             wb_cyc_i,
	input             wb_stb_i,
	input             wb_we_i,
	input      [3:0]  wb_sel_i,
	input      [31:0] wb_adr_i,
	input      [31:0] wb_dat_i,
	output reg        wb_ack_o,
	output reg [31:0] wb_dat_o,

	// Wishbone master (VRAM, writes only)
	output reg        m_cyc_o,
	output reg        m_stb_o,
	output reg        m_we_o,
	output reg [3:0]  m_sel_o,
	output reg [31:0] m_adr_o,
	output reg [31:0] m_dat_o,
	input      [31:0] m_dat_i,
	input             m_ack_i,

	input      [8:0]  beam_y,
	input             vblank
);

	localparam [10:0] BUF_LAST = STRIP_H * 80 - 1;   // a strip buffer's last word
	localparam [7:0]  STRIP_H1 = STRIP_H - 1;

	// ------------------------------------------------------------------
	// registers
	// ------------------------------------------------------------------

	reg        wait_beam;
	reg        vsync;
	reg [31:0] vbase;
	reg [31:0] strips;
	reg        soft_rst;

	// ------------------------------------------------------------------
	// the stream FIFO
	// ------------------------------------------------------------------

	// 2,048 words: a frame of lists (gldemo's is about 2,200) goes in
	// while the geometry unit works through it, so the CPU waits on a
	// full FIFO as little as it can
	localparam integer F_N = 1 << FIFO_LOG2;
	reg [31:0] fifo [0:F_N-1];
	reg [FIFO_LOG2-1:0] f_wr, f_rd;
	reg [11:0] f_n;
	reg [31:0] f_q;
	wire       f_full  = (f_n == F_N);
	wire       f_empty = (f_n == 12'd0);
	wire       wb_cmd  = wb_cyc_i && wb_stb_i && wb_we_i && !wb_ack_o && (wb_adr_i[3:0] == 4'd2);
	wire       f_push  = wb_cmd && !f_full;
	reg        f_pop;

	always @(posedge clk) begin
		if (f_push) fifo[f_wr] <= wb_dat_i;
		f_q <= fifo[f_rd];
	end

	always @(posedge clk) begin
		if (rst || soft_rst) begin
			f_wr <= 0;
			f_rd <= 0;
			f_n <= 12'd0;
		end else begin
			if (f_push) f_wr <= f_wr + 1'b1;
			if (f_pop) f_rd <= f_rd + 1'b1;
			if (f_push && !f_pop) f_n <= f_n + 12'd1;
			else if (!f_push && f_pop) f_n <= f_n - 12'd1;
		end
	end

	// ------------------------------------------------------------------
	// the geometry unit
	// ------------------------------------------------------------------

	wire        g_rd, g_wr;
	wire [17:0] g_addr;
	wire [31:0] g_wdata;
	reg  [31:0] g_rdata;
	reg         g_ready;

	localparam TEX = (TEX_LOG2 != 0);

	zeitlos32g #(.IMEM_HEX(TEX ? TEX_FW_HEX : FW_HEX),
		.IMEM_WORDS(TEX ? 4096 : 3072), .DMEM_WORDS(TEX ? 2048 : 1024)) geom (
		.clk(clk), .rst(rst || soft_rst),
		.io_rd(g_rd), .io_wr(g_wr), .io_addr(g_addr), .io_wdata(g_wdata),
		.io_rdata(g_rdata), .io_ready(g_ready)
	);

	// the record list: the core writes it, the raster reads it
	reg [31:0] list [0:LIST_WORDS-1];
	reg [12:0] l_raddr;
	reg [31:0] l_q, l_qr;          // the RAM's word; registered before use
	always @(posedge clk) begin
		if (g_wr && g_addr[17:16] == 2'b11) list[g_addr[14:2]] <= g_wdata;
		l_q <= list[l_raddr];
		l_qr <= l_q;
	end

	// the frame the core hands the raster
	reg [8:0]  fr_x0, fr_x1;
	reg [7:0]  fr_y0, fr_y1;
	reg [7:0]  fr_clear;
	reg [12:0] fr_nw;             // the list's words
	reg        fr_go;             // a frame handed over (a cycle)
	reg        r_busy;            // the raster has the list

	// the core's I/O cycles: IN waits for a word; the rest answer at once
	reg        g_inwait;          // an IN is waiting for the FIFO
	reg        g_inpop;           // ...and has popped: f_q next cycle
	always @(posedge clk) begin
		if (rst || soft_rst) begin
			g_ready <= 1'b0;
			g_inwait <= 1'b0;
			g_inpop <= 1'b0;
			f_pop <= 1'b0;
			fr_go <= 1'b0;
		end else begin
			g_ready <= 1'b0;
			f_pop <= 1'b0;
			fr_go <= 1'b0;
			g_inpop <= 1'b0;
			if (g_rd) begin
				if (g_addr == 18'h20000) g_inwait <= 1'b1;
				else begin
					g_rdata <= {31'd0, r_busy || fr_go};
					g_ready <= 1'b1;
				end
			end
			if (g_inwait && !f_empty && !f_pop && !g_inpop) begin
				f_pop <= 1'b1;
				g_inpop <= 1'b1;
				g_inwait <= 1'b0;
			end
			// f_pop this cycle: f_rd moves at the edge, f_q (the word
			// at the old f_rd) is read at that same edge
			if (g_inpop) begin
				g_rdata <= f_q;
				g_ready <= 1'b1;
			end
			if (g_wr) begin
				g_ready <= 1'b1;
				case (g_addr)
				18'h20008: begin fr_x0 <= g_wdata[8:0]; fr_x1 <= g_wdata[24:16]; end
				18'h2000c: begin fr_y0 <= g_wdata[7:0]; fr_y1 <= g_wdata[23:16]; end
				18'h20010: fr_clear <= g_wdata[7:0];
				18'h20014: begin fr_nw <= g_wdata[12:0]; fr_go <= 1'b1; end
				default: ;
				endcase
			end
		end
	end

	// ------------------------------------------------------------------
	// strip buffers: two, one drawn while the other goes out
	// ------------------------------------------------------------------

	reg        d_sel;             // the buffer being drawn
	reg        o_sel;             // the buffer going out
	reg [1:0]  ready;             // drawn, waiting to go out
	reg [8:0]  bclr0, bclr1;      // what each is clear to (bit 8: unknown)

	reg  [10:0] dr_a_addr;
	reg  [10:0] dr_b_addr;
	reg  [3:0]  dr_col_we;
	reg  [31:0] dr_col_d;
	reg  [3:0]  dr_dep_we;
	reg  [63:0] dr_dep_d;
	reg  [10:0] ou_a_addr;
	reg  [10:0] ou_b_addr;
	reg  [3:0]  ou_col_we;
	reg  [31:0] ou_col_d;
	reg  [3:0]  ou_dep_we;
	reg  [63:0] ou_dep_d;

	wire        o_act;
	reg         o_clr_set;
	reg         o_clr_buf;
	wire [31:0] b0_col, b1_col;
	wire [63:0] b0_dep, b1_dep;
	wire o_on0 = (o_act && !o_sel) || (o_clr_set && !o_clr_buf);
	wire o_on1 = (o_act && o_sel) || (o_clr_set && o_clr_buf);

	gpu_t2_buf #(.WORDS(STRIP_H * 80)) buf0 (
		.clk(clk),
		.a_addr(o_on0 ? ou_a_addr : dr_a_addr),
		.a_col(b0_col), .a_dep(b0_dep),
		.b_addr(o_on0 ? ou_b_addr : dr_b_addr),
		.b_col_we(o_on0 ? ou_col_we : (d_sel ? 4'd0 : dr_col_we)),
		.b_col_d(o_on0 ? ou_col_d : dr_col_d),
		.b_dep_we(o_on0 ? ou_dep_we : (d_sel ? 4'd0 : dr_dep_we)),
		.b_dep_d(o_on0 ? ou_dep_d : dr_dep_d)
	);

	generate
	if (TWO_BUFS) begin : g_buf1
		gpu_t2_buf #(.WORDS(STRIP_H * 80)) buf1 (
			.clk(clk),
			.a_addr(o_on1 ? ou_a_addr : dr_a_addr),
			.a_col(b1_col), .a_dep(b1_dep),
			.b_addr(o_on1 ? ou_b_addr : dr_b_addr),
			.b_col_we(o_on1 ? ou_col_we : (d_sel ? dr_col_we : 4'd0)),
			.b_col_d(o_on1 ? ou_col_d : dr_col_d),
			.b_dep_we(o_on1 ? ou_dep_we : (d_sel ? dr_dep_we : 4'd0)),
			.b_dep_d(o_on1 ? ou_dep_d : dr_dep_d)
		);
	end else begin : g_nobuf1
		assign b1_col = 32'd0;
		assign b1_dep = 64'd0;
	end
	endgenerate

	wire [63:0] dr_dep_q = d_sel ? b1_dep : b0_dep;
	wire [31:0] ou_col_q = o_sel ? b1_col : b0_col;

	// ------------------------------------------------------------------
	// the raster: the strip's records, four pixels a clock
	// ------------------------------------------------------------------

	localparam [4:0] R_IDLE = 5'd0, R_BEGIN = 5'd1, R_CLR = 5'd2, R_SCAN = 5'd3,
	                 R_W0 = 5'd4, R_HDR = 5'd5, R_LOAD = 5'd6, R_TSET = 5'd7,
	                 R_TROW = 5'd8, R_TWORD = 5'd9, R_TNEXT = 5'd10, R_DRAIN = 5'd11,
	                 R_LSTEP = 5'd12, R_LPIX = 5'd13, R_LADV = 5'd14, R_END = 5'd15,
	                 R_NEXTS = 5'd16, R_SKIP = 5'd17, R_W1 = 5'd18, R_SKIP2 = 5'd19;

	reg [4:0]  rs;

	// the strip
	reg [7:0]  s_y0, s_y1;
	reg        s_first;
	reg [12:0] r_ptr;             // the record being looked at
	reg [4:0]  r_wi;              // its word being loaded
	reg [31:0] rw [0:20];         // its words (TRI 11, LINE 10, TTRI 21)
	reg [4:0]  r_len;

	// a triangle in the strip
	reg [7:0]  t_y, t_y1;         // the row, the last row
	reg [8:0]  t_x, t_bx0, t_bx1;
	reg [7:0]  t_ymid;
	reg        t_sr, t_dep;
	reg signed [39:0] f_l, f_1, f_2;          // the edges at the row's first word
	reg signed [39:0] fw_l, fw_s;             // ...at this pixel (the short one in use)
	reg signed [21:0] sdy_l, sdy_1, sdy_2;    // 16 dy: a pixel to the right
	reg signed [21:0] sdx_l, sdx_1, sdx_2;    // 16 dx: a row down
	reg [31:0] t_dzdx, t_dzdy, t_dcdx, t_dcdy;
	reg [31:0] zrow, crow, zw, cw;            // at the row's first pixel (bx0); at this pixel

	// the record's corners, sign-extended to 18 bits (28.4)
	wire [17:0] v0x = {{2{rw[2][15]}}, rw[2][15:0]}, v0y = {{2{rw[2][31]}}, rw[2][31:16]};
	wire [17:0] v1x = {{2{rw[3][15]}}, rw[3][15:0]}, v1y = {{2{rw[3][31]}}, rw[3][31:16]};
	wire [17:0] v2x = {{2{rw[4][15]}}, rw[4][15:0]}, v2y = {{2{rw[4][31]}}, rw[4][31:16]};
	wire [17:0] t_x0 = {5'd0, t_bx0, 4'd0};        // the first pixel's x, 28.4
	wire [17:0] t_ys = {6'd0, t_y, 4'd0};          // the row's y, 28.4

	// the setup's shared subtractor: X0 - a.x and Y0 - a.y of each edge in turn
	reg  [17:0] su_a, su_b;
	always @(*) begin
		case (t_k)
		5'd1: begin su_a = t_x0; su_b = v0x; end
		5'd2: begin su_a = t_ys; su_b = v0y; end
		5'd3: begin su_a = t_x0; su_b = v0x; end
		5'd4: begin su_a = t_ys; su_b = v0y; end
		5'd5: begin su_a = t_x0; su_b = v1x; end
		default: begin su_a = t_ys; su_b = v1y; end
		endcase
	end
	wire [17:0] su_d = su_a - su_b;
	wire [7:0]  t_dyr = t_y - rw[0][15:8];      // the strip's first row, from ylo

	// the setup's multiplier: 18x18 signed, inputs and product registered
	reg signed [17:0] mu_a, mu_b;
	reg signed [35:0] mu_p;
	reg [4:0]  t_k;               // the setup's step
	reg signed [35:0] t_acc;
	always @(posedge clk) mu_p <= mu_a * mu_b;
	// an edge: the two products' difference; a plane: z0 (or c0) + the
	// low 32 bits of dz/dy (or dc/dy) * rows, from its two 16-bit halves
	wire [39:0] f_new  = {{4{t_acc[35]}}, t_acc} - {{4{mu_p[35]}}, mu_p};
	// the plane at the strip's first row: z (step 10), ci (12), and a
	// textured triangle's A, B and Q (14, 16, 18)
	// (without textures only z and ci: the tools cannot see that t_k
	// never reaches 14)
	reg  [31:0] pl_base;
	always @(*) begin
		case (t_k)
		5'd10: pl_base = rw[5];
		5'd12: pl_base = rw[8];
		5'd14: pl_base = TEX ? rw[12] : rw[8];
		5'd16: pl_base = TEX ? rw[15] : rw[8];
		default: pl_base = TEX ? rw[18] : rw[8];
		endcase
	end
	wire [31:0] pl_new = pl_base + t_acc[31:0] + {mu_p[15:0], 16'd0};

	// a line in the strip
	reg [31:0] ln_n, ln_i;
	reg [31:0] ln_x, ln_y, ln_dx, ln_dy, ln_z, ln_dz, ln_c, ln_dc;
	reg [1:0]  lwait;
	wire [31:0] ln_xr = ln_x + 32'h00080000;
	wire [31:0] ln_yr = ln_y + 32'h00080000;
	wire signed [11:0] ln_pxs = ln_xr[31:20];
	wire signed [11:0] ln_pys = ln_yr[31:20];
	wire        ln_in = (ln_pys >= $signed({4'd0, s_y0})) && (ln_pys <= $signed({4'd0, s_y1})) &&
	                    (ln_pxs >= $signed({3'd0, fr_x0})) && (ln_pxs <= $signed({3'd0, fr_x1}));
	wire [3:0]  ln_row = ln_pys[3:0] - s_y0[3:0];
	wire [10:0] ln_addr = {ln_row, 6'd0} + {2'd0, ln_row, 4'd0} + {4'd0, ln_pxs[8:2]};
	wire [31:0] ln_cr = ln_c + 32'd128;

	// the pixel: covered (zgl's top-left rule: short on the right, the
	// long edge's pixels in (>= 0), the short one's out (< 0)), its
	// depth and its dithered index. A pixel a clock: the four lanes of a
	// word would take a quarter of the cycles, and too much of a 45F.
	wire signed [21:0] sdy_s = (t_y < t_ymid) ? sdy_1 : sdy_2;
	wire signed [39:0] sdl = sdy_l, sds = sdy_s;
	wire        px_cover = t_sr ? (!fw_l[39] && fw_s[39]) : (!fw_s[39] && fw_l[39]);
	reg  [7:0]  th;               // bayer4[y & 3][x & 3]
	always @(*) begin
		case ({t_y[1:0], t_x[1:0]})
		4'h0: th = 8'd8;   4'h1: th = 8'd136; 4'h2: th = 8'd40;  4'h3: th = 8'd168;
		4'h4: th = 8'd200; 4'h5: th = 8'd72;  4'h6: th = 8'd232; 4'h7: th = 8'd104;
		4'h8: th = 8'd56;  4'h9: th = 8'd184; 4'ha: th = 8'd24;  4'hb: th = 8'd152;
		4'hc: th = 8'd248; 4'hd: th = 8'd120; 4'he: th = 8'd216; default: th = 8'd88;
		endcase
	end
	wire [31:0] px_ci = cw + {24'd0, th};
	wire [15:0] px_zv = zw[31] ? 16'd0 : zw[30:15];
	wire [3:0]  px_row = t_y[3:0] - s_y0[3:0];
	wire [10:0] px_addr = {px_row, 6'd0} + {2'd0, px_row, 4'd0} + {4'd0, t_x[8:2]};

	// the pixel pipe: p0 asked (the read address is out), p1 the word
	// comes out of the RAM, p2 it is registered, then the test and the
	// write (dr_b_*), which the RAM takes a cycle later
	reg        p0_v, p1_v, p2_v;
	reg [10:0] p0_addr, p1_addr, p2_addr;
	reg [3:0]  p0_m, p1_m, p2_m;      // lanes to draw
	reg        p0_dep, p1_dep, p2_dep;
	reg        p0_le, p1_le, p2_le;   // a line's test: <=
	reg [63:0] p0_zv, p1_zv, p2_zv;
	reg [31:0] p0_ci, p1_ci, p2_ci;
	reg [63:0] p2_old;
	wire [3:0] pass;
	assign pass[0] = p2_m[0] && (!p2_dep || (p2_le ? p2_zv[15:0]  <= p2_old[15:0]  : p2_zv[15:0]  < p2_old[15:0]));
	assign pass[1] = p2_m[1] && (!p2_dep || (p2_le ? p2_zv[31:16] <= p2_old[31:16] : p2_zv[31:16] < p2_old[31:16]));
	assign pass[2] = p2_m[2] && (!p2_dep || (p2_le ? p2_zv[47:32] <= p2_old[47:32] : p2_zv[47:32] < p2_old[47:32]));
	assign pass[3] = p2_m[3] && (!p2_dep || (p2_le ? p2_zv[63:48] <= p2_old[63:48] : p2_zv[63:48] < p2_old[63:48]));
	wire       tx_busy;               // the texture pipe (below)
	wire       pipe_busy = p0_v || p1_v || p2_v || (dr_col_we != 4'd0) || tx_busy;

	// ------------------------------------------------------------------
	// textures (TEX_LOG2): a TTRI's planes, the texture memory and the
	// texture pipe -- t2_uv(), t2_texel_at() and t2_combine() in
	// sw/common/zgl/zgl_t2.h, bit for bit
	// ------------------------------------------------------------------

	reg        t_tx;                      // the triangle is textured
	reg [31:0] t_tw;                      // ...its texture word
	reg [31:0] t_dadx, t_dady, t_dbdx, t_dbdy, t_dqdx, t_dqdy;
	reg [31:0] arow, brow, qrow;          // A, B, Q at the row's first pixel
	reg [31:0] aw, bw, qw;                // ...at this pixel

	localparam integer TX_LOG2 = TEX ? TEX_LOG2 : 4;
	localparam integer TX_W = 1 << (TX_LOG2 - 2);
	localparam [4:0]   TX_LOG2_5 = TEX ? TEX_LOG2 : 0;
	reg  [31:0] texm [0:TX_W-1];          // four texels a word, the first in 7:0
	reg  [31:0] tex_q;
	reg  [TX_LOG2-3:0] tex_wa;            // TEXADDR
	wire        tex_we = TEX && wb_cyc_i && wb_stb_i && wb_we_i && !wb_ack_o && (wb_adr_i[3:0] == 4'd13);

	// 1 / (1 + f) for f's first ten bits (rtl/gpu/gen_t2_recip.py)
	reg  [16:0] rcp [0:1023];
	reg  [16:0] rcp_q;
	initial $readmemh(RECIP_HEX, rcp);

	// the stages: x1 the pixel as R_TWORD found it; x2 Q clamped, its
	// leading bit, A's and B's shifts; x3 the table's index, A and B in
	// 18 bits; x4 the table's word; x5 the products; x6 s and t (12.4,
	// the low 12 bits); x7 the texel's place; x8 its word: the texel,
	// the pixel's index, and into p0
	reg        x1_v, x2_v, x3_v, x4_v, x5_v, x6_v, x7_v, x8_v;
	reg [10:0] x1_addr, x2_addr, x3_addr, x4_addr, x5_addr, x6_addr, x7_addr, x8_addr;
	reg [3:0]  x1_m, x2_m, x3_m, x4_m, x5_m, x6_m, x7_m, x8_m;
	reg        x1_dep, x2_dep, x3_dep, x4_dep, x5_dep, x6_dep, x7_dep, x8_dep;
	reg [15:0] x1_zv, x2_zv, x3_zv, x4_zv, x5_zv, x6_zv, x7_zv, x8_zv;
	reg [7:0]  x1_ci, x2_ci, x3_ci, x4_ci, x5_ci, x6_ci, x7_ci, x8_ci;
	reg [31:0] x1_a, x1_b, x1_q;
	reg [31:0] x2_q, x2_a, x2_b;
	reg [4:0]  x2_e;
	reg [3:0]  x2_sa, x2_sb;
	reg [9:0]  x3_idx;
	reg [17:0] x3_an, x3_bn, x4_an, x4_bn;
	reg [5:0]  x3_sha, x3_shb, x4_sha, x4_shb, x5_sha, x5_shb;
	reg signed [35:0] x5_pa, x5_pb;
	reg [11:0] x6_s, x6_t;
	reg [TX_LOG2-1:0] x7_ta;
	reg [1:0]  x8_bs;
	wire       tx_busy_w = x1_v || x2_v || x3_v || x4_v || x5_v || x6_v || x7_v || x8_v;
	assign     tx_busy = TEX && tx_busy_w;

	// x1: Q at least 32; its leading bit; A's and B's significant bits
	wire [31:0] x1_qc = ($signed(x1_q) < 32) ? 32'd32 : x1_q;
	wire [31:0] x1_aa = x1_a[31] ? ~x1_a : x1_a;
	wire [31:0] x1_ba = x1_b[31] ? ~x1_b : x1_b;
	reg  [4:0]  xe_q, xm_a, xm_b;
	reg  [5:0]  xl_a, xl_b;
	reg  [3:0]  xs_a, xs_b;
	integer     xi_q, xi_a, xi_b;
	always @(*) begin
		xe_q = 5'd0;
		for (xi_q = 0; xi_q < 32; xi_q = xi_q + 1) if (x1_qc[xi_q]) xe_q = xi_q;
	end
	always @(*) begin
		xm_a = 5'd0;
		for (xi_a = 0; xi_a < 32; xi_a = xi_a + 1) if (x1_aa[xi_a]) xm_a = xi_a;
		xl_a = (x1_aa == 32'd0) ? 6'd0 : {1'b0, xm_a} + 6'd1;
		xs_a = (xl_a > 6'd17) ? xl_a[3:0] - 4'd1 : 4'd0;   // bl - 17 (bl 18..31)
	end
	always @(*) begin
		xm_b = 5'd0;
		for (xi_b = 0; xi_b < 32; xi_b = xi_b + 1) if (x1_ba[xi_b]) xm_b = xi_b;
		xl_b = (x1_ba == 32'd0) ? 6'd0 : {1'b0, xm_b} + 6'd1;
		xs_b = (xl_b > 6'd17) ? xl_b[3:0] - 4'd1 : 4'd0;
	end

	// x2: the table's index, A and B brought to 18 bits
	wire [31:0] x2_qn  = x2_q << (5'd31 - x2_e);
	wire [9:0]  x2_idx = x2_qn[30:21];
	wire [31:0] x2_as  = $signed(x2_a) >>> x2_sa;
	wire [31:0] x2_bs  = $signed(x2_b) >>> x2_sb;
	wire [17:0] x2_an  = x2_as[17:0];
	wire [17:0] x2_bn  = x2_bs[17:0];

	// x4 -> x5: the table's word and the products (inputs and products
	// registered)
	always @(posedge clk) begin
		rcp_q <= rcp[x3_idx];
		x5_pa <= $signed(x4_an) * $signed({1'b0, rcp_q});
		x5_pb <= $signed(x4_bn) * $signed({1'b0, rcp_q});
	end

	// x5: s and t, 12.4, their low 12 bits
	wire [47:0] x5_pae = {{12{x5_pa[35]}}, x5_pa};
	wire [47:0] x5_pbe = {{12{x5_pb[35]}}, x5_pb};
	wire [47:0] x5_sx  = $signed(x5_pae) >>> x5_sha;
	wire [47:0] x5_tx  = $signed(x5_pbe) >>> x5_shb;
	wire [11:0] x5_s   = x5_sx[11:0];
	wire [11:0] x5_t   = x5_tx[11:0];

	// x6: the texel's place, (base + v << lw + u) mod the memory
	wire [2:0]  tw_lw = t_tw[18:16], tw_lh = t_tw[21:19];
	wire [6:0]  x6_u  = x6_s[10:4] & ((7'd1 << tw_lw) - 7'd1);
	wire [6:0]  x6_vv = x6_t[10:4] & ((7'd1 << tw_lh) - 7'd1);
	wire [15:0] x6_vo = {9'd0, x6_vv} << tw_lw;
	wire [15:0] x6_tf = t_tw[15:0] + x6_vo + {9'd0, x6_u};
	wire [TX_LOG2-1:0] x6_ta = x6_tf[TX_LOG2-1:0];

	// x7 -> x8: the texel's word
	always @(posedge clk) begin
		if (tex_we) texm[tex_wa] <= wb_dat_i;
		tex_q <= texm[x7_ta[TX_LOG2-1:2]];
	end

	// x8: the texel, the key, and the mode's index (T2_TM_*)
	reg  [7:0]  x8_tx;
	always @(*) begin
		case (x8_bs)
		2'd0: x8_tx = tex_q[7:0];
		2'd1: x8_tx = tex_q[15:8];
		2'd2: x8_tx = tex_q[23:16];
		default: x8_tx = tex_q[31:24];
		endcase
	end
	wire [8:0]  x8_tm   = {5'd0, x8_tx[3:0]} * ({5'd0, x8_ci[3:0]} + 9'd1);
	wire [3:0]  x8_tone = x8_tm[7:4];
	wire        x8_kill = t_tw[24] && (x8_tx == 8'd0);
	reg  [7:0]  x8_px;
	always @(*) begin
		case (t_tw[23:22])
		2'd0: x8_px = x8_tx;
		2'd1: x8_px = x8_tx + x8_ci;
		2'd2: x8_px = {x8_ci[7:4], x8_tone};
		default: x8_px = {x8_tx[7:4], x8_tone};
		endcase
	end

	// is the buffer to be drawn free, and clear to this frame's colour?
	reg        o_busy;
	reg [7:0]  o_clear;
	wire       d_busy = ready[d_sel] || (o_busy && (o_sel == d_sel)) || o_clr_set;
	wire [8:0] d_clr  = d_sel ? bclr1 : bclr0;
	reg [10:0] clr_w;

	// the strip handed to the output
	reg [7:0]  h_y0;
	reg [3:0]  h_rows1;
	reg [8:0]  h_cx0, h_cx1;
	reg [7:0]  h_clear;
	reg        h_first;
	reg        h_go;

	// the record's header (rw[0]): its rows, against the strip's
	wire [7:0] rh_ylo = l_qr[15:8], rh_yhi = l_qr[23:16];
	wire       rh_ttri = TEX && (l_qr[1:0] == 2'd3);
	wire       rh_tri = (l_qr[1:0] == 2'd1) || rh_ttri;
	wire [4:0] rh_len = rh_ttri ? 5'd21 : rh_tri ? 5'd11 : 5'd10;

	always @(posedge clk) begin
		if (rst || soft_rst) begin
			rs <= R_IDLE;
			r_busy <= 1'b0;
			d_sel <= 1'b0;
			p0_v <= 1'b0; p1_v <= 1'b0; p2_v <= 1'b0;
			x1_v <= 1'b0; x2_v <= 1'b0; x3_v <= 1'b0; x4_v <= 1'b0;
			x5_v <= 1'b0; x6_v <= 1'b0; x7_v <= 1'b0; x8_v <= 1'b0;
			h_go <= 1'b0;
			bclr0 <= 9'h100;
			bclr1 <= 9'h100;
			dr_col_we <= 4'd0;
			dr_dep_we <= 4'd0;
		end else begin
			h_go <= 1'b0;
			dr_col_we <= 4'd0;
			dr_dep_we <= 4'd0;

			if (o_clr_set) begin
				if (o_clr_buf) bclr1 <= {1'b0, o_clear};
				else bclr0 <= {1'b0, o_clear};
			end

			// the pipe: p2's test and write, p1 -> p2, p0 -> p1
			if (p2_v) begin
				dr_b_addr <= p2_addr;
				dr_col_we <= pass;
				dr_col_d <= p2_ci;
				dr_dep_we <= p2_dep ? pass : 4'd0;
				dr_dep_d <= p2_zv;
			end
			p2_v <= p1_v; p2_addr <= p1_addr; p2_m <= p1_m; p2_dep <= p1_dep; p2_le <= p1_le;
			p2_zv <= p1_zv; p2_ci <= p1_ci; p2_old <= dr_dep_q;
			p1_v <= p0_v; p1_addr <= p0_addr; p1_m <= p0_m; p1_dep <= p0_dep; p1_le <= p0_le;
			p1_zv <= p0_zv; p1_ci <= p0_ci;
			p0_v <= 1'b0;

			// the texture pipe: a textured pixel's texel, eight stages,
			// into p0 as an untextured pixel goes (t2_uv(), zgl_t2.h)
			x1_v <= 1'b0;
			x2_v <= x1_v && TEX;
			x2_addr <= x1_addr; x2_m <= x1_m; x2_dep <= x1_dep; x2_zv <= x1_zv; x2_ci <= x1_ci;
			x2_q <= x1_qc; x2_e <= xe_q; x2_a <= x1_a; x2_b <= x1_b; x2_sa <= xs_a; x2_sb <= xs_b;
			x3_v <= x2_v;
			x3_addr <= x2_addr; x3_m <= x2_m; x3_dep <= x2_dep; x3_zv <= x2_zv; x3_ci <= x2_ci;
			x3_idx <= x2_idx; x3_an <= x2_an; x3_bn <= x2_bn;
			x3_sha <= 6'd9 + {1'b0, x2_e} - {2'd0, x2_sa};
			x3_shb <= 6'd9 + {1'b0, x2_e} - {2'd0, x2_sb};
			x4_v <= x3_v;
			x4_addr <= x3_addr; x4_m <= x3_m; x4_dep <= x3_dep; x4_zv <= x3_zv; x4_ci <= x3_ci;
			x4_an <= x3_an; x4_bn <= x3_bn; x4_sha <= x3_sha; x4_shb <= x3_shb;
			x5_v <= x4_v;
			x5_addr <= x4_addr; x5_m <= x4_m; x5_dep <= x4_dep; x5_zv <= x4_zv; x5_ci <= x4_ci;
			x5_sha <= x4_sha; x5_shb <= x4_shb;
			x6_v <= x5_v;
			x6_addr <= x5_addr; x6_m <= x5_m; x6_dep <= x5_dep; x6_zv <= x5_zv; x6_ci <= x5_ci;
			x6_s <= x5_s; x6_t <= x5_t;
			x7_v <= x6_v;
			x7_addr <= x6_addr; x7_m <= x6_m; x7_dep <= x6_dep; x7_zv <= x6_zv; x7_ci <= x6_ci;
			x7_ta <= x6_ta;
			x8_v <= x7_v;
			x8_addr <= x7_addr; x8_m <= x7_m; x8_dep <= x7_dep; x8_zv <= x7_zv; x8_ci <= x7_ci;
			x8_bs <= x7_ta[1:0];
			if (x8_v && !x8_kill) begin
				dr_a_addr <= x8_addr;
				p0_v <= 1'b1;
				p0_addr <= x8_addr;
				p0_m <= x8_m;
				p0_dep <= x8_dep;
				p0_le <= 1'b0;
				p0_zv <= {4{x8_zv}};
				p0_ci <= {4{x8_px}};
			end

			case (rs)
			R_IDLE: begin
				if (fr_go) begin
					r_busy <= 1'b1;
					s_y0 <= fr_y0;
					s_first <= 1'b1;
					rs <= R_BEGIN;
				end
			end

			// a strip: its buffer back from going out, clear to the colour
			R_BEGIN: begin
				s_y1 <= (fr_y1 < s_y0 + STRIP_H1) ? fr_y1 : s_y0 + STRIP_H1;
				if (!d_busy) begin
					if (d_clr != {1'b0, fr_clear}) begin
						clr_w <= 11'd0;
						rs <= R_CLR;
					end else begin
						r_ptr <= 13'd0;
						rs <= R_SCAN;
					end
				end
			end

			R_CLR: begin
				dr_b_addr <= clr_w;
				dr_col_we <= 4'b1111;
				dr_col_d <= {4{fr_clear}};
				dr_dep_we <= 4'b1111;
				dr_dep_d <= {64{1'b1}};
				clr_w <= clr_w + 11'd1;
				if (clr_w == BUF_LAST) begin
					if (d_sel) bclr1 <= {1'b0, fr_clear};
					else bclr0 <= {1'b0, fr_clear};
					r_ptr <= 13'd0;
					rs <= R_SCAN;
				end
			end

			// the next record's header: does it touch the strip?
			R_SCAN: begin
				if (r_ptr >= fr_nw) rs <= R_END;
				else begin
					l_raddr <= r_ptr;
					rs <= R_W0;
				end
			end
			R_W0: rs <= R_W1;                   // the address is in
			R_W1: rs <= R_HDR;                  // the word is out
			R_HDR: begin                        // l_qr: the header
				r_len <= rh_len;
				if (rh_yhi < s_y0 || rh_ylo > s_y1) begin
					r_ptr <= r_ptr + {8'd0, rh_len};
					rs <= R_SCAN;
				end else begin
					rw[0] <= l_qr;
					r_wi <= 5'd1;
					l_raddr <= r_ptr + 13'd1;
					rs <= R_SKIP;
				end
			end
			R_SKIP: begin                       // the second word's address is in
				l_raddr <= l_raddr + 13'd1;
				rs <= R_SKIP2;
			end
			R_SKIP2: begin                      // ...and its word out
				l_raddr <= l_raddr + 13'd1;
				rs <= R_LOAD;
			end
			R_LOAD: begin
				// words 11..20 only a TTRI has: none without textures
				if (TEX || r_wi < 5'd11) rw[r_wi] <= l_qr;
				l_raddr <= l_raddr + 13'd1;
				r_wi <= r_wi + 5'd1;
				if (r_wi == r_len - 5'd1) begin
					r_ptr <= r_ptr + {8'd0, r_len};
					t_k <= 5'd0;
					if (rw[0][1:0] == 2'd1 || (TEX && rw[0][1:0] == 2'd3)) rs <= R_TSET;
					else begin
						ln_n <= rw[1]; ln_i <= 32'd0;
						ln_x <= rw[2]; ln_y <= rw[3]; ln_dx <= rw[4]; ln_dy <= rw[5];
						ln_z <= rw[6]; ln_dz <= rw[7]; ln_c <= rw[8]; ln_dc <= l_qr;
						rs <= R_LSTEP;
					end
				end
			end

			// --- a triangle: set up for the strip, a product at a time ---
			R_TSET: begin
				t_k <= t_k + 5'd1;
				case (t_k)
				5'd0: begin
					t_sr <= rw[0][3];
					t_dep <= rw[0][2];
					t_ymid <= rw[0][31:24];
					t_y <= (rw[0][15:8] > s_y0) ? rw[0][15:8] : s_y0;
					t_y1 <= (rw[0][23:16] < s_y1) ? rw[0][23:16] : s_y1;
					t_bx0 <= rw[1][8:0];
					t_bx1 <= rw[1][24:16];
					t_dzdx <= rw[6]; t_dzdy <= rw[7];
					t_dcdx <= rw[9]; t_dcdy <= rw[10];
					t_tx <= TEX && (rw[0][1:0] == 2'd3);
					t_tw <= rw[11];
					t_dadx <= rw[13]; t_dady <= rw[14];
					t_dbdx <= rw[16]; t_dbdy <= rw[17];
					t_dqdx <= rw[19]; t_dqdy <= rw[20];
					// 16 dy and 16 dx of each edge (v0 v2, v0 v1, v1 v2)
					sdy_l <= ($signed(rw[4][31:16]) - $signed(rw[2][31:16])) * 16;
					sdy_1 <= ($signed(rw[3][31:16]) - $signed(rw[2][31:16])) * 16;
					sdy_2 <= ($signed(rw[4][31:16]) - $signed(rw[3][31:16])) * 16;
					sdx_l <= ($signed(rw[4][15:0]) - $signed(rw[2][15:0])) * 16;
					sdx_1 <= ($signed(rw[3][15:0]) - $signed(rw[2][15:0])) * 16;
					sdx_2 <= ($signed(rw[4][15:0]) - $signed(rw[3][15:0])) * 16;
				end
				// each edge at the strip's first row and the first word:
				// F = (X0 - a.x) dy - (Y0 - a.y) dx, two products; the
				// row's planes z0 + dz/dy * (row - ylo), the low 32 bits
				// from two products each
				// (one subtractor and one adder, shared by the steps)
				5'd1: begin mu_a <= su_d; mu_b <= sdy_l >>> 4; end
				5'd2: begin mu_a <= su_d; mu_b <= sdx_l >>> 4; end
				5'd3: begin mu_a <= su_d; mu_b <= sdy_1 >>> 4; t_acc <= mu_p; end
				5'd4: begin mu_a <= su_d; mu_b <= sdx_1 >>> 4; f_l <= f_new; end
				5'd5: begin mu_a <= su_d; mu_b <= sdy_2 >>> 4; t_acc <= mu_p; end
				5'd6: begin mu_a <= su_d; mu_b <= sdx_2 >>> 4; f_1 <= f_new; end
				5'd7: begin mu_a <= {2'd0, rw[7][15:0]}; mu_b <= {10'd0, t_dyr}; t_acc <= mu_p; end
				5'd8: begin mu_a <= {2'd0, rw[7][31:16]}; mu_b <= {10'd0, t_dyr}; f_2 <= f_new; end
				5'd9: begin mu_a <= {2'd0, rw[10][15:0]}; mu_b <= {10'd0, t_dyr}; t_acc <= mu_p; end
				5'd10: begin mu_a <= {2'd0, rw[10][31:16]}; mu_b <= {10'd0, t_dyr}; zrow <= pl_new; end
				5'd11: begin t_acc <= mu_p; mu_a <= {2'd0, rw[14][15:0]}; mu_b <= {10'd0, t_dyr}; end
				5'd12: begin
					crow <= pl_new;
					mu_a <= {2'd0, rw[14][31:16]}; mu_b <= {10'd0, t_dyr};
					if (!t_tx) rs <= R_TROW;
				end
				// a textured triangle: A, B and Q at the strip's first row
				5'd13: begin t_acc <= mu_p; mu_a <= {2'd0, rw[17][15:0]}; mu_b <= {10'd0, t_dyr}; end
				5'd14: begin arow <= pl_new; mu_a <= {2'd0, rw[17][31:16]}; mu_b <= {10'd0, t_dyr}; end
				5'd15: begin t_acc <= mu_p; mu_a <= {2'd0, rw[20][15:0]}; mu_b <= {10'd0, t_dyr}; end
				5'd16: begin brow <= pl_new; mu_a <= {2'd0, rw[20][31:16]}; mu_b <= {10'd0, t_dyr}; end
				5'd17: t_acc <= mu_p;
				5'd18: begin
					qrow <= pl_new;
					rs <= R_TROW;
				end
				default: ;
				endcase
			end

			// a row: the edges and planes at its first pixel
			R_TROW: begin
				t_x <= t_bx0;
				fw_l <= f_l;
				fw_s <= (t_y < t_ymid) ? f_1 : f_2;
				zw <= zrow;
				cw <= crow;
				aw <= arow; bw <= brow; qw <= qrow;
				rs <= R_TWORD;
			end

			// a pixel into the pipe if covered, then the next
			R_TWORD: begin
				if (px_cover && t_tx) begin
					// textured: through the texture pipe first (below)
					x1_v <= 1'b1;
					x1_addr <= px_addr;
					x1_m <= 4'b0001 << t_x[1:0];
					x1_dep <= t_dep;
					x1_zv <= px_zv;
					x1_ci <= px_ci[15:8];
					x1_a <= aw; x1_b <= bw; x1_q <= qw;
				end else if (px_cover) begin
					dr_a_addr <= px_addr;
					p0_v <= 1'b1;
					p0_addr <= px_addr;
					p0_m <= 4'b0001 << t_x[1:0];
					p0_dep <= t_dep;
					p0_le <= 1'b0;
					p0_zv <= {4{px_zv}};
					p0_ci <= {4{px_ci[15:8]}};
				end
				fw_l <= fw_l + sdl;
				fw_s <= fw_s + sds;
				zw <= zw + t_dzdx;
				cw <= cw + t_dcdx;
				aw <= aw + t_dadx; bw <= bw + t_dbdx; qw <= qw + t_dqdx;
				t_x <= t_x + 9'd1;
				if (t_x == t_bx1) rs <= R_TNEXT;
			end

			R_TNEXT: begin
				f_l <= f_l - sdx_l;
				f_1 <= f_1 - sdx_1;
				f_2 <= f_2 - sdx_2;
				zrow <= zrow + t_dzdy;
				crow <= crow + t_dcdy;
				arow <= arow + t_dady; brow <= brow + t_dbdy; qrow <= qrow + t_dqdy;
				t_y <= t_y + 8'd1;
				if (t_y == t_y1) rs <= R_DRAIN;
				else rs <= R_TROW;
			end

			// the last pixel through the pipe before the next record's
			// first can read a word it wrote
			R_DRAIN: if (!pipe_busy) rs <= R_SCAN;

			// --- a line: every step, those in the strip drawn ---
			R_LSTEP: begin
				if (ln_in) begin
					dr_a_addr <= ln_addr;
					p0_v <= 1'b1;
					p0_addr <= ln_addr;
					p0_m <= 4'b0001 << ln_pxs[1:0];
					p0_dep <= rw[0][2];
					p0_le <= 1'b1;
					p0_zv <= {4{ln_z[31] ? 16'd0 : ln_z[30:15]}};
					p0_ci <= {4{ln_cr[15:8]}};
					lwait <= 2'd3;
					rs <= R_LPIX;
				end else rs <= R_LADV;
			end
			R_LPIX: begin
				// the next step can land on the pixel just written
				lwait <= lwait - 2'd1;
				if (lwait == 2'd0) rs <= R_LADV;
			end
			R_LADV: begin
				ln_x <= ln_x + ln_dx;
				ln_y <= ln_y + ln_dy;
				ln_z <= ln_z + ln_dz;
				ln_c <= ln_c + ln_dc;
				ln_i <= ln_i + 32'd1;
				if (ln_i == ln_n) rs <= R_DRAIN;
				else rs <= R_LSTEP;
			end

			// the strip is drawn: out it goes; the next
			R_END: if (!pipe_busy) begin
				h_y0 <= s_y0; h_rows1 <= s_y1[3:0] - s_y0[3:0];
				h_cx0 <= fr_x0; h_cx1 <= fr_x1;
				h_clear <= fr_clear;
				h_first <= s_first;
				h_go <= 1'b1;
				if (TWO_BUFS) d_sel <= ~d_sel;
				rs <= R_NEXTS;
			end
			R_NEXTS: begin
				s_first <= 1'b0;
				if (s_y1 >= fr_y1) begin
					r_busy <= 1'b0;
					rs <= R_IDLE;
				end else begin
					s_y0 <= s_y1 + 8'd1;
					rs <= R_BEGIN;
				end
			end

			default: rs <= R_IDLE;
			endcase
		end
	end

	// ------------------------------------------------------------------
	// the output side: a drawn strip to chunky VRAM, clearing behind
	// (chunky VRAM, or main memory when LINEAR)
	// ------------------------------------------------------------------

	localparam [2:0] O_IDLE = 3'd0, O_BEAM = 3'd1, O_READ = 3'd2, O_DATA = 3'd3,
	                 O_WRITE = 3'd4, O_NEXT = 3'd5, O_DONE = 3'd6;

	reg [2:0]  ost;
	assign o_act = (ost != O_IDLE);
	reg [7:0]  o_y0;
	reg        o_first;
	reg [3:0]  o_rows1;
	reg [8:0]  o_cx0, o_cx1;
	reg [3:0]  o_row;
	reg [1:0]  o_seg;
	reg [4:0]  o_i;
	reg [10:0] o_w;

	// (a table, not a multiply: a multiplier's output into the compares
	// below was the slowest path out of context)
	reg  [8:0] o_segx;
	always @(*) begin
		case (o_seg)
		2'd0: o_segx = 9'd0;
		2'd1: o_segx = 9'd80;
		2'd2: o_segx = 9'd160;
		default: o_segx = 9'd240;
		endcase
	end
	wire [8:0] o_xb = o_segx + {2'd0, o_i, 2'd0};
	reg  [3:0] o_in_q;            // registered in O_READ for O_WRITE
	reg  [14:0] o_vword_q;
	reg        o_rowin_q;
	wire [3:0] o_in;
	assign o_in[0] = (o_xb       >= o_cx0) && (o_xb       <= o_cx1);
	assign o_in[1] = (o_xb + 9'd1 >= o_cx0) && (o_xb + 9'd1 <= o_cx1);
	assign o_in[2] = (o_xb + 9'd2 >= o_cx0) && (o_xb + 9'd2 <= o_cx1);
	assign o_in[3] = (o_xb + 9'd3 >= o_cx0) && (o_xb + 9'd3 <= o_cx1);
	wire [9:0]  o_fbrow = {2'd0, o_y0} + {6'd0, o_row} + (o_seg[0] ? 10'd240 : 10'd0) +
	                      (o_seg[1] ? 10'd480 : 10'd0);
	// LINEAR: line y's word 20 seg + i at 80 y + 20 seg + i
	wire [9:0]  o_line = {2'd0, o_y0} + {6'd0, o_row};
	wire [14:0] o_lword = {o_line[8:0], 6'd0} + {1'd0, o_line, 4'd0} +
	                      {9'd0, o_seg, 4'd0} + {11'd0, o_seg, 2'd0} + {10'd0, o_i};
	wire [14:0] o_vword = LINEAR ? o_lword :
	                      ({o_fbrow, 4'd0} + {3'd0, o_fbrow, 2'd0} + {10'd0, o_i});
	wire        o_beam_in = (beam_y >= {1'b0, o_y0}) && (beam_y <= {1'b0, o_y0} + {5'd0, o_rows1});

	// -- the posted-write queue (POST_LOG2) --
	localparam integer PQ_N = (POST_LOG2 != 0) ? (1 << POST_LOG2) : 2;
	localparam integer PQ_B = (POST_LOG2 != 0) ? POST_LOG2 : 1;
	reg  [50:0]     pq [0:PQ_N-1];        // { word, sel, data }
	reg  [50:0]     pq_q;
	reg  [PQ_B-1:0] pq_wp, pq_rp;
	reg  [PQ_B:0]   pq_n;
	reg  [1:0]      pq_st;                // 0 idle, 1 read, 2 on the bus, 3 the next word's read
	reg  [3:0]      pq_run;               // words this grant
	wire            pq_full = (pq_n == PQ_N);
	wire            pq_push = (POST_LOG2 != 0) && (ost == O_WRITE) && !pq_full &&
	                          (o_in_q != 4'd0) && o_rowin_q;
	wire            pq_pop = (pq_st == 2'd2) && m_ack_i;
	wire            pq_busy = (POST_LOG2 != 0) && ((pq_n != 0) || (pq_st != 2'd0));

	always @(posedge clk) begin
		if (pq_push) pq[pq_wp] <= {o_vword_q, o_in_q, ou_col_q};
		pq_q <= pq[pq_rp];
	end

	always @(posedge clk) begin
		if (rst || soft_rst) begin
			ost <= O_IDLE;
			o_sel <= 1'b0;
			o_busy <= 1'b0;
			ready <= 2'b00;
			o_clr_set <= 1'b0;
			ou_col_we <= 4'd0;
			ou_dep_we <= 4'd0;
			m_cyc_o <= 1'b0;
			m_stb_o <= 1'b0;
			m_we_o <= 1'b0;
			m_sel_o <= 4'd0;
			strips <= 32'd0;
			pq_wp <= 0;
			pq_rp <= 0;
			pq_n <= 0;
			pq_st <= 2'd0;
			pq_run <= 4'd0;
		end else begin
			// the queue: its count, and its words to the bus
			if (POST_LOG2 != 0) begin
				if (pq_push) pq_wp <= pq_wp + 1'b1;
				if (pq_push && !pq_pop) pq_n <= pq_n + 1'b1;
				else if (pq_pop && !pq_push) pq_n <= pq_n - 1'b1;
				case (pq_st)
				2'd0: if (pq_n != 0) pq_st <= 2'd1;    // pq_q reads pq_rp
				2'd1: begin
					m_cyc_o <= 1'b1;
					m_stb_o <= 1'b1;
					m_we_o <= 1'b1;
					m_sel_o <= pq_q[35:32];
					m_adr_o <= vbase + {15'd0, pq_q[50:36], 2'd0};
					m_dat_o <= pq_q[31:0];
					pq_st <= 2'd2;
				end
				2'd2: if (m_ack_i) begin
					pq_rp <= pq_rp + 1'b1;
					if (pq_n > 1 && pq_run != 4'd7) begin
						// more queued: STB drops, CYC does not -- up
						// to 8 words a grant (main memory's
						// controllers start on CYC and STB; the DDR3
						// one keeps the 16-byte block a run of stores
						// lands in). tb_console_ddr3.v, a busy CPU:
						// 2.4 times the writes of a word a grant;
						// 16 gave little more and starved the CPU.
						m_stb_o <= 1'b0;
						pq_run <= pq_run + 4'd1;
						pq_st <= 2'd3;
					end else begin
						m_cyc_o <= 1'b0;
						m_stb_o <= 1'b0;
						m_we_o <= 1'b0;
						pq_run <= 4'd0;
						pq_st <= 2'd0;
					end
				end
				2'd3: pq_st <= 2'd1;                    // pq_q reads the new pq_rp
				endcase
			end
			ou_col_we <= 4'd0;
			ou_dep_we <= 4'd0;
			o_clr_set <= 1'b0;
			if (h_go) ready[TWO_BUFS ? ~d_sel : d_sel] <= 1'b1;   // d_sel has just flipped (two)

			case (ost)
			O_IDLE: begin
				if (ready[o_sel]) begin
					o_busy <= 1'b1;
					o_y0 <= h_y0; o_rows1 <= h_rows1; o_cx0 <= h_cx0; o_cx1 <= h_cx1;
					o_clear <= h_clear;
					o_first <= h_first;
					o_row <= 4'd0; o_seg <= 2'd0; o_i <= 5'd0; o_w <= 11'd0;
					ost <= O_BEAM;
				end
			end
			O_BEAM: if ((!wait_beam || !o_beam_in) && (!vsync || !o_first || vblank)) ost <= O_READ;
			O_READ: begin
				ou_a_addr <= o_w;
				o_in_q <= o_in;
				o_vword_q <= o_vword;
				o_rowin_q <= (o_row <= o_rows1);
				ost <= O_DATA;
			end
			O_DATA: ost <= O_WRITE;
			O_WRITE: begin
				if (POST_LOG2 != 0) begin
					// into the queue (pq_push, above), the strip's word
					// cleared behind, as below; a full queue waits
					if (!pq_full) begin
						ou_b_addr <= o_w;
						ou_col_we <= 4'b1111;
						ou_col_d <= {4{o_clear}};
						ou_dep_we <= 4'b1111;
						ou_dep_d <= {64{1'b1}};
						ost <= O_NEXT;
					end
				end else if (!m_cyc_o) begin
					ou_b_addr <= o_w;
					ou_col_we <= 4'b1111;
					ou_col_d <= {4{o_clear}};
					ou_dep_we <= 4'b1111;
					ou_dep_d <= {64{1'b1}};
					if (o_in_q != 4'd0 && o_rowin_q) begin
						m_cyc_o <= 1'b1;
						m_stb_o <= 1'b1;
						m_we_o <= 1'b1;
						m_sel_o <= o_in_q;
						m_adr_o <= vbase + {15'd0, o_vword_q, 2'd0};
						m_dat_o <= ou_col_q;
					end else ost <= O_NEXT;
				end else if (m_ack_i) begin
					m_cyc_o <= 1'b0;
					m_stb_o <= 1'b0;
					m_we_o <= 1'b0;
					ost <= O_NEXT;
				end
			end
			O_NEXT: begin
				o_w <= o_w + 11'd1;
				if (o_i == 5'd19) begin
					o_i <= 5'd0;
					if (o_seg == 2'd3) begin
						o_seg <= 2'd0;
						o_row <= o_row + 4'd1;
					end else o_seg <= o_seg + 2'd1;
				end else o_i <= o_i + 5'd1;
				if (o_w == BUF_LAST) ost <= O_DONE;
				else ost <= O_READ;
			end
			O_DONE: begin
				ready[o_sel] <= 1'b0;
				o_clr_set <= 1'b1;
				o_clr_buf <= o_sel;
				if (TWO_BUFS) o_sel <= ~o_sel;
				o_busy <= 1'b0;
				strips <= strips + 32'd1;
				ost <= O_IDLE;
			end
			default: ost <= O_IDLE;
			endcase
		end
	end

	// ------------------------------------------------------------------
	// the slave port
	// ------------------------------------------------------------------

	// the core is idle when it waits for a stream word that isn't there
	wire geom_idle = g_inwait && f_empty;
	wire busy = !f_empty || !geom_idle || r_busy || fr_go || h_go || (ready != 2'b00) || o_busy || o_clr_set ||
	            pq_busy;

	always @(posedge clk) begin
		if (rst) begin
			wb_ack_o <= 1'b0;
			wb_dat_o <= 32'd0;
			wait_beam <= 1'b0;
			vsync <= 1'b0;
			vbase <= 32'h20000000;
			soft_rst <= 1'b0;
		end else begin
			wb_ack_o <= 1'b0;
			soft_rst <= 1'b0;
			if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin
				if (wb_we_i) begin
					case (wb_adr_i[3:0])
					4'd0: begin wait_beam <= wb_dat_i[0]; vsync <= wb_dat_i[1]; wb_ack_o <= 1'b1; end
					4'd2: if (!f_full) wb_ack_o <= 1'b1;
					4'd3: begin vbase <= wb_dat_i; wb_ack_o <= 1'b1; end
					4'd5: begin soft_rst <= 1'b1; wb_ack_o <= 1'b1; end
					// TEXADDR, TEXDATA (the texture memory is written above)
					4'd12: begin tex_wa <= wb_dat_i[TX_LOG2-3:0]; wb_ack_o <= 1'b1; end
					4'd13: begin tex_wa <= tex_wa + 1'b1; wb_ack_o <= 1'b1; end
					default: wb_ack_o <= 1'b1;
					endcase
				end else begin
					case (wb_adr_i[3:0])
					4'd0: wb_dat_o <= {30'd0, vsync, wait_beam};
					4'd1: wb_dat_o <= {4'd0, F_N[11:0] - f_n, 15'd0, busy};
					4'd3: wb_dat_o <= vbase;
					4'd4: wb_dat_o <= strips;
					4'd7: wb_dat_o <= 32'h5A545232;
					// CAPS: textures, and the texture memory's size
					4'd11: wb_dat_o <= {19'd0, TX_LOG2_5, 7'd0, TEX ? 1'b1 : 1'b0};
					default: wb_dat_o <= 32'd0;
					endcase
					wb_ack_o <= 1'b1;
				end
			end
		end
	end

endmodule
