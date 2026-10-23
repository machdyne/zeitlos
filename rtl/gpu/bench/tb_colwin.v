`timescale 1ns / 1ps

// Self-checking testbench for the colour window in rtl/gpu/gpu_video.v
// (WIN_AVAIL; docs/color.md, "The colour window"): in desktop mode, one
// rectangle shown through the palette, its index the desktop's bit and
// the second sheet's planes.
//
// tb_planes.v's method: a VRAM of one and a half or two sheets filled
// with a fixed pseudo-random pattern, a model that works out from first
// principles what every visible pixel of a whole frame must be, and a
// check of every one.
//
// Inside the rectangle the index on screen (cidx_o) must be
//
//   bit 0     the desktop's pixel, sheet 0 at (x, y)
//   bit 4     sheet 1 at (x, y)                 rows mod 240 at six planes
//   bit 5     sheet 1 at (x + 320, y)           columns mod 640
//   bit 6, 7  the same as 4, 5, 240 rows on     eight planes only
//
// and the outputs the palette's colour for it; outside, the monochrome
// path, exactly the desktop's pixel -- which also checks that the
// monochrome pixel is delayed to meet x running a clock early.
//
// Checked here:
//
//   1. a 320x240 window at an odd position, power-on palette.
//   2. a window across the fold (from (400,300) to the corner), smaller
//      than 320x240, a rewritten palette.
//   3. a window at (320,240), the whole lower-right quarter.
//   4. the cursor (the `pixel` input): inverts every plane in use
//      inside, the pixel outside.
//   5. paper mode outside the window: inverted; inside: the palette.
//   6. the window off: the desktop exactly as before, x not leading.
//   7. game mode with the window still asked for: the window ignored
//      (game-mode colour, as tb_planes.v's first case).
//   8. with -DGPU_DDMI (and the stubs): the 8-bit TMDS inputs.
//
// Running it -- the trailing-comma patch as the other video benches:
//
//   sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
//       rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
//   iverilog -g2005 -DNPLANES=8 -o /tmp/tb_colwin8.out \
//       rtl/gpu/bench/tb_colwin.v /tmp/gpu_video_fix.v
//   vvp /tmp/tb_colwin8.out
//
// and -DNPLANES=6 for six. About a dozen whole frames each.

`ifndef NPLANES
`define NPLANES 8
`endif

