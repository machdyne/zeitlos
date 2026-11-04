`timescale 1ns / 1ps

// Self-checking testbench for rtl/gpu/gpu_ckfetch.v: chunky game mode's
// picture from main memory (`VRAM_SDRAM; docs/profiles.md, "Lakritz").
//
// gpu_video as tb_chunky.v drives it (eight planes, chunky), its
// graphics port through gpu_ckfetch to a 4,800-word VRAM (two 320x240
// planes) and, behind rtl/arbiter_gfx.v, a main memory that answers
// every beat after a random 1-12 cycles -- with four-word bursts --
// shared with a second master writing elsewhere as fast as it is let.
// EVERY visible pixel of a frame is checked against a model of the
// linear picture: line y at base + 320 y, pixel x its byte x.
//
//   1. chunky from main memory at base A, a whole frame; no line late
//      after the first frame (misses) and 240 lines fetched a frame.
//   2. CKBASE moved to B while a frame is shown: the rest of THAT frame
//      is still A (the base is taken at line 0), the next all B.
//   3. bursts off (BURST 0, single reads): the same picture (a second
//      instance, `CKF_BURST=0).
//   4. chunky off: the two planes from VRAM, and planes 2..7 (rows 240
//      on, past the VRAM) read 0.
//
// Running it -- the trailing-comma patch as the other video benches:
//
//   sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
//       rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
//   iverilog -g2005 -o /tmp/tb_ckfetch.out rtl/gpu/bench/tb_ckfetch.v \
//       /tmp/gpu_video_fix.v rtl/gpu/gpu_ckfetch.v rtl/arbiter_gfx.v
//   vvp /tmp/tb_ckfetch.out
//   (and with -DCKF_BURST=0 for case 3)

