/*
 * Zeitlos SOC GPU
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * gpu_tri.v -- the triangle engine (docs/gpu_tri.md): zgl's ZGL_FRAMES
 * strips drawn in hardware and written into chunky VRAM.
 *
 * The CPU (sw/common/zgl/zgl_frames.c, zglf_t_frame()) cuts each frame
 * into strips of up to 16 rows and sets each strip's triangles and lines
 * up as commands: a triangle as one or two trapezoids whose edges are
 * already at their first row, a line as its steps inside the strip.
 * The engine is left the stepping and the pixels -- no division, and one
 * multiply a row -- and draws exactly what the software draws, pixel for
 * pixel (rtl/gpu/bench/tb_gpu_tri.v holds it to the software's pictures).
 *
 * -- Commands (written to CMD, a 512-word FIFO) --
 *
 *   BEGIN  3 << 28 | first << 27 | y0 << 12 | clear << 4 | rows - 1
 *          cx0 | cx1 << 16
 *   TRAP   1 << 28 | depth << 27 | ya << 4 | yb       (rows of the strip)
 *          left edge, right edge, 3 words each: q | qs << 16, r | rs << 16, D
 *          z0, dz/dx, dz/dy                          (z at column 0 of row ya)
 *          c0, dc/dx, dc/dy                          (the index, 8.8)
 *   LINE   2 << 28 | depth << 27 | steps - 1
 *          x, y (pixel = value >>> 20), dx, dy, z, dz, c, dc
 *   END    4 << 28
 *
 * -- Strips --
 *
 * Two strip buffers, each a colour buffer (320 x 16 bytes, as 1,280
 * words of four pixels) and a depth buffer (320 x 16 x 16 bits, as
 * 1,280 words of four). One is drawn while the other goes out to VRAM;
 * going out clears it, word by word, behind the read. A pixel a clock
 * along a span: the depth word is read for pixel x while the word of
 * pixel x - 1 is written (different lanes of the same word, or a
 * different word).
 *
 * -- Out to VRAM --
 *
 * Chunky (docs/color.md, "Chunky"): row r's segment s (80 pixels, 20
 * words) is framebuffer row r + 240 (s & 1) + 480 (s >> 1), 20 words a
 * row, so a strip goes out in the order its buffer holds it. Bytes
 * outside cx0..cx1 are not written (byte selects). With CTRL.VSYNC a
 * frame's first strip (BEGIN's first bit) waits for vertical blanking:
 * a strip then goes out in a seventh of the time the beam takes to show
 * it, so the frame keeps ahead of the beam as long as drawing does.
 * With CTRL.WAIT_BEAM a strip waits while the beam is inside its rows
 * (beam_y; not wired in the SOC yet).
 *
 * -- Out to a window: 1 bpp --
 *
 * With CTRL.MONO a strip goes instead into the 1 bpp desktop framebuffer
 * (640 x 480, 20 words a row, pixel x at bit x & 31): the picture at
 * WIN (its top-left on the screen), only inside CLIPX/CLIPY (picture
 * coordinates: the part of the window that is visible, one rectangle).
 * Each index's brightness (LUM, 0..16, from the palette) against an
 * 8x8 ordered dither, as the software's monochrome strips do. Words the
 * rectangle covers in part are read, merged and written back; the
 * buffer is then cleared in a sweep of its own.
 *
 * -- Registers (word offsets) --
 *
 *   0 CTRL    rw  [0] WAIT_BEAM  [1] VSYNC  [2] MONO
 *   1 STATUS  r   [0] busy (anything queued, drawn or going out)
 *                 [1] a bad command was seen (until RESET)
 *                 [25:16] FIFO words free
 *   2 CMD     w   a command word; the write waits while the FIFO is full
 *   3 VBASE   rw  chunky VRAM's byte address (0x20000000)
 *   4 STRIPS  r   strips written out since RESET
 *   5 RESET   w   any value: FIFO emptied, engine and output stopped
 *   6 LUM     w   index | lum << 8: an index's brightness, 0..16 (MONO)
 *   7 ID      r   0x5A545249 ("ZTRI"): the engine is here. A bitstream
 *                 without it answers this address from csrs.v, with 0
 *   8 WIN     rw  x | y << 16: the picture's top-left on the screen (MONO)
 *   9 CLIPX   rw  x0 | x1 << 16: the visible columns, picture coordinates
 *  10 CLIPY   rw  y0 | y1 << 16: the visible rows
 *
 * Plain Verilog-2001: no declarations inside blocks, one driver a signal.
 */

module gpu_tri_buf (
	input             clk,
	// port A: read both words
	input      [10:0] a_addr,
	output reg [31:0] a_col,
	output reg [63:0] a_dep,
	// port B: write lanes
	input      [10:0] b_addr,
	input      [3:0]  b_col_we,
	input      [31:0] b_col_d,
	input      [3:0]  b_dep_we,
	input      [63:0] b_dep_d
);
	reg [31:0] col [0:1279];
	reg [63:0] dep [0:1279];

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