module tb_colwin;

	localparam NP = `NPLANES;
	localparam WORDS = (NP == 8) ? 19200 : 14400;
	localparam S1_ROWS = (NP == 8) ? 480 : 240;
	localparam NPAL = (NP == 8) ? 256 : 64;

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
	reg [9:0] win_x0 = 0, win_y0 = 0, win_x1 = 0, win_y1 = 0;

	wire red, green, blue, hsync, vsync, is_visible;
	wire [9:0] x, y;
	wire [14:0] gb_adr_o;
	wire [3:0] dvi_p;
	wire [3:0] dac;
	wire [15:0] frame_ctr;
	wire in_vblank;

	// a stand-in cursor, from x and y as the real one is
	reg cursor_test = 0;
	wire cursor_pix = cursor_test && x[3] && !y[2];

	integer errors = 0;

	always #10.417 clk  = ~clk;
	always #19.841 pclk = ~pclk;
	always #3.968  bclk = ~bclk;

	gpu_video #(.COLOR_AVAIL(1'b1), .GAME_PLANES(NP), .WIN_AVAIL(1'b1)) dut (
		.pixel(cursor_pix),
		.clk(clk),
		.pclk(pclk),
		.bclk(bclk),
		.resetn(resetn),
		.video_mode(video_mode),
		.video_bright(4'd7),
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
		.pal_we(pal_we),
		.pal_idx(pal_idx),
		.pal_rgb(pal_rgb),
		.frame_ctr(frame_ctr),
		.in_vblank(in_vblank),
		.red(red), .green(green), .blue(blue),
		.hsync(hsync), .vsync(vsync),
		.dvi_p(dvi_p),
		.dac(dac),
		.x(x), .y(y),
		.is_visible(is_visible),
		.gb_adr_o(gb_adr_o),
		.gb_dat_i(gb_dat_i)
	);

	// -- VRAM: the graphics port of rtl/mem/vram.v, behaviourally --
	reg [31:0] vram [0:WORDS-1];
	always @(posedge clk)
		gb_dat_i <= (gb_adr_o < WORDS) ? vram[gb_adr_o] : 32'hxxxxxxxx;

	reg [11:0] pal_model [0:255];
	reg [11:0] ega [0:15];

	// what the frame being checked was configured as
	reg m_game;
	reg m_win;
	reg [9:0] m_x0, m_y0, m_x1, m_y1;
	reg m_paper;
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

	integer px, py;
	reg inside, cur_exp, mono_exp;
	reg [7:0] expect_idx;
	reg [11:0] prgb;
	integer mism;
	integer n_in, n_out;

	always @(negedge pclk) begin
		if (m_check && dut.is_visible) begin
			px = dut.hc - H_START;
			py = dut.vc - V_START;
			cur_exp = cursor_test && px[3] && !py[2];

			inside = m_win && !m_game &&
				px >= m_x0 && px < m_x1 && py >= m_y0 && py < m_y1;

			if (dut.win_px !== inside) begin
				if (mism < 8)
					$display("FAIL: pixel (%0d,%0d): win_px %b, expected %b",
						px, py, dut.win_px, inside);
				mism = mism + 1;
			end

			if (inside) begin
				n_in = n_in + 1;
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
				prgb = pal_model[expect_idx];
				if (dut.cidx_o !== expect_idx) begin
					if (mism < 8)
						$display("FAIL: pixel (%0d,%0d): index %0h, expected %0h",
							px, py, dut.cidx_o, expect_idx);
					mism = mism + 1;
				end else if (red !== prgb[11] || green !== prgb[7] || blue !== prgb[3]) begin
					if (mism < 8)
						$display("FAIL: pixel (%0d,%0d): VGA %b%b%b, expected %b%b%b from entry %0h",
							px, py, red, green, blue, prgb[11], prgb[7], prgb[3], expect_idx);
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
			end else if (!m_game) begin
				n_out = n_out + 1;
				mono_exp = s0bit(px, py) ^ cur_exp ^ m_paper;
				if (red !== mono_exp || green !== mono_exp || blue !== mono_exp) begin
					if (mism < 8)
						$display("FAIL: pixel (%0d,%0d): mono %b%b%b, expected %b",
							px, py, red, green, blue, mono_exp);
					mism = mism + 1;
				end
`ifdef GPU_DDMI
				if (dut.ddmi_red !== (mono_exp ? 8'h80 : 8'h00) ||
				    dut.ddmi_green !== (mono_exp ? 8'h80 : 8'h00) ||
				    dut.ddmi_blue !== (mono_exp ? 8'h80 : 8'h00)) begin
					if (mism < 8)
						$display("FAIL: pixel (%0d,%0d): DDMI mono %h %h %h",
							px, py, dut.ddmi_red, dut.ddmi_green, dut.ddmi_blue);
					mism = mism + 1;
				end
