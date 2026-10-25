`timescale 1ns / 1ps

// Self-checking testbench for desktop colour's exact path in
// rtl/gpu/gpu_video.v (DESK_AVAIL; docs/color.md, "Desktop colour"),
// and for the 3-bit VGA DAC (VGA_DAC3).
//
// tb_colwin.v's method: a two-sheet VRAM filled with a fixed
// pseudo-random pattern, a model of what every visible pixel of a whole
// frame must be, and a check of every one.
//
// With the colour window's DESK bit set, inside the rectangle:
//
//   accent (sheet 1 at (x, y)) set, pointer not on it
//       -> the palette, index { accent, desktop bit } (0x10 | d); the
//          index's other bits are masked to 0
//   otherwise
//       -> the monochrome pixel: desktop bit ^ pointer ^ paper, through
//          the phosphor's ink at its brightness, exactly
//
// Checked here:
//
//   1. the whole screen, white, level 7: DDMI and VGA.
//   2. amber at level 11: the untinted pixels carry amber's exact ink.
//   3. paper: untinted pixels inverted; tinted ones the palette.
//   4. the pointer (the `pixel` input): monochrome XOR everywhere,
//      also over accent pixels.
//   5. a smaller window: outside it plain monochrome.
//   6. DESK off: the ordinary 32-colour window (tb_colwin.v's index).
//   7. game mode with DESK asked for: ignored.
//
// Build variants:
//
//   default           DESK_AVAIL 1, eight planes
//   -DNODESK          DESK_AVAIL 0: the DESK bit does nothing (case 1
//                     then expects the ordinary window)
//   -DNPLANES=6       six planes, DESK_AVAIL 1: likewise does nothing
//   -DDAC3            VGA_DAC3 1: red/green/blue and the _lo bits are
//                     checked as a 3-bit value
//   -DGPU_DDMI -DTB_STUBS (and the DDMI sources): the 8-bit TMDS inputs
//
//   sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
//       rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
//   iverilog -g2005 -DGPU_DDMI -DTB_STUBS -o /tmp/tb_desk.out \
//       rtl/gpu/bench/tb_deskcol.v /tmp/gpu_video_fix.v \
//       rtl/gpu/gpu_ddmi.v rtl/gpu/tmds_encoder.v rtl/gpu/ddmi_island.v
//   vvp /tmp/tb_desk.out

`ifndef NPLANES
`define NPLANES 8
`endif

module tb_deskcol;

	localparam NP = `NPLANES;
	localparam WORDS = (NP == 8) ? 19200 : 14400;
	localparam S1_ROWS = (NP == 8) ? 480 : 240;
	localparam NPAL = (NP == 8) ? 256 : 64;
`ifdef NODESK
	localparam DESK = 1'b0;
`else
	localparam DESK = 1'b1;
`endif
	// the exact path exists only with eight planes
	localparam DESK_REAL = DESK && (NP == 8);
`ifdef DAC3
	localparam DAC3 = 1'b1;
`else
	localparam DAC3 = 1'b0;