module gpu_tri_wb (
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

	// the game-mode line being shown, 0..239 (anything else: none)
	input      [8:0]  beam_y,
	// in vertical blanking, synchronised to clk
	input             vblank
);

	// ------------------------------------------------------------------
	// registers
	// ------------------------------------------------------------------

	reg        wait_beam;
	reg        vsync;
	reg        mono;
	reg [9:0]  win_x;
	reg [8:0]  win_y;
	reg [8:0]  clip_x0, clip_x1;
	reg [7:0]  clip_y0, clip_y1;
	reg [4:0]  lum [0:255];
	reg [31:0] vbase;
	reg [31:0] strips;
	reg        bad_cmd;
	reg        soft_rst;

	// ------------------------------------------------------------------
	// command FIFO
	// ------------------------------------------------------------------

	reg [31:0] fifo [0:511];
	reg [8:0]  f_wr, f_rd;
	reg [9:0]  f_n;
	reg [31:0] f_q;
	wire       f_full  = (f_n == 10'd512);
	wire       f_empty = (f_n == 10'd0);
	wire       wb_cmd  = wb_cyc_i && wb_stb_i && wb_we_i && !wb_ack_o && (wb_adr_i[3:0] == 4'd2);
	wire       f_push  = wb_cmd && !f_full;
	reg        f_pop;             // the engine takes a word (f_q next cycle)

	always @(posedge clk) begin
		if (f_push) fifo[f_wr] <= wb_dat_i;
		f_q <= fifo[f_rd];
	end

	always @(posedge clk) begin
		if (rst || soft_rst) begin
			f_wr <= 9'd0;
			f_rd <= 9'd0;
			f_n <= 10'd0;
		end else begin
			if (f_push) f_wr <= f_wr + 9'd1;
			if (f_pop) f_rd <= f_rd + 9'd1;
			if (f_push && !f_pop) f_n <= f_n + 10'd1;
			else if (!f_push && f_pop) f_n <= f_n - 10'd1;
		end
	end

	// ------------------------------------------------------------------
	// strip buffers: two, one drawn while the other goes out
	// ------------------------------------------------------------------

	reg        d_sel;             // the buffer being drawn
	reg        o_sel;             // the buffer going out
	reg [1:0]  ready;             // drawn, waiting to go out
	reg [8:0]  bclr0, bclr1;      // what each is clear to (bit 8: unknown)

	// what the draw side and the output side drive on the ports
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
	wire [31:0] b0_col, b1_col;
	wire [63:0] b0_dep, b1_dep;

	// A buffer is the output side's while it goes out, and otherwise
	// the draw side's if it is the one being drawn (d_sel): the draw
	// side waits for its buffer to come back (S_BEGIN), and its writes
	// reach no other.
	wire o_on0 = (o_act && !o_sel) || (o_clr_set && !o_clr_buf);
	wire o_on1 = (o_act && o_sel) || (o_clr_set && o_clr_buf);

	gpu_tri_buf buf0 (
		.clk(clk),
		.a_addr(o_on0 ? ou_a_addr : dr_a_addr),
		.a_col(b0_col), .a_dep(b0_dep),
		.b_addr(o_on0 ? ou_b_addr : dr_b_addr),
		.b_col_we(o_on0 ? ou_col_we : (d_sel ? 4'd0 : dr_col_we)),
		.b_col_d(o_on0 ? ou_col_d : dr_col_d),
		.b_dep_we(o_on0 ? ou_dep_we : (d_sel ? 4'd0 : dr_dep_we)),
		.b_dep_d(o_on0 ? ou_dep_d : dr_dep_d)
	);

	gpu_tri_buf buf1 (
		.clk(clk),
		.a_addr(o_on1 ? ou_a_addr : dr_a_addr),
		.a_col(b1_col), .a_dep(b1_dep),
		.b_addr(o_on1 ? ou_b_addr : dr_b_addr),
		.b_col_we(o_on1 ? ou_col_we : (d_sel ? dr_col_we : 4'd0)),
		.b_col_d(o_on1 ? ou_col_d : dr_col_d),
		.b_dep_we(o_on1 ? ou_dep_we : (d_sel ? dr_dep_we : 4'd0)),
		.b_dep_d(o_on1 ? ou_dep_d : dr_dep_d)
	);

	wire [63:0] dr_dep_q = d_sel ? b1_dep : b0_dep;
	wire [31:0] ou_col_q = o_sel ? b1_col : b0_col;

	// ------------------------------------------------------------------
	// the draw side
	// ------------------------------------------------------------------

	localparam [3:0] S_IDLE  = 4'd0,  S_HDR   = 4'd1,  S_ARGS  = 4'd2,
	                 S_BEGIN = 4'd3,  S_CLR   = 4'd4,  S_END   = 4'd5,
	                 S_ROW   = 4'd6,  S_MUL   = 4'd7,  S_SPAN  = 4'd8,
	                 S_NEXT  = 4'd9,  S_LSTEP = 4'd10, S_LPIX  = 4'd11,
	                 S_LADV  = 4'd12, S_DRAIN = 4'd13;

	reg [3:0]  st;
	reg [3:0]  op;
	reg        c_depth;
	reg [3:0]  nargs;             // argument words still to come
	reg [3:0]  widx;              // the next argument word
	reg        rpend;             // a FIFO word was taken last cycle

	// the strip being drawn
	reg [7:0]  s_y0;
	reg [3:0]  s_rows1;           // rows - 1
	reg [7:0]  s_clear;
	reg        s_first;           // the frame's first strip
	reg [8:0]  s_cx0, s_cx1;

	// a trapezoid
	reg [3:0]  t_ya, t_yb, t_row;
	reg signed [15:0] l_q, r_q, l_qs, r_qs;
	reg [16:0] l_r, r_r;          // 0 .. D - 1
	reg [15:0] l_rs, r_rs, l_d, r_d;
	reg [31:0] z_row, dzdx, dzdy, c_row, dcdx, dcdy;
	reg [31:0] z_px, c_px;
	reg [8:0]  x, x_end;
	reg        span_empty;

	// a line
	reg [15:0] l_steps;
	reg [31:0] ln_x, ln_y, ln_dx, ln_dy, ln_z, ln_dz, ln_c, ln_dc;

	// a pixel's way through: p0 the depth word asked for, p1 it comes
	// (its lane registered), p2 the test is made, then the write
	// (dr_b_*), then the RAM. The test has a stage of its own: the RAM's
	// output, the buffer and lane muxes and the compare in one cycle
	// were 22 ns on the full sergei_ml2 SOC.
	reg        p0_v;
	reg [10:0] p0_addr;
	reg [1:0]  p0_lane;
	reg [15:0] p0_zv;
	reg [7:0]  p0_ci;
	reg        p0_le;
	reg        p0_depth;
	reg [1:0]  lwait;
	reg        p1_v;
	reg [10:0] p1_addr;
	reg [1:0]  p1_lane;
	reg [15:0] p1_zv;
	reg [7:0]  p1_ci;
	reg        p1_le;             // a line's test: <= rather than <
	reg        p2_v;
	reg [10:0] p2_addr;
	reg [1:0]  p2_lane;
	reg [15:0] p2_zv;
	reg [7:0]  p2_ci;
	reg        p2_le;
	reg        p2_depth;
	reg [15:0] p2_old;

	// clearing a buffer (S_CLR)
	reg [10:0] clr_w;

	// --- combinational helpers ---

	wire signed [16:0] l_qx  = {l_q[15], l_q};
	wire signed [16:0] r_qx1 = {r_q[15], r_q} - 17'sd1;
	wire signed [16:0] cx0s  = {8'd0, s_cx0};
	wire signed [16:0] cx1s  = {8'd0, s_cx1};
	wire signed [16:0] row_x0 = (l_qx < cx0s) ? cx0s : l_qx;
	wire signed [16:0] row_x1 = (r_qx1 > cx1s) ? cx1s : r_qx1;

	wire signed [17:0] l_rn = $signed({1'b0, l_r}) - $signed({2'b0, l_rs});
	wire signed [17:0] r_rn = $signed({1'b0, r_r}) - $signed({2'b0, r_rs});

	wire [31:0] z_start = z_row + dzdx * {23'd0, x};
	wire [31:0] c_start = c_row + dcdx * {23'd0, x};

	// the word address of (row, x): row * 80 + x / 4
	wire [10:0] row_base  = {t_row, 6'd0} + {2'd0, t_row, 4'd0};
	wire [10:0] span_addr = row_base + {4'd0, x[8:2]};

	// dither: bayer4[picture row & 3][x & 3]
	wire [1:0]  dth_row = s_y0[1:0] + t_row[1:0];
	reg  [7:0]  dth;
	always @(*) begin
		case ({dth_row, x[1:0]})
		4'h0: dth = 8'd8;   4'h1: dth = 8'd136; 4'h2: dth = 8'd40;  4'h3: dth = 8'd168;
		4'h4: dth = 8'd200; 4'h5: dth = 8'd72;  4'h6: dth = 8'd232; 4'h7: dth = 8'd104;
		4'h8: dth = 8'd56;  4'h9: dth = 8'd184; 4'ha: dth = 8'd24;  4'hb: dth = 8'd152;
		4'hc: dth = 8'd248; 4'hd: dth = 8'd120; 4'he: dth = 8'd216; default: dth = 8'd88;
		endcase
	end
	wire [31:0] c_dth = c_px + {24'd0, dth};
	wire [15:0] zv    = z_px[31] ? 16'd0 : z_px[30:15];

	// a line's pixel
	wire signed [31:0] ln_xs = ln_x;
	wire signed [31:0] ln_ys = ln_y;
	wire signed [11:0] ln_pxs = ln_xs[31:20];
	wire signed [11:0] ln_pys = ln_ys[31:20] - $signed({4'd0, s_y0});
	wire        ln_in = (ln_pys >= 12'sd0) && (ln_pys <= $signed({8'd0, s_rows1})) &&
	                    (ln_pxs >= $signed({3'd0, s_cx0})) && (ln_pxs <= $signed({3'd0, s_cx1}));
	wire [31:0] ln_cr = ln_c + 32'd128;
	wire [15:0] ln_zv = ln_z[31] ? 16'd0 : ln_z[30:15];
	wire [10:0] ln_base = {ln_pys[3:0], 6'd0} + {2'd0, ln_pys[3:0], 4'd0};
	wire [10:0] ln_addr = ln_base + {4'd0, ln_pxs[8:2]};

	// the second stage's depth lane, and its verdict
	reg  [15:0] p1_old;
	always @(*) begin
		case (p1_lane)
		2'd0: p1_old = dr_dep_q[15:0];
		2'd1: p1_old = dr_dep_q[31:16];
		2'd2: p1_old = dr_dep_q[47:32];
		default: p1_old = dr_dep_q[63:48];
		endcase
	end
	reg        p1_depth;
	wire       p2_pass = !p2_depth || (p2_le ? (p2_zv <= p2_old) : (p2_zv < p2_old));

	// is the buffer to be drawn free, and clear to this strip's colour?
	wire       d_busy  = ready[d_sel] || (o_busy && (o_sel == d_sel)) || o_clr_set;
	wire [8:0] d_clr   = d_sel ? bclr1 : bclr0;

	// the output side (below) says when it is busy and with what
	reg        o_busy;
	reg        o_clr_set;         // it has just cleared o_clr_buf to o_clear
	reg        o_clr_buf;
	reg [7:0]  o_clear;

	// strip handed to the output: its numbers
	reg [7:0]  h_y0;
	reg [3:0]  h_rows1;
	reg [8:0]  h_cx0, h_cx1;
	reg [7:0]  h_clear;
	reg        h_first;
	reg        h_go;              // a strip handed over this cycle

	always @(posedge clk) begin
		if (rst || soft_rst) begin
			st <= S_IDLE;
			f_pop <= 1'b0;
			rpend <= 1'b0;
			d_sel <= 1'b0;
			p0_v <= 1'b0;
			p1_v <= 1'b0;
			p2_v <= 1'b0;
			bad_cmd <= 1'b0;
			h_go <= 1'b0;
			bclr0 <= 9'h100;
			bclr1 <= 9'h100;
			dr_col_we <= 4'd0;
			dr_dep_we <= 4'd0;
			s_y0 <= 8'd0; s_rows1 <= 4'd15; s_cx0 <= 9'd0; s_cx1 <= 9'd319; s_clear <= 8'd0;
		end else begin
			f_pop <= 1'b0;
			rpend <= f_pop;
			h_go <= 1'b0;
			dr_col_we <= 4'd0;
			dr_dep_we <= 4'd0;

			// the output side records what it cleared to
			if (o_clr_set) begin
				if (o_clr_buf) bclr1 <= {1'b0, o_clear};
				else bclr0 <= {1'b0, o_clear};
			end

			// a pixel's third stage: the test, and the write out
			if (p2_v) begin
				dr_b_addr <= p2_addr;
				if (p2_pass) begin
					dr_col_we <= 4'b0001 << p2_lane;
					dr_col_d <= {4{p2_ci}};
					if (p2_depth) begin
						dr_dep_we <= 4'b0001 << p2_lane;
						dr_dep_d <= {4{p2_zv}};
					end
				end
			end
			// the second: the depth word in, its lane kept
			p2_v <= p1_v;
			p2_addr <= p1_addr;
			p2_lane <= p1_lane;
			p2_zv <= p1_zv;
			p2_ci <= p1_ci;
			p2_le <= p1_le;
			p2_depth <= p1_depth;
			p2_old <= p1_old;
			p1_v <= p0_v;
			p1_addr <= p0_addr;
			p1_lane <= p0_lane;
			p1_zv <= p0_zv;
			p1_ci <= p0_ci;
			p1_le <= p0_le;
			p1_depth <= p0_depth;
			p0_v <= 1'b0;

			case (st)
			S_IDLE: begin
				if (f_n > (f_pop ? 10'd1 : 10'd0)) begin
					f_pop <= 1'b1;
					st <= S_HDR;
				end
			end

			S_HDR: if (rpend) begin
				op <= f_q[31:28];
				c_depth <= f_q[27];
				widx <= 4'd0;
				case (f_q[31:28])
				4'd1: begin t_ya <= f_q[7:4]; t_yb <= f_q[3:0]; nargs <= 4'd12; st <= S_ARGS; end
				4'd2: begin l_steps <= f_q[15:0]; nargs <= 4'd8; st <= S_ARGS; end
				4'd3: begin
					s_y0 <= f_q[19:12]; s_clear <= f_q[11:4]; s_rows1 <= f_q[3:0];
					s_first <= f_q[27];
					nargs <= 4'd1; st <= S_ARGS;
				end
				4'd4: st <= S_END;
				default: begin bad_cmd <= 1'b1; st <= S_IDLE; end
				endcase
			end

			S_ARGS: begin
				// take words as they come, one a cycle when the FIFO has them
				if (nargs != 4'd0 && (f_n > (f_pop ? 10'd1 : 10'd0))) begin
					f_pop <= 1'b1;
					nargs <= nargs - 4'd1;
				end
				if (rpend) begin
					widx <= widx + 4'd1;
					case (op)
					4'd1: case (widx)
						4'd0: begin l_q <= f_q[15:0]; l_qs <= f_q[31:16]; end
						4'd1: begin l_r <= {1'b0, f_q[15:0]}; l_rs <= f_q[31:16]; end
						4'd2: l_d <= f_q[15:0];
						4'd3: begin r_q <= f_q[15:0]; r_qs <= f_q[31:16]; end
						4'd4: begin r_r <= {1'b0, f_q[15:0]}; r_rs <= f_q[31:16]; end
						4'd5: r_d <= f_q[15:0];
						4'd6: z_row <= f_q;
						4'd7: dzdx <= f_q;
						4'd8: dzdy <= f_q;
						4'd9: c_row <= f_q;
						4'd10: dcdx <= f_q;
						default: begin dcdy <= f_q; t_row <= t_ya; st <= S_ROW; end
						endcase
					4'd2: case (widx)
						4'd0: ln_x <= f_q;
						4'd1: ln_y <= f_q;
						4'd2: ln_dx <= f_q;
						4'd3: ln_dy <= f_q;
						4'd4: ln_z <= f_q;
						4'd5: ln_dz <= f_q;
						4'd6: ln_c <= f_q;
						default: begin ln_dc <= f_q; st <= S_LSTEP; end
						endcase
					default: begin                 // BEGIN's columns
						s_cx0 <= f_q[8:0];
						s_cx1 <= f_q[24:16];
						st <= S_BEGIN;
					end
					endcase
				end
			end

			S_BEGIN: begin
				// wait for this buffer to come back from going out; clear
				// it again if it is clear to another colour
				if (!d_busy) begin
					if (d_clr != {1'b0, s_clear}) begin
						clr_w <= 11'd0;
						st <= S_CLR;
					end else st <= S_IDLE;
				end
			end

			S_CLR: begin
				dr_b_addr <= clr_w;
				dr_col_we <= 4'b1111;
				dr_col_d <= {4{s_clear}};
				dr_dep_we <= 4'b1111;
				dr_dep_d <= {64{1'b1}};
				clr_w <= clr_w + 11'd1;
				if (clr_w == 11'd1279) begin
					if (d_sel) bclr1 <= {1'b0, s_clear};
					else bclr0 <= {1'b0, s_clear};
					st <= S_IDLE;
				end
			end

			S_END: begin
				// the strip goes out; draw the next in the other buffer
				// (the last pixel's write is already out of the pipe)
				h_y0 <= s_y0; h_rows1 <= s_rows1; h_cx0 <= s_cx0; h_cx1 <= s_cx1;
				h_clear <= s_clear;
				h_first <= s_first;
				h_go <= 1'b1;
				d_sel <= ~d_sel;
				st <= S_IDLE;
			end

			// --- a trapezoid, a row at a time ---
			S_ROW: begin
				x <= row_x0[8:0];
				x_end <= row_x1[8:0];
				span_empty <= (row_x0 > row_x1);
				// step both edges (edge_step())
				if (l_rn < 0) begin l_r <= l_rn[16:0] + {1'b0, l_d}; l_q <= l_q + l_qs + 16'sd1; end
				else begin l_r <= l_rn[16:0]; l_q <= l_q + l_qs; end
				if (r_rn < 0) begin r_r <= r_rn[16:0] + {1'b0, r_d}; r_q <= r_q + r_qs + 16'sd1; end
				else begin r_r <= r_rn[16:0]; r_q <= r_q + r_qs; end
				st <= S_MUL;
			end

			S_MUL: begin
				z_px <= z_start;
				c_px <= c_start;
				z_row <= z_row + dzdy;
				c_row <= c_row + dcdy;
				st <= span_empty ? S_NEXT : S_SPAN;
			end

			S_SPAN: begin
				// first stage: read this pixel's depth word
				dr_a_addr <= span_addr;
				p0_v <= 1'b1;
				p0_addr <= span_addr;
				p0_lane <= x[1:0];
				p0_zv <= zv;
				p0_ci <= c_dth[15:8];
				p0_le <= 1'b0;
				p0_depth <= c_depth;
				z_px <= z_px + dzdx;
				c_px <= c_px + dcdx;
				x <= x + 9'd1;
				if (x == x_end) st <= S_NEXT;
			end

			S_NEXT: begin
				t_row <= t_row + 4'd1;
				if (t_row == t_yb) st <= S_DRAIN;
				else st <= S_ROW;
			end

			// the last pixel through the test (its write follows, before
			// any S_END or S_CLR can be reached)
			S_DRAIN: if (!p0_v && !p1_v && !p2_v) st <= S_IDLE;

			// --- a line, a step at a time ---
			S_LSTEP: begin
				if (ln_in) begin
					dr_a_addr <= ln_addr;
					p0_v <= 1'b1;
					p0_addr <= ln_addr;
					p0_lane <= ln_pxs[1:0];
					p0_zv <= ln_zv;
					p0_ci <= ln_cr[15:8];
					p0_le <= 1'b1;
					p0_depth <= c_depth;
					lwait <= 2'd2;
					st <= S_LPIX;
				end else st <= S_LADV;
			end

			S_LPIX: begin
				// the pixel goes through the pipe before the next step
				// reads: a step can land on the pixel just written
				lwait <= lwait - 2'd1;
				if (lwait == 2'd0) st <= S_LADV;
			end

			S_LADV: begin
				ln_x <= ln_x + ln_dx;
				ln_y <= ln_y + ln_dy;
				ln_z <= ln_z + ln_dz;
				ln_c <= ln_c + ln_dc;
				l_steps <= l_steps - 16'd1;
				if (l_steps == 16'd0) st <= S_DRAIN;
				else st <= S_LSTEP;
			end

			default: st <= S_IDLE;
			endcase
		end
	end

	// ------------------------------------------------------------------
	// the output side: a drawn strip to chunky VRAM, clearing behind
	// ------------------------------------------------------------------

	localparam [3:0] O_IDLE = 4'd0, O_BEAM = 4'd1, O_READ = 4'd2, O_DATA = 4'd3,
	                 O_WRITE = 4'd4, O_NEXT = 4'd5,
	                 O_MROW = 4'd6, O_MPIX = 4'd7, O_MRD = 4'd8, O_MWR = 4'd9,
	                 O_MNEXT = 4'd10, O_MCLR = 4'd11, O_DONE = 4'd12;

	reg [3:0]  ost;
	// the output side holds its buffer from its first read to its last
	// clear write, which lands the cycle after it returns to O_IDLE --
	// when o_sel already names the other buffer, so that cycle's owner
	// is o_clr_buf (o_on0, o_on1)
	assign o_act = (ost != O_IDLE);
	reg [7:0]  o_y0;
	reg        o_first;
	reg [3:0]  o_rows1;
	reg [8:0]  o_cx0, o_cx1;
	reg [3:0]  o_row;
	reg [1:0]  o_seg;
	reg [4:0]  o_i;
	reg [10:0] o_w;               // the buffer word: row * 80 + seg * 20 + i
	reg [31:0] o_word;
	reg [3:0]  o_sel_b;

	// the byte selects of this word: its pixels inside cx0..cx1
	// (a table, not a multiply: a multiplier's output into the compares
	// below and on to the VRAM address was a slow path)
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

	// the framebuffer row and word in VRAM
	wire [9:0]  o_fbrow = {2'd0, o_y0} + {6'd0, o_row} + (o_seg[0] ? 10'd240 : 10'd0) +
	                      (o_seg[1] ? 10'd480 : 10'd0);
	wire [14:0] o_vword = {o_fbrow, 4'd0} + {3'd0, o_fbrow, 2'd0} + {10'd0, o_i};
	wire        o_beam_in = (beam_y >= {1'b0, o_y0}) && (beam_y <= {1'b0, o_y0} + {5'd0, o_rows1});

	// -- MONO: a row's columns ex0..ex1 into screen words dw..dw_end --
	wire [7:0]  m_py   = o_y0 + {4'd0, o_row};
	wire [8:0]  m_ex0  = (clip_x0 > o_cx0) ? clip_x0 : o_cx0;
	wire [8:0]  m_ex1  = (clip_x1 < o_cx1) ? clip_x1 : o_cx1;
	wire        m_rowin = (o_row <= o_rows1) && (m_py >= clip_y0) && (m_py <= clip_y1) && (m_ex0 <= m_ex1);
	wire [10:0] m_sx0  = {1'b0, win_x} + {2'd0, m_ex0};          // screen columns
	wire [10:0] m_sx1  = {1'b0, win_x} + {2'd0, m_ex1};
	reg  [5:0]  m_dw, m_dw_end;    // screen words of the row
	reg  [4:0]  m_b;               // the bit being asked for
	reg         m_issue;           // still asking (bits m_b..31)
	reg  [31:0] m_acc, m_msk;
	reg  [31:0] m_old;
	reg         m_rmw;
	reg  [2:0]  m_drain;
	// the pixel of bit m_b: its picture column, inside the row's or not
	wire [10:0] m_dx   = {m_dw, 5'd0} + {6'd0, m_b};
	wire [10:0] m_px   = m_dx - {1'b0, win_x};
	wire        m_pin  = !m_dx[10] && (m_dx >= m_sx0) && (m_dx <= m_sx1);
	wire [10:0] m_addr = {o_row, 6'd0} + {2'd0, o_row, 4'd0} + {4'd0, m_px[8:2]};
	// stages: asked (s1), the word is here (s2), its byte (s3)
	reg         s1_v, s2_v, s1_in, s2_in;
	reg  [1:0]  s1_lane, s2_lane;
	reg  [4:0]  s1_b, s2_b;
	reg  [5:0]  s1_th, s2_th;
	reg  [7:0]  s2_ci;
	// a third stage: the byte is registered before its brightness is
	// looked up (block RAM to the lookup in one cycle missed 50 MHz)
	reg         s3_v, s3_in;
	reg  [4:0]  s3_b;
	reg  [5:0]  s3_th;
	reg  [31:0] s3_word;           // the buffer word, registered (rule: no
	reg  [1:0]  s3_lane;           // block RAM output straight into logic)
	reg         s4_v, s4_in;
	reg  [4:0]  s4_b;
	reg  [5:0]  s4_th;
	reg  [7:0]  s4_byte;
	reg  [5:0]  m_th;              // bayer8[py & 7][px & 7]
	always @(*) begin
		case ({m_py[2:0], m_px[2:0]})
		6'o00: m_th = 6'd0;  6'o01: m_th = 6'd32; 6'o02: m_th = 6'd8;  6'o03: m_th = 6'd40;
		6'o04: m_th = 6'd2;  6'o05: m_th = 6'd34; 6'o06: m_th = 6'd10; 6'o07: m_th = 6'd42;
		6'o10: m_th = 6'd48; 6'o11: m_th = 6'd16; 6'o12: m_th = 6'd56; 6'o13: m_th = 6'd24;
		6'o14: m_th = 6'd50; 6'o15: m_th = 6'd18; 6'o16: m_th = 6'd58; 6'o17: m_th = 6'd26;
		6'o20: m_th = 6'd12; 6'o21: m_th = 6'd44; 6'o22: m_th = 6'd4;  6'o23: m_th = 6'd36;
		6'o24: m_th = 6'd14; 6'o25: m_th = 6'd46; 6'o26: m_th = 6'd6;  6'o27: m_th = 6'd38;
		6'o30: m_th = 6'd60; 6'o31: m_th = 6'd28; 6'o32: m_th = 6'd52; 6'o33: m_th = 6'd20;
		6'o34: m_th = 6'd62; 6'o35: m_th = 6'd30; 6'o36: m_th = 6'd54; 6'o37: m_th = 6'd22;
		6'o40: m_th = 6'd3;  6'o41: m_th = 6'd35; 6'o42: m_th = 6'd11; 6'o43: m_th = 6'd43;
		6'o44: m_th = 6'd1;  6'o45: m_th = 6'd33; 6'o46: m_th = 6'd9;  6'o47: m_th = 6'd41;
		6'o50: m_th = 6'd51; 6'o51: m_th = 6'd19; 6'o52: m_th = 6'd59; 6'o53: m_th = 6'd27;
		6'o54: m_th = 6'd49; 6'o55: m_th = 6'd17; 6'o56: m_th = 6'd57; 6'o57: m_th = 6'd25;
		6'o60: m_th = 6'd15; 6'o61: m_th = 6'd47; 6'o62: m_th = 6'd7;  6'o63: m_th = 6'd39;
		6'o64: m_th = 6'd13; 6'o65: m_th = 6'd45; 6'o66: m_th = 6'd5;  6'o67: m_th = 6'd37;
		6'o70: m_th = 6'd63; 6'o71: m_th = 6'd31; 6'o72: m_th = 6'd55; 6'o73: m_th = 6'd23;
		6'o74: m_th = 6'd61; 6'o75: m_th = 6'd29; 6'o76: m_th = 6'd53; default: m_th = 6'd21;
		endcase
	end
	// a fourth: the registered word's byte, then its brightness (the
	// buffer's output through both muxes to the lookup's address was
	// sergei_ml2's slowest path at 51 MHz)
	reg  [7:0]  s3_byte;
	always @(*) begin
		case (s3_lane)
		2'd0: s3_byte = s3_word[7:0];
		2'd1: s3_byte = s3_word[15:8];
		2'd2: s3_byte = s3_word[23:16];
		default: s3_byte = s3_word[31:24];
		endcase
	end
	wire [4:0]  s4_lum = lum[s4_byte];
	wire        s4_bit = ({s4_lum, 2'b00} > {1'b0, s4_th});
	// the screen word's VRAM address: (win_y + py) * 20 + dw
	wire [8:0]  m_sy   = win_y + {1'b0, m_py};
	wire [14:0] m_vw   = {1'b0, m_sy, 4'd0} + {3'd0, m_sy, 2'd0} + {9'd0, m_dw};

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
			s1_v <= 1'b0;
			s2_v <= 1'b0;
			s3_v <= 1'b0;
			s4_v <= 1'b0;
		end else begin
			ou_col_we <= 4'd0;
			ou_dep_we <= 4'd0;
			o_clr_set <= 1'b0;
			if (h_go) ready[~d_sel] <= 1'b1;   // d_sel has just flipped

			// MONO's pipe: s1 asked, s2 has its word; a bit a cycle
			s2_v <= s1_v; s2_in <= s1_in; s2_lane <= s1_lane; s2_b <= s1_b; s2_th <= s1_th;
			s3_v <= s2_v; s3_in <= s2_in; s3_b <= s2_b; s3_th <= s2_th; s3_word <= ou_col_q; s3_lane <= s2_lane;
			s4_v <= s3_v; s4_in <= s3_in; s4_b <= s3_b; s4_th <= s3_th; s4_byte <= s3_byte;
			if (s4_v) begin
				m_acc[s4_b] <= s4_bit;
				m_msk[s4_b] <= s4_in;
			end

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

			O_BEAM: if ((!wait_beam || !o_beam_in) && (!vsync || !o_first || vblank))
				ost <= mono ? O_MROW : O_READ;
			// (a strip's rows go out first, so the wait is before them)

			O_READ: begin
				ou_a_addr <= o_w;
				o_in_q <= o_in;
				o_vword_q <= o_vword;
				o_rowin_q <= (o_row <= o_rows1);
				ost <= O_DATA;
			end

			O_DATA: ost <= O_WRITE;    // the word is read

			O_WRITE: begin
				if (!m_cyc_o) begin
					o_word <= ou_col_q;
					// clear it behind the read
					ou_b_addr <= o_w;
					ou_col_we <= 4'b1111;
					ou_col_d <= {4{o_clear}};
					ou_dep_we <= 4'b1111;
					ou_dep_d <= {64{1'b1}};
					// rows past the strip's own are only cleared: the
					// whole buffer is then clear to this strip's colour
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
				if (o_w == 11'd1279) ost <= O_DONE;     // the whole buffer: out and clear
				else ost <= O_READ;
			end

			O_DONE: begin
				ready[o_sel] <= 1'b0;
				o_clr_set <= 1'b1;
				o_clr_buf <= o_sel;
				o_sel <= ~o_sel;
				o_busy <= 1'b0;
				strips <= strips + 32'd1;
				ost <= O_IDLE;
			end

			// --- MONO: a row at a time, a screen word at a time ---
			O_MROW: begin
				if (m_rowin) begin
					m_dw <= m_sx0[10:5]; m_dw_end <= m_sx1[10:5];
					m_b <= 5'd0; m_issue <= 1'b1; m_acc <= 32'd0; m_msk <= 32'd0;
					ost <= O_MPIX;
				end else if (o_row == o_rows1) begin
					o_w <= 11'd0;
					ost <= O_MCLR;
				end else o_row <= o_row + 4'd1;
			end

			O_MPIX: begin
				// ask for bit m_b's pixel; take in what came two cycles on
				if (m_issue) begin
					ou_a_addr <= m_addr;
					s1_v <= 1'b1;
					s1_in <= m_pin;
					s1_lane <= m_px[1:0];
					s1_b <= m_b;
					s1_th <= m_th;
					m_b <= m_b + 5'd1;
					if (m_b == 5'd31) begin m_issue <= 1'b0; m_drain <= 3'd4; end
				end else begin
					s1_v <= 1'b0;
					m_drain <= m_drain - 3'd1;
					if (m_drain == 3'd0) begin
						m_rmw <= 1'b0;
						ost <= O_MRD;
					end
				end
			end

			O_MRD: begin
				// a word the rectangle covers in part is read and merged
				if (m_msk == 32'd0) ost <= O_MNEXT;
				else if (m_msk == 32'hffffffff || m_rmw) ost <= O_MWR;
				else if (!m_cyc_o) begin
					m_cyc_o <= 1'b1; m_stb_o <= 1'b1; m_we_o <= 1'b0; m_sel_o <= 4'b1111;
					m_adr_o <= vbase + {15'd0, m_vw, 2'd0};
				end else if (m_ack_i) begin
					m_cyc_o <= 1'b0; m_stb_o <= 1'b0;
					m_old <= m_dat_i;
					m_rmw <= 1'b1;
				end
			end

			O_MWR: begin
				if (!m_cyc_o) begin
					m_cyc_o <= 1'b1; m_stb_o <= 1'b1; m_we_o <= 1'b1; m_sel_o <= 4'b1111;
					m_adr_o <= vbase + {15'd0, m_vw, 2'd0};
					m_dat_o <= m_rmw ? ((m_old & ~m_msk) | (m_acc & m_msk)) : m_acc;
				end else if (m_ack_i) begin
					m_cyc_o <= 1'b0; m_stb_o <= 1'b0; m_we_o <= 1'b0;
					ost <= O_MNEXT;
				end
			end

			O_MNEXT: begin
				if (m_dw == m_dw_end) begin
					o_row <= o_row + 4'd1;
					ost <= (o_row == o_rows1) ? O_MCLR : O_MROW;
					o_w <= 11'd0;
				end else begin
					m_dw <= m_dw + 6'd1;
					m_b <= 5'd0; m_issue <= 1'b1; m_acc <= 32'd0; m_msk <= 32'd0;
					ost <= O_MPIX;
				end
			end

			O_MCLR: begin
				ou_b_addr <= o_w;
				ou_col_we <= 4'b1111;
				ou_col_d <= {4{o_clear}};
				ou_dep_we <= 4'b1111;
				ou_dep_d <= {64{1'b1}};
				o_w <= o_w + 11'd1;
				if (o_w == 11'd1279) ost <= O_DONE;
			end

			default: ost <= O_IDLE;
			endcase
		end
	end

	// ------------------------------------------------------------------
	// the slave port
	// ------------------------------------------------------------------

	wire busy = !f_empty || (st != S_IDLE) || f_pop || rpend || p0_v || p1_v || p2_v || h_go ||
	            (ready != 2'b00) || o_busy || o_clr_set;

	always @(posedge clk) begin
		if (rst) begin
			wb_ack_o <= 1'b0;
			wb_dat_o <= 32'd0;
			wait_beam <= 1'b0;
			vsync <= 1'b0;
			mono <= 1'b0;
			win_x <= 10'd0; win_y <= 9'd0;
			clip_x0 <= 9'd0; clip_x1 <= 9'd319; clip_y0 <= 8'd0; clip_y1 <= 8'd239;
			vbase <= 32'h20000000;
			soft_rst <= 1'b0;
		end else begin
			wb_ack_o <= 1'b0;
			soft_rst <= 1'b0;
			if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin
				if (wb_we_i) begin
					case (wb_adr_i[3:0])
					4'd0: begin wait_beam <= wb_dat_i[0]; vsync <= wb_dat_i[1]; mono <= wb_dat_i[2]; wb_ack_o <= 1'b1; end
					4'd6: begin lum[wb_dat_i[7:0]] <= wb_dat_i[12:8]; wb_ack_o <= 1'b1; end
					4'd8: begin win_x <= wb_dat_i[9:0]; win_y <= wb_dat_i[24:16]; wb_ack_o <= 1'b1; end
					4'd9: begin clip_x0 <= wb_dat_i[8:0]; clip_x1 <= wb_dat_i[24:16]; wb_ack_o <= 1'b1; end
					4'd10: begin clip_y0 <= wb_dat_i[7:0]; clip_y1 <= wb_dat_i[23:16]; wb_ack_o <= 1'b1; end
					4'd2: if (!f_full) wb_ack_o <= 1'b1;   // the FIFO took it
					4'd3: begin vbase <= wb_dat_i; wb_ack_o <= 1'b1; end
					4'd5: begin soft_rst <= 1'b1; wb_ack_o <= 1'b1; end
					default: wb_ack_o <= 1'b1;
					endcase
				end else begin
					case (wb_adr_i[3:0])
					4'd0: wb_dat_o <= {29'd0, mono, vsync, wait_beam};
					4'd8: wb_dat_o <= {7'd0, win_y, 6'd0, win_x};
					4'd9: wb_dat_o <= {7'd0, clip_x1, 7'd0, clip_x0};
					4'd10: wb_dat_o <= {8'd0, clip_y1, 8'd0, clip_y0};
					4'd1: wb_dat_o <= {6'd0, 10'd512 - f_n, 14'd0, bad_cmd, busy};
					4'd3: wb_dat_o <= vbase;
					4'd4: wb_dat_o <= strips;
					4'd7: wb_dat_o <= 32'h5A545249;
					default: wb_dat_o <= 32'd0;
					endcase
					wb_ack_o <= 1'b1;
				end
			end
		end
	end

endmodule