`endif
			end else begin
				// game mode, 16-colour layout at (0,0), all planes
				n_out = n_out + 1;
				expect_idx = 8'd0;
				expect_idx[0] = s0bit(px / 2, py / 2);
				expect_idx[1] = s0bit(px / 2 + 320, py / 2);
				expect_idx[2] = s0bit(px / 2, py / 2 + 240);
				expect_idx[3] = s0bit(px / 2 + 320, py / 2 + 240);
				expect_idx[4] = s1bit(px / 2, py / 2);
				expect_idx[5] = s1bit(px / 2 + 320, py / 2);
				if (NP == 8) begin
					expect_idx[6] = s1bit(px / 2, py / 2 + 240);
					expect_idx[7] = s1bit(px / 2 + 320, py / 2 + 240);
				end
				if (dut.cidx_o !== expect_idx || dut.color_on !== 1'b1) begin
					if (mism < 8)
						$display("FAIL: game pixel (%0d,%0d): index %0h, expected %0h",
							px, py, dut.cidx_o, expect_idx);
					mism = mism + 1;
				end
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
			// two flips inside one pixel clock would cancel in the
			// toggle's synchroniser; software's stores are far apart
			repeat (4) @(posedge pclk);
		end
	endtask

	task set_win;
		input en;
		input [9:0] x0;
		input [9:0] y0;
		input [9:0] x1;
		input [9:0] y1;
		begin
			@(posedge clk);
			win_en <= en;
			win_x0 <= x0;
			win_y0 <= y0;
			win_x1 <= x1;
			win_y1 <= y1;
			view_load <= ~view_load;
			// two flips inside one pixel clock would cancel in the
			// toggle's synchroniser; software's stores are far apart
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
		input want_lead;
		begin
			repeat (8) @(posedge pclk);
			next_frame();
			m_game = game_en;
			m_win = win_en;
			m_x0 = win_x0;  m_y0 = win_y0;
			m_x1 = win_x1;  m_y1 = win_y1;
			m_paper = (video_mode == 2'd3);
			mism = 0;
			n_in = 0;
			n_out = 0;
			m_check = 1;
			goto(V_STOP - 1, H_STOP - 2);
			m_check = 0;
			if (dut.x_lead !== want_lead) begin
				$display("FAIL: %0s: x_lead %b, expected %b", what, dut.x_lead, want_lead);
				errors = errors + 1;
			end
			if (mism != 0) begin
				$display("FAIL: %0s: %0d mismatching pixels (%0d inside, %0d outside)",
					what, mism, n_in, n_out);
				errors = errors + 1;
			end else begin
				$display("ok:   %0s (%0d inside, %0d outside)", what, n_in, n_out);
			end
		end
	endtask

	integer i;
	reg [31:0] seed;

	initial begin

		seed = 32'h1357_9bdf;
		for (i = 0; i < WORDS; i = i + 1) begin
			seed = seed * 32'd1664525 + 32'd1013904223;
			vram[i] = seed ^ (i << 13) ^ (i * 32'h9e37_79b9);
		end

		ega[0]  = 12'h000;  ega[1]  = 12'h00a;
		ega[2]  = 12'h0a0;  ega[3]  = 12'h0aa;
		ega[4]  = 12'ha00;  ega[5]  = 12'ha0a;
		ega[6]  = 12'ha50;  ega[7]  = 12'haaa;
		ega[8]  = 12'h555;  ega[9]  = 12'h55f;
		ega[10] = 12'h5f5;  ega[11] = 12'h5ff;
		ega[12] = 12'hf55;  ega[13] = 12'hf5f;
		ega[14] = 12'hff5;  ega[15] = 12'hfff;
		for (i = 0; i < 256; i = i + 1)
			pal_model[i] = ega[i % 16];

		resetn = 0;
		repeat (8) @(posedge pclk);
		resetn = 1;

		$display("GAME_PLANES %0d, colour window", NP);

		// ---- 1: 320x240 at an odd position ----
		set_view(0, 0, 3'd0, 6'd0);
		set_win(1, 10'd137, 10'd61, 10'd457, 10'd301);
		check_frame("320x240 at (137,61), power-on palette", 1);

		// ---- 2: across the fold, smaller, new palette ----
		for (i = 0; i < NPAL; i = i + 1) begin
			seed = seed * 32'd1664525 + 32'd1013904223;
			pal_write(i, seed[23:12]);
		end
		set_win(1, 10'd400, 10'd300, 10'd640, 10'd480);
		check_frame("240x180 at (400,300), to the corner, new palette", 1);

		// ---- 3: the lower-right quarter ----
		set_win(1, 10'd320, 10'd240, 10'd640, 10'd480);
		check_frame("320x240 at (320,240)", 1);

		// ---- 4: the cursor ----
		cursor_test = 1;
		set_win(1, 10'd31, 10'd7, 10'd351, 10'd247);
		check_frame("cursor: every plane inside, the pixel outside", 1);
		cursor_test = 0;

		// ---- 5: paper outside ----
		video_mode = 2'd3;
		repeat (4) @(posedge clk);
		check_frame("paper mode outside the window", 1);
		video_mode = 2'd0;

		// ---- 6: the window off ----
		set_win(0, 10'd31, 10'd7, 10'd351, 10'd247);
		check_frame("window off: the desktop as before", 0);

		// ---- 7: game mode wins ----
		set_win(1, 10'd0, 10'd0, 10'd320, 10'd240);
		set_view(1, 1, NP - 1, 6'b11_10_01);
		check_frame("game mode, window asked for: ignored", 1);
		set_view(0, 0, 3'd0, 6'd0);
		check_frame("back to the desktop: the window again", 1);

		if (errors == 0)
			$display("RESULT: PASS");
		else
			$display("RESULT: FAIL (%0d errors)", errors);

		$finish;
	end

endmodule

`ifdef TB_STUBS
// tb_color.v's stand-ins for the vendor primitives the DDMI path
// instantiates; only the 8-bit channel values going into the TMDS
// encoder are checked.
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