`endif

	localparam H_DISP = 640, H_FP = 16, H_PW = 96, H_BP = 48;
	localparam V_DISP = 480, V_FP = 10, V_PW = 2,  V_BP = 33;
	localparam H_START = H_FP + H_PW + H_BP;   // 160
	localparam H_STOP  = H_START + H_DISP;     // 800
	localparam V_START = V_FP + V_PW + V_BP;   // 45
	localparam V_STOP  = V_START + V_DISP;     // 525

	reg clk = 0;
	reg pclk = 0;
	reg bclk = 0;
	reg resetn = 0;

	reg [1:0] video_mode = 2'd0;
	reg [3:0] video_bright = 4'd7;
	reg [31:0] gb_dat_i = 32'h0;

	reg view_load = 0;
	reg game_en = 0;
	reg game_wrap = 0;
	reg [9:0] view_x = 0;
	reg [9:0] view_y = 0;

	reg color_en = 0;
	reg [2:0] color_np = 0;
	reg [5:0] color_off = 0;
	reg pal_we = 0;
	reg [7:0] pal_idx = 0;
	reg [11:0] pal_rgb = 0;

	reg win_en = 0;
	reg win_desk = 0;
	reg [9:0] win_x0 = 0, win_y0 = 0, win_x1 = 0, win_y1 = 0;

	wire red, green, blue, hsync, vsync, is_visible;
	wire [1:0] red_lo, green_lo, blue_lo;
	wire [9:0] x, y;
	wire [14:0] gb_adr_o;
	wire [3:0] dvi_p;
	wire [3:0] dac;
	wire [15:0] frame_ctr;
	wire in_vblank;

	reg cursor_test = 0;
	wire cursor_pix = cursor_test && x[3] && !y[2];

	integer errors = 0;

	always #10.417 clk  = ~clk;
	always #19.841 pclk = ~pclk;
	always #3.968  bclk = ~bclk;

	gpu_video #(.COLOR_AVAIL(1'b1), .GAME_PLANES(NP), .WIN_AVAIL(1'b1),
		.DESK_AVAIL(DESK), .VGA_DAC3(DAC3)) dut (
		.pixel(cursor_pix),
		.clk(clk),
		.pclk(pclk),
		.bclk(bclk),
		.resetn(resetn),
		.video_mode(video_mode),
		.video_bright(video_bright),
		.view_load(view_load),
		.game_en(game_en),
		.game_wrap(game_wrap),
		.view_x(view_x),
		.view_y(view_y),
		.color_en(color_en),
		.color_np(color_np),
		.color_off(color_off),
		.win_en(win_en),
		.win_x0(win_x0),
		.win_y0(win_y0),
		.win_x1(win_x1),
		.win_y1(win_y1),
		.win_desk(win_desk),
		.pal_we(pal_we),
		.pal_idx(pal_idx),
		.pal_rgb(pal_rgb),
		.frame_ctr(frame_ctr),
		.in_vblank(in_vblank),
		.red(red), .green(green), .blue(blue),
		.red_lo(red_lo), .green_lo(green_lo), .blue_lo(blue_lo),
		.hsync(hsync), .vsync(vsync),
		.dvi_p(dvi_p),
		.dac(dac),
		.x(x), .y(y),
		.is_visible(is_visible),
		.gb_adr_o(gb_adr_o),
		.gb_dat_i(gb_dat_i)
	);

	reg [31:0] vram [0:WORDS-1];
	always @(posedge clk)
		gb_dat_i <= (gb_adr_o < WORDS) ? vram[gb_adr_o] : 32'hxxxxxxxx;

	reg [11:0] pal_model [0:255];

	reg m_game, m_win, m_desk, m_paper;
	reg [1:0] m_mode;
	reg [3:0] m_level;
	reg [9:0] m_x0, m_y0, m_x1, m_y1;
	reg m_check = 0;

	function s0bit;
		input integer c;
		input integer r;
		begin
			s0bit = vram[r * 20 + c / 32][c % 32];
		end
	endfunction

	function s1bit;
		input integer c;
		input integer r;
		integer cc, rr;
		begin
			cc = c % 640;
			rr = (r % 480) % S1_ROWS;
			s1bit = vram[9600 + rr * 20 + cc / 32][cc % 32];
		end
	endfunction

	// the monochrome inks, from the formulas in gpu_video.v's header
	function [7:0] ink_w;
		input [3:0] l;
		integer w;
		begin
			w = 16 * (l + 1);
			ink_w = (w > 255) ? 8'd255 : w;
		end
	endfunction

	function [7:0] ink_ar;
		input [3:0] l;
		integer w, v;
		begin
			w = (16 * (l + 1) > 255) ? 255 : 16 * (l + 1);
			v = w + w / 2 + w / 8 + w / 64;
			ink_ar = (v > 255) ? 8'd255 : v;
		end
	endfunction

	function [7:0] ink_ag;
		input [3:0] l;
		integer w, v;
		begin
			w = (16 * (l + 1) > 255) ? 255 : 16 * (l + 1);
			v = w + w / 8 + w / 16 + w / 32 + w / 128;
			ink_ag = (v > 255) ? 8'd255 : v;
		end
	endfunction

	// { r, g, b } ink for mode and level
	function [23:0] ink;
		input [1:0] mode;
		input [3:0] l;
		begin
			case (mode)
				2'd1: ink = { ink_ar(l), ink_ag(l), 8'd0 };
				2'd2: ink = { 8'd0, ink_w(l), 8'd0 };
				default: ink = { ink_w(l), ink_w(l), ink_w(l) };
			endcase
		end
	endfunction

	// a channel on a 3-bit DAC: the top three bits, never black if lit
	function [2:0] dac3;
		input [7:0] v;
		begin
			dac3 = (v[7:5] == 3'd0 && v != 8'd0) ? 3'd1 : v[7:5];
		end
	endfunction

	// the VGA a 1-bit board shows for a monochrome pixel
	function [2:0] vga1_mono;
		input [1:0] mode;
		input lit;
		begin
			vga1_mono = { lit && mode != 2'd2, lit,
				lit && mode != 2'd1 && mode != 2'd2 };
		end
	endfunction

	integer px, py;
	reg inside, cur_exp, mono_exp, acc, use_pal;
	reg [7:0] expect_idx;
	reg [11:0] prgb;
	reg [23:0] want_ink;
	reg [8:0] want_vga;     // { r2 r1 r0 g2 g1 g0 b2 b1 b0 }
	reg [8:0] got_vga;
	reg [2:0] v1;
	integer mism;
	integer n_pal, n_mono;

	always @(negedge pclk) begin
		if (m_check && dut.is_visible && !m_game) begin
			px = dut.hc - H_START;
			py = dut.vc - V_START;
			cur_exp = cursor_test && px[3] && !py[2];
			inside = m_win && px >= m_x0 && px < m_x1 && py >= m_y0 && py < m_y1;
			acc = s1bit(px, py);

			if (inside && m_desk && DESK_REAL) begin
				use_pal = acc && !cur_exp;
				expect_idx = { 3'b000, 1'b1, 3'b000, s0bit(px, py) };
			end else if (inside) begin
				use_pal = 1'b1;
				expect_idx = 8'd0;
				expect_idx[0] = s0bit(px, py);
				expect_idx[4] = s1bit(px, py);
				expect_idx[5] = s1bit(px + 320, py);
				if (NP == 8) begin
					expect_idx[6] = s1bit(px, py + 240);
					expect_idx[7] = s1bit(px + 320, py + 240);
				end
				if (cur_exp)
					expect_idx = expect_idx ^ ((NP == 8) ? 8'hf1 : 8'h31);
			end else begin
				use_pal = 1'b0;
			end

			got_vga = { red, red_lo, green, green_lo, blue, blue_lo };
			want_ink = ink(m_mode, m_level);

			if (use_pal) begin
				n_pal = n_pal + 1;
				prgb = pal_model[expect_idx];
				if (DAC3)
					want_vga = { prgb[11:9], prgb[7:5], prgb[3:1] };
				else
					want_vga = { prgb[11], 2'b00, prgb[7], 2'b00, prgb[3], 2'b00 };
				if (dut.win_px !== 1'b1 || dut.cidx_o !== expect_idx) begin
					if (mism < 8)
						$display("FAIL: pixel (%0d,%0d): palette index %0h, expected %0h",
							px, py, dut.cidx_o, expect_idx);
					mism = mism + 1;
				end else if (got_vga !== want_vga) begin
					if (mism < 8)
						$display("FAIL: pixel (%0d,%0d): VGA %b, expected %b (entry %0h)",
							px, py, got_vga, want_vga, expect_idx);
					mism = mism + 1;
				end
`ifdef GPU_DDMI
				if (dut.ddmi_red !== { prgb[11:8], prgb[11:8] } ||
				    dut.ddmi_green !== { prgb[7:4], prgb[7:4] } ||
				    dut.ddmi_blue !== { prgb[3:0], prgb[3:0] }) begin
					if (mism < 8)
						$display("FAIL: pixel (%0d,%0d): DDMI %h %h %h for entry %0h",
							px, py, dut.ddmi_red, dut.ddmi_green, dut.ddmi_blue, expect_idx);
					mism = mism + 1;
				end