`ifndef CKF_BURST
`define CKF_BURST 1
`endif

module tb_ckfetch;

	localparam VWORDS = 4800;
	localparam MWORDS = 65536;          // main memory model: 256 KB
	localparam [31:0] MBASE = 32'h4000_0000;
	localparam [31:0] PIC_A = MBASE + 32'h0000_1000;
	localparam [31:0] PIC_B = MBASE + 32'h0001_8000;

	localparam H_DISP = 640, H_FP = 16, H_PW = 96, H_BP = 48;
	localparam V_DISP = 480, V_FP = 10, V_PW = 2,  V_BP = 33;
	localparam H_START = H_FP + H_PW + H_BP;
	localparam H_STOP  = H_START + H_DISP;
	localparam V_START = V_FP + V_PW + V_BP;
	localparam V_STOP  = V_START + V_DISP;

	reg clk = 0;
	reg pclk = 0;
	reg bclk = 0;
	reg resetn = 0;
	wire rst = !resetn;

	always #10.417 clk  = ~clk;
	always #19.841 pclk = ~pclk;
	always #3.968  bclk = ~bclk;

	reg view_load = 0;
	reg game_en = 0;
	reg game_wrap = 0;
	reg [9:0] view_x = 0;
	reg [9:0] view_y = 0;
	reg color_en = 0;
	reg [2:0] color_np = 0;
	reg [5:0] color_off = 0;
	reg color_chunky = 0;
	reg [31:0] ckbase = 32'h0;

	wire red, green, blue, hsync, vsync, is_visible;
	wire [9:0] x, y;
	wire [14:0] gb_adr;
	wire [31:0] gb_dat;
	wire [9:0] gb_row;
	wire [3:0] dvi_p;
	wire [3:0] dac;
	wire [15:0] frame_ctr;
	wire in_vblank;

	integer errors = 0;

	gpu_video #(.COLOR_AVAIL(1'b1), .GAME_PLANES(8), .CHUNKY_AVAIL(1'b1)) dut (
		.pixel(1'b0),
		.clk(clk), .pclk(pclk), .bclk(bclk), .resetn(resetn),
		.video_mode(2'd0),
		.view_load(view_load), .game_en(game_en), .game_wrap(game_wrap),
		.view_x(view_x), .view_y(view_y),
		.color_en(color_en), .color_np(color_np), .color_off(color_off),
		.color_chunky(color_chunky),
		.pal_we(1'b0), .pal_idx(8'd0), .pal_rgb(12'd0),
		.frame_ctr(frame_ctr), .in_vblank(in_vblank),
		.red(red), .green(green), .blue(blue),
		.hsync(hsync), .vsync(vsync), .dvi_p(dvi_p), .dac(dac),
		.x(x), .y(y), .is_visible(is_visible),
		.gb_row_o(gb_row),
		.gb_adr_o(gb_adr),
		.gb_dat_i(gb_dat)
	);

	// -- VRAM: two planes, rtl/mem/vram.v's graphics port (an
	// out-of-range read is x there: gpu_ckfetch must make it 0) --
	reg [31:0] vram [0:VWORDS-1];
	reg [31:0] vram_q;
	always @(posedge clk)
		vram_q <= (gb_adr < VWORDS) ? vram[gb_adr] : 32'hxxxxxxxx;

	// -- the fetch --
	wire [31:0] f_adr, f_dat;
	wire [3:0]  f_sel;
	wire [2:0]  f_cti;
	wire        f_cyc, f_stb, f_we, f_ack;
	wire [15:0] misses, lines;

	gpu_ckfetch #(.BURST(`CKF_BURST), .VRAM_WORDS(VWORDS)) ckf (
		.clk(clk), .rst(rst),
		.en(color_chunky && color_en && game_en),
		.base(ckbase),
		.gb_row(gb_row),
		.gb_adr(gb_adr), .gb_dat(gb_dat), .vram_dat(vram_q),
		.m_adr_o(f_adr), .m_cyc_o(f_cyc), .m_stb_o(f_stb), .m_cti_o(f_cti),
		.m_sel_o(f_sel), .m_we_o(f_we), .m_dat_i(f_dat), .m_ack_i(f_ack),
		.misses(misses), .lines(lines)
	);

	// -- a second master: writes past the pictures, as fast as it can --
	reg  [31:0] w_adr = MBASE + 32'h0003_0000;
	reg  [31:0] w_dat = 32'h0;
	reg         w_cyc = 0, w_stb = 0;
	wire        w_ack;
	reg         w_on = 0;
	integer     w_count = 0;
	always @(posedge clk) begin
		if (!w_cyc) begin
			if (w_on) begin
				w_cyc <= 1'b1;
				w_stb <= 1'b1;
				w_dat <= w_dat + 32'h01010101;
			end
		end else if (w_ack) begin
			w_cyc <= 1'b0;
			w_stb <= 1'b0;
			w_adr <= (w_adr >= MBASE + 32'h0003_F000) ? (MBASE + 32'h0003_0000) : (w_adr + 32'd4);
			w_count = w_count + 1;
		end
	end

	// -- the gfx port's arbiter, as sysctl wires it (no audio) --
	wire [31:0] s_adr, s_dat_o;
	reg  [31:0] s_dat_i;
	wire [3:0]  s_sel;
	wire [2:0]  s_cti;
	wire        s_we, s_stb, s_cyc;
	reg         s_ack;
	wire [1:0]  gm;

	wb_arbiter_gfx arb (
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
		.s_adr_o(s_adr), .s_dat_o(s_dat_o), .s_dat_i(s_dat_i),
		.s_we_o(s_we), .s_sel_o(s_sel), .s_cti_o(s_cti),
		.s_stb_o(s_stb), .s_cyc_o(s_cyc), .s_ack_i(s_ack),
		.master(gm)
	);

	// -- main memory: every beat after 1-12 cycles; a burst's beats
	// follow the master's address. Checks the burst rule the SDRAM
	// controller has: CTI 010 only at a 16-byte aligned address, and
	// four beats taken. --
	reg [31:0] mem [0:MWORDS-1];
	integer lat = 0;
	reg [31:0] rnd = 32'h1234_5678;
	reg gap = 0;
	integer beats = 0;
	reg in_burst = 0;
	wire [15:0] mi = s_adr[17:2];
	always @(posedge clk) begin
		s_ack <= 1'b0;
		rnd = rnd * 32'd1103515245 + 32'd12345;
		if (!s_cyc) begin
			if (in_burst && beats != 0 && beats != 4) begin
				$display("FAIL: a burst of %0d beats", beats);
				errors = errors + 1;
			end
			in_burst <= 1'b0;
			beats = 0;
			gap <= 1'b0;
			lat = 0;
		end else if (s_stb && !s_ack) begin
			if (s_cti == 3'b010 && beats == 0) begin
				in_burst <= 1'b1;
				if (s_adr[3:0] != 4'h0) begin
					$display("FAIL: burst at %h, not 16-byte aligned", s_adr);
					errors = errors + 1;
				end
			end
			if (lat == 0) lat = 1 + (rnd[19:16] % 12);
			lat = lat - 1;
			if (lat == 0) begin
				s_ack <= 1'b1;
				if (s_we) mem[mi] <= s_dat_o;
				else s_dat_i <= mem[mi];
				beats = beats + 1;
				if (s_adr < MBASE || s_adr >= MBASE + MWORDS * 4) begin
					$display("FAIL: an access at %h, outside the memory", s_adr);
					errors = errors + 1;
				end
			end
		end
	end

	// -- the model --
	reg m_check = 0;
	reg m_ck = 0;
	reg [31:0] m_base;
	reg [2:0] m_np;
	integer mism;
	integer px, py, c0, r0, k;
	reg [7:0] expect_idx, raw, mask;
	reg [31:0] wd;
	integer wi;

	function [7:0] lbyte;
		input [31:0] b;
		input integer c;
		input integer r;
		integer a;
		reg [31:0] w;
		begin
			a = (b - MBASE) / 4 + r * 80 + c / 4;
			w = mem[a];
			lbyte = w >> (8 * (c % 4));
		end
	endfunction

	// the planes as z_color_16 lays them out (plane k's offsets 01,
	// 10, 11: dx, dy, both); past VRAM (rows 240 on) 0
	function pbit;
		input integer kk;
		input integer c;
		input integer r;
		integer ck, rk, j, a;
		begin
			ck = c; rk = r;
			if (kk >= 4) begin
				pbit = 1'b0;
			end else begin
				j = kk;
				if (j == 1 || j == 3) ck = ck + 320;
				if (j == 2 || j == 3) rk = rk + 240;
				ck = ck % 640; rk = rk % 480;
				a = rk * 20 + ck / 32;
				pbit = (a < VWORDS) ? vram[a][ck % 32] : 1'b0;
			end
		end
	endfunction

	always @(negedge pclk) begin
		if (m_check && dut.is_visible) begin
			px = dut.hc - H_START;
			py = dut.vc - V_START;
			c0 = px / 2;
			r0 = py / 2;
			if (m_ck) begin
				raw = lbyte(m_base, c0, r0);
				mask = 8'hff;
			end else begin
				for (k = 0; k < 8; k = k + 1) begin
					raw[k] = pbit(k, c0, r0);
					mask[k] = (k <= m_np);
				end
			end
			expect_idx = raw & mask;
			if (dut.cidx_o !== expect_idx) begin
				if (mism < 8)
					$display("FAIL: pixel (%0d,%0d): index %h, expected %h",
						px, py, dut.cidx_o, expect_idx);
				mism = mism + 1;
			end
		end
	end

	task set_cfg;
		input col;
		input [2:0] np;
		input [5:0] off;
		begin
			@(posedge clk);
			game_en   <= 1'b1;
			game_wrap <= 1'b0;
			view_x    <= 0;
			view_y    <= 0;
			color_en  <= col;
			color_np  <= np;
			color_off <= off;
			view_load <= ~view_load;
		end
	endtask

	task goto;
		input [10:0] want_vc;
		input [10:0] want_hc;
		begin
			while (!(dut.vc == want_vc && dut.hc == want_hc))
				@(posedge pclk);
		end
	endtask

	task next_frame;
		begin
			goto(V_STOP - 1, H_STOP - 1);
			@(posedge pclk);
			@(posedge pclk);
		end
	endtask

	// a whole frame against the model, from the next frame boundary
	task check_frame;
		input [8*64-1:0] what;
		input [31:0] base;
		begin
			repeat (8) @(posedge pclk);
			next_frame();
			m_base = base;
			m_np = color_np;
			m_ck = color_chunky && color_en;
			mism = 0;
			m_check = 1;
			goto(V_STOP - 1, H_STOP - 2);
			m_check = 0;
			if (mism != 0) begin
				$display("FAIL: %0s: %0d mismatching pixels", what, mism);
				errors = errors + 1;
			end else
				$display("ok:   %0s", what);
		end
	endtask

	integer i;
	reg [31:0] seed;
	reg [15:0] mis0, lin0;
	integer w0;

	initial begin
		seed = 32'h8765_4321;
		for (i = 0; i < MWORDS; i = i + 1) begin
			seed = seed * 32'd1664525 + 32'd1013904223;
			mem[i] = seed ^ (i * 32'h9e37_79b9);
		end
		for (i = 0; i < VWORDS; i = i + 1) begin
			seed = seed * 32'd1664525 + 32'd1013904223;
			vram[i] = seed;
		end

		resetn = 0;
		repeat (8) @(posedge pclk);
		resetn = 1;
		w_on = 1;

		// ---- 1: chunky from main memory ----
		ckbase = PIC_A;
		color_chunky = 1;
		set_cfg(1, 3'd7, 6'b11_10_01);
		next_frame();
		mis0 = misses;
		lin0 = lines;
		w0 = w_count;
		check_frame("chunky from main memory, base A", PIC_A);
		if (misses != mis0) begin
			$display("FAIL: %0d lines late in a frame", misses - mis0);
			errors = errors + 1;
		end else
			$display("ok:   no line late (%0d before, the first frame's)", mis0);
		// two frames' worth of lines: the one checked and the one before
		if (lines - lin0 < 16'd240 || lines - lin0 > 16'd482) begin
			$display("FAIL: %0d lines fetched", lines - lin0);
			errors = errors + 1;
		end else
			$display("ok:   %0d lines fetched; the other master got %0d writes in",
				lines - lin0, w_count - w0);

		// ---- 2: CKBASE moved while a frame is shown ----
		next_frame();
		goto(V_START + 200, 0);     // line 100 of the game frame
		ckbase = PIC_B;
		m_base = PIC_A;
		m_ck = 1;
		m_np = 3'd7;
		mism = 0;
		m_check = 1;
		goto(V_STOP - 1, H_STOP - 2);
		m_check = 0;
		if (mism != 0) begin
			$display("FAIL: the frame CKBASE moved in: %0d pixels not A", mism);
			errors = errors + 1;
		end else
			$display("ok:   the frame CKBASE moved in stays A");
		@(posedge pclk);
		@(posedge pclk);
		m_base = PIC_B;
		mism = 0;
		m_check = 1;
		goto(V_STOP - 1, H_STOP - 2);
		m_check = 0;
		if (mism != 0) begin
			$display("FAIL: the frame after: %0d pixels not B", mism);
			errors = errors + 1;
		end else
			$display("ok:   the frame after is B, whole");

		// ---- 4: chunky off: the two planes, nothing past them ----
		color_chunky = 0;
		set_cfg(1, 3'd1, 6'b11_10_01);
		check_frame("planes 0 and 1 from VRAM", 0);
		set_cfg(1, 3'd7, 6'b11_10_01);
		check_frame("planes 2..7 (past VRAM) read 0", 0);

		if (errors == 0)
			$display("RESULT: PASS");
		else
			$display("RESULT: FAIL (%0d errors)", errors);
		$finish;
	end

	// a hang
	initial begin
		#400_000_000;
		$display("RESULT: FAIL (timeout)");
		$finish;
	end

endmodule
