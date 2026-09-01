/*
 * tb_gpu_blit -- differential test of gpu_blit_wb.
 *
 * Runs the reference and the modified blitter SIDE BY SIDE off the
 * same stimulus and compares every framebuffer write, cycle by cycle
 * of transaction order, plus the busy/idle handshake.
 *
 * Written after a change to the clip path silently hung the blitter on
 * hardware -- the screen went blank after wm loaded and zgfx reported
 * "blitter wait timed out". Nothing in the tree caught it, because
 * nothing tested this module at all. A differential test is the right
 * shape here: the reference is the behaviour that works, and the only
 * question worth answering is whether the new one still does exactly
 * that.
 *
 *   iverilog -g2005 -o /tmp/tb_gpu_blit rtl/tb/tb_gpu_blit.v \
 *       rtl/gpu/gpu_blit.v
 *   vvp /tmp/tb_gpu_blit
 *
 * To diff against a candidate, compile the candidate under a different
 * module name (see REF_ONLY below) -- the Makefile target `make
 * test_blit` does this.
 */

`timescale 1ns / 1ps

module tb_gpu_blit;

	localparam real CLK_HALF_NS = 10.416667;   // 48MHz

	reg clk = 0;
	reg rst = 1;

	always #(CLK_HALF_NS) clk = ~clk;

	// -- wishbone slave side, driven by the test --
	reg [31:0] wb_adr, wb_dat;
	reg wb_we, wb_stb, wb_cyc;
	wire wb_ack_a, wb_ack_b;
	wire [31:0] wb_dat_a, wb_dat_b;

	// -- framebuffer master side --
	wire m_cyc_a, m_stb_a, m_we_a; wire [3:0] m_sel_a;
	wire [31:0] m_adr_a, m_dat_a;
	wire m_cyc_b, m_stb_b, m_we_b; wire [3:0] m_sel_b;
	wire [31:0] m_adr_b, m_dat_b;

	wire s_cyc_a, s_stb_a, s_we_a; wire [3:0] s_sel_a; wire [31:0] s_adr_a;
	wire s_cyc_b, s_stb_b, s_we_b; wire [3:0] s_sel_b; wire [31:0] s_adr_b;

	wire [11:0] g_adr_a, g_adr_b;
	wire busy_a, busy_b;

	reg m_ack_a, m_ack_b, s_ack_a, s_ack_b;
	reg [31:0] m_dat_i_a, m_dat_i_b;
	// main-memory source model for the s port (copy mode with
	// CTRL_SRCMEM). Read-only, shared contents, so a divergence between
	// the two DUTs can only be theirs.
	reg [31:0] s_dat_i_a, s_dat_i_b;
	reg [31:0] mem_s [0:4095];

	integer errors = 0;
	integer writes_a = 0, writes_b = 0;
	integer i;

	// Framebuffer models. Separate arrays so a divergence shows up as
	// a content difference and not just a transaction-order one.
	reg [31:0] fb_a [0:4095];
	reg [31:0] fb_b [0:4095];

	gpu_blit_wb dut_a (
		.clk(clk), .rst(rst),
		.wb_cyc_i(wb_cyc), .wb_stb_i(wb_stb), .wb_we_i(wb_we),
		.wb_sel_i(4'hF), .wb_adr_i(wb_adr), .wb_dat_i(wb_dat),
		.wb_ack_o(wb_ack_a), .wb_dat_o(wb_dat_a),
		.m_cyc_o(m_cyc_a), .m_stb_o(m_stb_a), .m_we_o(m_we_a),
		.m_sel_o(m_sel_a), .m_adr_o(m_adr_a), .m_dat_o(m_dat_a),
		.m_dat_i(m_dat_i_a), .m_ack_i(m_ack_a),
		.s_cyc_o(s_cyc_a), .s_stb_o(s_stb_a), .s_we_o(s_we_a),
		.s_sel_o(s_sel_a), .s_adr_o(s_adr_a), .s_dat_i(s_dat_i_a),
		.s_ack_i(s_ack_a),
		.glyph_addr_o(g_adr_a), .glyph_data_i(8'hA5), .busy(busy_a)
	);

`ifndef REF_ONLY
	gpu_blit_cand dut_b (
		.clk(clk), .rst(rst),
		.wb_cyc_i(wb_cyc), .wb_stb_i(wb_stb), .wb_we_i(wb_we),
		.wb_sel_i(4'hF), .wb_adr_i(wb_adr), .wb_dat_i(wb_dat),
		.wb_ack_o(wb_ack_b), .wb_dat_o(wb_dat_b),
		.m_cyc_o(m_cyc_b), .m_stb_o(m_stb_b), .m_we_o(m_we_b),
		.m_sel_o(m_sel_b), .m_adr_o(m_adr_b), .m_dat_o(m_dat_b),
		.m_dat_i(m_dat_i_b), .m_ack_i(m_ack_b),
		.s_cyc_o(s_cyc_b), .s_stb_o(s_stb_b), .s_we_o(s_we_b),
		.s_sel_o(s_sel_b), .s_adr_o(s_adr_b), .s_dat_i(s_dat_i_b),
		.s_ack_i(s_ack_b),
		.glyph_addr_o(g_adr_b), .glyph_data_i(8'hA5), .busy(busy_b)
	);
`endif

	// -- framebuffer slaves: single wait state, so the blitter's
	//    handshake is actually exercised rather than short-circuited --
	reg [1:0] wa, wb_;

	always @(posedge clk) begin
		if (rst) begin
			m_ack_a <= 0; wa <= 0;
		end else begin
			m_ack_a <= 0;
			if (m_cyc_a && m_stb_a && !m_ack_a) begin
				if (wa == 1) begin
					m_ack_a <= 1; wa <= 0;
					m_dat_i_a <= fb_a[m_adr_a[13:2]];
					if (m_we_a) begin
						fb_a[m_adr_a[13:2]] <= m_dat_a;
						writes_a = writes_a + 1;
					end
				end else wa <= wa + 1;
			end else wa <= 0;
		end
	end

	always @(posedge clk) begin
		if (rst) begin
			m_ack_b <= 0; wb_ <= 0;
		end else begin
			m_ack_b <= 0;
			if (m_cyc_b && m_stb_b && !m_ack_b) begin
				if (wb_ == 1) begin
					m_ack_b <= 1; wb_ <= 0;
					m_dat_i_b <= fb_b[m_adr_b[13:2]];
					if (m_we_b) begin
						fb_b[m_adr_b[13:2]] <= m_dat_b;
						writes_b = writes_b + 1;
					end
				end else wb_ <= wb_ + 1;
			end else wb_ <= 0;
		end
	end

	always @(posedge clk) begin
		s_ack_a <= s_cyc_a && s_stb_a && !s_ack_a;
		s_ack_b <= s_cyc_b && s_stb_b && !s_ack_b;
		s_dat_i_a <= mem_s[s_adr_a[13:2]];
		s_dat_i_b <= mem_s[s_adr_b[13:2]];
	end

	task wr;
		input [3:0] a;
		input [31:0] d;
		begin
			@(posedge clk);
			wb_adr <= { 28'h0, a };
			wb_dat <= d;
			wb_we <= 1; wb_stb <= 1; wb_cyc <= 1;
			@(posedge clk);
			while (!wb_ack_a) @(posedge clk);
			wb_we <= 0; wb_stb <= 0; wb_cyc <= 0;
			@(posedge clk);
		end
	endtask

	// Run one fill and wait for both to go idle. `gap` is the number
	// of idle cycles between the last parameter write and the START
	// write -- 0 exercises the tightest possible back-to-back case,
	// which is what a fast bus master could produce.
	task fill;
		input [31:0] x, y, w, h;
		input [31:0] pat;
		input integer gap;
		input clipen;
		integer t;
		begin
			wr(4'd2, x); wr(4'd3, y); wr(4'd4, w); wr(4'd5, h);
			wr(4'd6, pat);
			for (t = 0; t < gap; t = t + 1) @(posedge clk);
			wr(4'd0, { 28'h0, 1'b0, clipen, 1'b1, 1'b1 });  // clip|fill|start

			t = 0;
			while ((busy_a
`ifndef REF_ONLY
				|| busy_b
`endif
				) && t < 200000) begin
				@(posedge clk);
				t = t + 1;
			end

			if (t >= 200000) begin
				$display("FAIL x=%0d y=%0d w=%0d h=%0d gap=%0d: BLITTER STUCK",
					x, y, w, h, gap);
				errors = errors + 1;
			end
			repeat (4) @(posedge clk);
		end
	endtask

	/* One glyph blit. The glyph ROM model returns a fixed byte for
	 * every address, which is enough for a differential test -- both
	 * DUTs see identical data, so any difference is theirs. */
	task glyph;
		input [31:0] x, y, gw, gh;
		input [31:0] fg, bg;
		input integer gap;
		integer t;
		begin
			wr(4'd2, x); wr(4'd3, y);
			wr(4'd7, 32'h0);     // glyph_addr
			wr(4'd8, gw); wr(4'd9, gh);
			wr(4'd10, fg); wr(4'd11, bg);
			for (t = 0; t < gap; t = t + 1) @(posedge clk);
			wr(4'd0, { 27'h0, 1'b0, 1'b1, 1'b0, 1'b0, 1'b1 }); // glyph|start

			t = 0;
			while ((busy_a
`ifndef REF_ONLY
				|| busy_b
`endif
				) && t < 200000) begin
				@(posedge clk);
				t = t + 1;
			end
			if (t >= 200000) begin
				$display("FAIL glyph x=%0d y=%0d w=%0d h=%0d: STUCK", x, y, gw, gh);
				errors = errors + 1;
			end
			repeat (4) @(posedge clk);
		end
	endtask

	/* One copy blit: VRAM->VRAM (srcmem=0, source read on the m port)
	 * or main memory->VRAM (srcmem=1, source read on the s port), with
	 * an arbitrary window bit offset, optional SRC_PRIME_ZERO, a ROP,
	 * and optional cookie-cut against src_b. This is the path that goes
	 * through the barrel shifter (mem_blend), which the fills and glyph
	 * blits above never touch. */
	task copy;
		input [31:0] x, y, w, h;
		input [31:0] src, stride, shift, srcb;
		input srcmem, cookie;
		input [1:0] rop;
		input clipen;
		input integer gap;
		integer t;
		begin
			wr(4'd2, x); wr(4'd3, y); wr(4'd4, w); wr(4'd5, h);
			wr(4'd12, src); wr(4'd13, stride); wr(4'd14, shift);
			wr(4'd15, srcb);
			for (t = 0; t < gap; t = t + 1) @(posedge clk);
			wr(4'd0, { 23'h0, 1'b0, cookie, rop, srcmem, 1'b0, clipen,
				1'b0, 1'b1 });   // dither=0|cookie|rop|srcmem|glyph=0|clip|fill=0|start

			t = 0;
			while ((busy_a
`ifndef REF_ONLY
				|| busy_b
`endif
				) && t < 400000) begin
				@(posedge clk);
				t = t + 1;
			end
			if (t >= 400000) begin
				$display("FAIL copy x=%0d y=%0d w=%0d h=%0d shift=%0h srcmem=%0d cookie=%0d: STUCK",
					x, y, w, h, shift, srcmem, cookie);
				errors = errors + 1;
			end
			repeat (4) @(posedge clk);
		end
	endtask

	task compare;
		input [255:0] what;
		integer k;
		integer diffs;
		begin
`ifndef REF_ONLY
			diffs = 0;
			for (k = 0; k < 4096; k = k + 1)
				if (fb_a[k] !== fb_b[k]) diffs = diffs + 1;
			if (diffs != 0 || writes_a != writes_b) begin
				$display("FAIL %0s: %0d differing words, %0d vs %0d writes",
					what, diffs, writes_a, writes_b);
				errors = errors + 1;
			end else begin
				$display("  ok  %0s (%0d writes, framebuffers identical)",
					what, writes_a);
			end
`else
			$display("  ref %0s: %0d writes", what, writes_a);
`endif
		end
	endtask

	initial begin
		wb_adr = 0; wb_dat = 0; wb_we = 0; wb_stb = 0; wb_cyc = 0;
		m_ack_a = 0; m_ack_b = 0; s_ack_a = 0; s_ack_b = 0;
		m_dat_i_a = 0; m_dat_i_b = 0; wa = 0; wb_ = 0;
		for (i = 0; i < 4096; i = i + 1) begin
			fb_a[i] = 32'hDEAD0000 + i;
			fb_b[i] = 32'hDEAD0000 + i;
			// something with structure in every bit position, so a
			// shift by the wrong amount cannot look like the right one
			mem_s[i] = (32'h9E3779B9 * (i + 1)) ^ (i << 7) ^ 32'h5A5A00FF;
		end
		s_dat_i_a = 0; s_dat_i_b = 0;

		repeat (8) @(posedge clk);
		rst = 0;
		repeat (4) @(posedge clk);

		$display("");
		$display("=== gpu_blit differential test ===");

		// A plain aligned fill.
		fill(32'd0, 32'd0, 32'd32, 32'd4, 32'hFFFFFFFF, 4, 1'b1);
		compare("aligned fill 32x4 at (0,0)");

		// Unaligned in x, so the left/right masks are exercised.
		fill(32'd5, 32'd2, 32'd53, 32'd3, 32'hAAAAAAAA, 4, 1'b1);
		compare("unaligned fill 53x3 at (5,2)");

		// Clipped against the right edge.
		fill(32'd600, 32'd1, 32'd200, 32'd2, 32'hFFFFFFFF, 4, 1'b1);
		compare("fill clipped at the right edge");

		// Clipped against the bottom edge.
		fill(32'd0, 32'd470, 32'd64, 32'd50, 32'hFFFFFFFF, 4, 1'b1);
		compare("fill clipped at the bottom edge");

		// Entirely off screen -- must abort, not hang.
		fill(32'd700, 32'd10, 32'd8, 32'd8, 32'hFFFFFFFF, 4, 1'b1);
		compare("fill entirely off screen");

		// Degenerate sizes.
		fill(32'd10, 32'd10, 32'd0, 32'd8, 32'hFFFFFFFF, 4, 1'b1);
		compare("zero width");
		fill(32'd10, 32'd10, 32'd8, 32'd0, 32'hFFFFFFFF, 4, 1'b1);
		compare("zero height");

		// Clipping disabled.
		fill(32'd8, 32'd8, 32'd16, 32'd2, 32'h0F0F0F0F, 4, 1'b0);
		compare("clip disabled");

		// THE CASE THAT BROKE THE SCREEN: START immediately after the
		// last parameter write, with no idle cycles between. A
		// pipeline register that assumes the parameters have settled
		// fails exactly here and nowhere else.
		fill(32'd16, 32'd16, 32'd48, 32'd2, 32'hFFFFFFFF, 0, 1'b1);
		compare("START with zero gap after parameters");
		fill(32'd24, 32'd20, 32'd40, 32'd2, 32'h12345678, 1, 1'b1);
		compare("START one cycle after parameters");

		// Back-to-back fills: the second must not clip against the
		// first one's coordinates.
		fill(32'd0, 32'd100, 32'd64, 32'd2, 32'hFFFFFFFF, 0, 1'b1);
		fill(32'd0, 32'd104, 32'd64, 32'd2, 32'h00000000, 0, 1'b1);
		compare("back-to-back fills, no gap");

		// -- glyph mode --
		//
		// Its own coverage, because the mask generator
		// `(1 << work_glyph_w) - 1` is a 32-bit variable shifter on a
		// value that is never more than 32, and narrowing it is
		// exactly the kind of change that needs a test in front of it.
		// Widths either side of a word boundary, and the degenerate
		// and out-of-range cases the shifter has to keep handling.

		glyph(32'd0,  32'd0,  32'd5,  32'd8, 32'h1, 32'h0, 4);
		compare("glyph 5x8 at (0,0), word aligned");

		glyph(32'd29, 32'd4,  32'd5,  32'd8, 32'h1, 32'h0, 4);
		compare("glyph 5x8 straddling a word boundary");

		glyph(32'd10, 32'd10, 32'd8,  32'd8, 32'h1, 32'h1, 4);
		compare("glyph 8x8, fg == bg");

		glyph(32'd60, 32'd20, 32'd16, 32'd8, 32'h0, 32'h1, 4);
		compare("glyph 16 wide, inverse");

		glyph(32'd0,  32'd30, 32'd1,  32'd1, 32'h1, 32'h0, 4);
		compare("glyph 1x1, smallest legal");

		glyph(32'd0,  32'd40, 32'd32, 32'd2, 32'h1, 32'h0, 4);
		compare("glyph 32 wide -- mask is exactly all-ones");

		glyph(32'd0,  32'd50, 32'd33, 32'd2, 32'h1, 32'h0, 4);
		compare("glyph 33 wide -- shift past the word, mask saturates");

		glyph(32'd0,  32'd60, 32'd0,  32'd2, 32'h1, 32'h0, 4);
		compare("glyph zero width");

		glyph(32'd0,  32'd70, 32'h8000_0000, 32'd1, 32'h1, 32'h0, 4);
		compare("glyph absurd width -- shift amount out of range");

		glyph(32'd8,  32'd80, 32'd5,  32'd8, 32'h1, 32'h0, 0);
		compare("glyph START with zero gap after parameters");


		// ---- copy mode: the barrel-shifter path ----
		//
		// Source VRAM rows are whatever the fills above left behind;
		// both DUTs see the same, so any divergence is the shifter's.

		// VRAM -> VRAM, aligned, shift 0
		copy(32'd0,  32'd100, 32'd64, 32'd3, 32'd0,    32'd80, 32'h0,  32'd0, 1'b0, 1'b0, 2'd0, 1'b1, 4);
		compare("vram copy 64x3 shift 0");
		// VRAM -> VRAM, every kind of misalignment
		copy(32'd5,  32'd110, 32'd53, 32'd3, 32'd160,  32'd80, 32'h3,  32'd0, 1'b0, 1'b0, 2'd0, 1'b1, 4);
		compare("vram copy 53x3 at x=5 shift 3");
		copy(32'd37, 32'd120, 32'd100,32'd2, 32'd324,  32'd80, 32'h11, 32'd0, 1'b0, 1'b0, 2'd0, 1'b1, 4);
		compare("vram copy 100x2 at x=37 shift 17");
		copy(32'd1,  32'd130, 32'd31, 32'd2, 32'd400,  32'd80, 32'h1f, 32'd0, 1'b0, 1'b0, 2'd0, 1'b1, 4);
		compare("vram copy 31x2 shift 31");
		// SRC_PRIME_ZERO: window starts with zeros, first word not read
		copy(32'd40, 32'd140, 32'd48, 32'd2, 32'd480,  32'd80, 32'h11d,32'd0, 1'b0, 1'b0, 2'd0, 1'b1, 4);
		compare("vram copy with SRC_PRIME_ZERO");
		// ROPs through the shifter
		copy(32'd8,  32'd150, 32'd70, 32'd2, 32'd560,  32'd80, 32'h9,  32'd0, 1'b0, 1'b0, 2'd1, 1'b1, 4);
		compare("vram copy rop 1");
		copy(32'd8,  32'd160, 32'd70, 32'd2, 32'd640,  32'd80, 32'h9,  32'd0, 1'b0, 1'b0, 2'd2, 1'b1, 4);
		compare("vram copy rop 2");
		copy(32'd8,  32'd170, 32'd70, 32'd2, 32'd720,  32'd80, 32'h9,  32'd0, 1'b0, 1'b0, 2'd3, 1'b1, 4);
		compare("vram copy rop 3");
		// cookie cut, VRAM source: A at src, B at srcb, same shift
		copy(32'd13, 32'd180, 32'd60, 32'd3, 32'd800,  32'd80, 32'h7,  32'd1040, 1'b0, 1'b1, 2'd0, 1'b1, 4);
		compare("vram cookie-cut 60x3 shift 7");
		copy(32'd0,  32'd190, 32'd64, 32'd2, 32'd1280, 32'd80, 32'h0,  32'd1440, 1'b0, 1'b1, 2'd0, 1'b1, 4);
		compare("vram cookie-cut aligned shift 0");

		// main memory -> VRAM (s port). This is the exact path nextpnr
		// reported as critical: bus read data straight into the shifter.
		copy(32'd0,  32'd200, 32'd64, 32'd3, 32'd0,    32'd16, 32'h0,  32'd0, 1'b1, 1'b0, 2'd0, 1'b1, 4);
		compare("mem copy 64x3 shift 0");
		copy(32'd5,  32'd210, 32'd53, 32'd3, 32'd256,  32'd16, 32'h3,  32'd0, 1'b1, 1'b0, 2'd0, 1'b1, 4);
		compare("mem copy 53x3 at x=5 shift 3");
		copy(32'd37, 32'd220, 32'd100,32'd2, 32'd512,  32'd20, 32'h11, 32'd0, 1'b1, 1'b0, 2'd0, 1'b1, 4);
		compare("mem copy 100x2 at x=37 shift 17");
		copy(32'd1,  32'd230, 32'd31, 32'd2, 32'd768,  32'd8,  32'h1f, 32'd0, 1'b1, 1'b0, 2'd0, 1'b1, 4);
		compare("mem copy 31x2 shift 31");
		copy(32'd40, 32'd240, 32'd48, 32'd2, 32'd1024, 32'd12, 32'h11d,32'd0, 1'b1, 1'b0, 2'd0, 1'b1, 4);
		compare("mem copy with SRC_PRIME_ZERO");
		copy(32'd8,  32'd250, 32'd70, 32'd2, 32'd1536, 32'd16, 32'h9,  32'd0, 1'b1, 1'b0, 2'd2, 1'b1, 4);
		compare("mem copy rop 2");
		copy(32'd13, 32'd260, 32'd60, 32'd3, 32'd2048, 32'd16, 32'h7,  32'd3072, 1'b1, 1'b1, 2'd0, 1'b1, 4);
		compare("mem cookie-cut 60x3 shift 7");
		copy(32'd600,32'd270, 32'd100,32'd2, 32'd2304, 32'd16, 32'h5,  32'd0, 1'b1, 1'b0, 2'd0, 1'b1, 4);
		compare("mem copy clipped at right edge");
		copy(32'd0,  32'd280, 32'd64, 32'd2, 32'd2560, 32'd16, 32'h2,  32'd0, 1'b1, 1'b0, 2'd0, 1'b1, 0);
		compare("mem copy START with zero gap");
		copy(32'd0,  32'd290, 32'd64, 32'd2, 32'd2816, 32'd16, 32'h2,  32'd0, 1'b1, 1'b0, 2'd0, 1'b0, 4);
		compare("mem copy clip disabled");
		// back-to-back copies, so the register capture from the previous
		// blit cannot leak into the next
		copy(32'd3,  32'd300, 32'd40, 32'd1, 32'd3200, 32'd16, 32'h1,  32'd0, 1'b1, 1'b0, 2'd0, 1'b1, 0);
		copy(32'd3,  32'd301, 32'd40, 32'd1, 32'd1600, 32'd80, 32'h1e, 32'd0, 1'b0, 1'b0, 2'd0, 1'b1, 0);
		compare("mem copy then vram copy, zero gap");

		$display("");
		if (errors == 0) $display("=== tb_gpu_blit: PASS ===");
		else $display("=== tb_gpu_blit: %0d FAILURE(S) ===", errors);
		$display("");
		$finish;
	end

endmodule