`endif
			end else begin
				n_mono = n_mono + 1;
				mono_exp = s0bit(px, py) ^ cur_exp ^ m_paper;
				if (DAC3)
					want_vga = mono_exp ? { dac3(want_ink[23:16]), dac3(want_ink[15:8]),
						dac3(want_ink[7:0]) } : 9'd0;
				else
				begin
					v1 = vga1_mono(m_mode, mono_exp);
					want_vga = { v1[2], 2'b00, v1[1], 2'b00, v1[0], 2'b00 };
				end
				if (got_vga !== want_vga) begin
					if (mism < 8)
						$display("FAIL: pixel (%0d,%0d): mono VGA %b, expected %b",
							px, py, got_vga, want_vga);
					mism = mism + 1;
				end
`ifdef GPU_DDMI
				if (dut.ddmi_red !== (mono_exp ? want_ink[23:16] : 8'h00) ||
				    dut.ddmi_green !== (mono_exp ? want_ink[15:8] : 8'h00) ||
				    dut.ddmi_blue !== (mono_exp ? want_ink[7:0] : 8'h00)) begin
					if (mism < 8)
						$display("FAIL: pixel (%0d,%0d): DDMI mono %h %h %h, expected %h",
							px, py, dut.ddmi_red, dut.ddmi_green, dut.ddmi_blue,
							mono_exp ? want_ink : 24'd0);
					mism = mism + 1;
				end
`endif
			end
		end
	end

	task set_view;
		input en;
		input col;
		input [2:0] np;
		input [5:0] off;
		begin
			@(posedge clk);
			game_en   <= en;
			game_wrap <= 1'b0;
			view_x    <= 10'd0;
			view_y    <= 10'd0;
			color_en  <= col;
			color_np  <= np;
			color_off <= off;
			view_load <= ~view_load;
			repeat (4) @(posedge pclk);
		end
	endtask

	task set_win;
		input en;
		input desk;
		input [9:0] x0;
		input [9:0] y0;
		input [9:0] x1;
		input [9:0] y1;
		begin
			@(posedge clk);
			win_en <= en;
			win_desk <= desk;
			win_x0 <= x0;
			win_y0 <= y0;
			win_x1 <= x1;
			win_y1 <= y1;
			view_load <= ~view_load;
			repeat (4) @(posedge pclk);
		end
	endtask

	task pal_write;
		input [7:0] idx;
		input [11:0] rgb;
		begin
			@(posedge clk);
			pal_we  <= 1'b1;
			pal_idx <= idx;
			pal_rgb <= rgb;
			@(posedge clk);
			pal_we  <= 1'b0;
			pal_model[idx % NPAL] = rgb;
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

	task check_frame;
		input [8*64-1:0] what;
		input want_some_pal;
		begin
			repeat (8) @(posedge pclk);
			next_frame();
			m_game = game_en;
			m_win = win_en;
			m_desk = win_desk;
			m_x0 = win_x0;  m_y0 = win_y0;
			m_x1 = win_x1;  m_y1 = win_y1;
			m_mode = video_mode;
			m_level = video_bright;
			m_paper = (video_mode == 2'd3);
			mism = 0;
			n_pal = 0;
			n_mono = 0;
			m_check = 1;
			goto(V_STOP - 1, H_STOP - 2);
			m_check = 0;
			if (want_some_pal && n_pal == 0 && !m_game) begin
				$display("FAIL: %0s: no palette pixels at all", what);
				errors = errors + 1;
			end
			if (mism != 0) begin
				$display("FAIL: %0s: %0d mismatching pixels (%0d palette, %0d mono)",
					what, mism, n_pal, n_mono);
				errors = errors + 1;
			end else begin
				$display("ok:   %0s (%0d palette, %0d mono)", what, n_pal, n_mono);
			end
		end
	endtask

	integer i;
	reg [31:0] seed;

	initial begin

		seed = 32'h2468_ace1;
		for (i = 0; i < WORDS; i = i + 1) begin
			seed = seed * 32'd1664525 + 32'd1013904223;
			vram[i] = seed ^ (i << 11) ^ (i * 32'h9e37_79b9);
		end

		resetn = 0;
		repeat (8) @(posedge pclk);
		resetn = 1;

		$display("GAME_PLANES %0d, DESK_AVAIL %0d, VGA_DAC3 %0d", NP, DESK, DAC3);

		// a palette with every entry distinct in its top bits as far as
		// possible, so a wrong entry shows on VGA too
		for (i = 0; i < NPAL; i = i + 1) begin
			seed = seed * 32'd1664525 + 32'd1013904223;
			pal_write(i, seed[23:12]);
		end
		pal_write(8'h10, 12'h123);
		pal_write(8'h11, 12'hCA8);

		set_view(0, 0, 3'd0, 6'd0);

		// ---- 1: the whole screen ----
		set_win(1, 1, 10'd0, 10'd0, 10'd640, 10'd480);
		check_frame("whole screen, white, level 7", 1);

		// ---- 2: amber, level 11 ----
		video_mode = 2'd1;
		video_bright = 4'd11;
		repeat (4) @(posedge clk);
		check_frame("amber, level 11: exact ink", 1);

		// ---- 3: paper ----
		video_mode = 2'd3;
		video_bright = 4'd7;
		repeat (4) @(posedge clk);
		check_frame("paper", 1);
		video_mode = 2'd0;

		// ---- 4: the pointer ----
		cursor_test = 1;
		repeat (4) @(posedge clk);
		check_frame("pointer: monochrome XOR over accents too", 1);
		cursor_test = 0;

		// ---- 5: a smaller window ----
		set_win(1, 1, 10'd100, 10'd50, 10'd420, 10'd290);
		check_frame("a 320x240 window, DESK", 1);

		// ---- 6: DESK off ----
		set_win(1, 0, 10'd100, 10'd50, 10'd420, 10'd290);
		check_frame("DESK off: the ordinary window", 1);

		// ---- 7: game mode ----
		set_win(1, 1, 10'd0, 10'd0, 10'd640, 10'd480);
		set_view(1, 1, NP - 1, 6'b11_10_01);
		repeat (8) @(posedge pclk);
		next_frame();
		next_frame();
		if (dut.desk_active !== 1'b0 || dut.win_active !== 1'b0) begin
			$display("FAIL: game mode: desk_active %b win_active %b",
				dut.desk_active, dut.win_active);
			errors = errors + 1;
		end else
			$display("ok:   game mode: DESK and the window ignored");
		set_view(0, 0, 3'd0, 6'd0);
		check_frame("back to the desktop: DESK again", 1);

		if (dut.desk_active !== DESK_REAL) begin
			$display("FAIL: desk_active %b, expected %b", dut.desk_active, DESK_REAL);
			errors = errors + 1;
		end

		if (errors == 0)
			$display("RESULT: PASS");
		else
			$display("RESULT: FAIL (%0d errors)", errors);

		$finish;
	end

endmodule

`ifdef TB_STUBS
module ODDRX1F (input D0, input D1, input SCLK, input RST, output Q);
	assign Q = D0;
endmodule

module SB_IO (
	inout PACKAGE_PIN,
	input CLOCK_ENABLE,
	input OUTPUT_CLK,
	input OUTPUT_ENABLE,
	input D_OUT_0,
	input D_OUT_1
);
	parameter [5:0] PIN_TYPE = 6'b0;
	parameter IO_STANDARD = "";
endmodule
`endif
