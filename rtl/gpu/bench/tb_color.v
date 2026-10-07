`timescale 1ns / 1ps

// Self-checking testbench for rtl/gpu/gpu_video.v's game-mode colour
// (docs/color.md).
//
// Unlike tb_game_mode.v this one checks PIXELS, not addresses, because
// colour is a data path: a plane word fetched from the wrong row, held
// one crossing too long, or rotated by the wrong ten words all leave x
// and y perfectly correct. So the VRAM here is a real array filled with
// a fixed pseudo-random pattern, the testbench works out from first
// principles which framebuffer bit every plane should show at every
// visible pixel, and compares the colour index the DUT produced --
// EVERY visible pixel of a whole frame, per configuration. Spot checks
// would miss exactly the bugs worth having a bench for: one plane one
// word late, or wrong only across the wrap seam.
//
// The model shares nothing with the DUT except the VRAM contents and
// the raster position (hc/vc, read hierarchically, as tb_game_mode.v
// does): the viewport origin, clamp, wrap, doubling, plane offsets and
// cursor XOR are all recomputed here independently.
//
// Checked here:
//
//   1. 16 colours, the four quadrants as planes, viewport at (0,0).
//   2. 4 colours double-buffered: plane 1 at +320 columns, viewport
//      on the lower page (0,240).
//   3. 4 colours side-scrolling: plane 1 at +240 rows, wrap on, a
//      NON-word-aligned origin (600) that crosses the 639->0 seam
//      mid-line -- the case where a word-crossing bug would hide.
//   4. 8 colours: plane 3 must read as 0 whatever VRAM holds.
//   5. 16 colours with an odd origin (137,61) in wrap mode, so every
//      plane straddles words and wraps on both axes.
//   6. the cursor inverts only the ACTIVE planes (index ^ 2^n-1).
//   7. the palette: writes land, VGA shows the top bit of each
//      channel, and blanking is black.
//   8. colour enabled but game mode OFF: the desktop path, untouched.
//   9. colour off in game mode: monochrome, phosphor honoured.
//  10. colour on ignores the phosphor (paper mode does not invert).
//  11. a colour change written mid-frame waits for the frame boundary.
//  12. with -DGPU_DDMI (and the stubs at the bottom of this file): the
//      8-bit TMDS inputs are each 4-bit channel replicated.
//
// Running it -- same trailing-comma patch as the other video benches
// (see rtl/gpu/bench/README.md):
//
//   sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
//       rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
//   iverilog -g2005 -o /tmp/tb_color.out \
//       rtl/gpu/bench/tb_color.v /tmp/gpu_video_fix.v
//   vvp /tmp/tb_color.out
//
// and for the DDMI variant:
//
//   iverilog -g2005 -DGPU_DDMI -DTB_STUBS -o /tmp/tb_color_ddmi.out \
//       rtl/gpu/bench/tb_color.v /tmp/gpu_video_fix.v \
//       rtl/gpu/gpu_ddmi.v rtl/gpu/tmds_encoder.v
//   vvp /tmp/tb_color_ddmi.out
//
// Each runs about thirty whole frames: a few minutes, or about twenty
// with DDMI (the 126MHz serialiser dominates the simulation).

module tb_color;

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
	reg [1:0] color_np = 0;
	reg [5:0] color_off = 0;
	reg pal_we = 0;
	reg [3:0] pal_idx = 0;
	reg [11:0] pal_rgb = 0;

	wire red, green, blue, hsync, vsync, is_visible;
	wire [9:0] x, y;
	wire [14:0] gb_adr_o;
	wire [3:0] dvi_p;
	wire [3:0] dac;
	wire [15:0] frame_ctr;
	wire in_vblank;

	// cursor: a checkerboard over part of the screen, driven from the
	// DUT's own framebuffer coordinates exactly as gpu_cursor.v is
	reg cursor_test = 0;
	wire cursor_pix = cursor_test && x[3] && !y[2];

	integer errors = 0;

	// 48MHz and 25.2MHz, as on the boards. The fetch's timing margin is
	// a property of this ratio, so the bench uses the real one.
	always #10.417 clk  = ~clk;
	always #19.841 pclk = ~pclk;
	always #3.968  bclk = ~bclk;

	gpu_video #(.COLOR_AVAIL(1'b1)) dut (
		.pixel(cursor_pix),
		.clk(clk),
		.pclk(pclk),
		.bclk(bclk),
		.resetn(resetn),
		.video_mode(video_mode),
		.view_load(view_load),
		.game_en(game_en),
		.game_wrap(game_wrap),
		.view_x(view_x),
		.view_y(view_y),
		.color_en(color_en),
		.color_np(color_np),
		.color_off(color_off),
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
	reg [31:0] vram [0:9599];
	always @(posedge clk)
		gb_dat_i <= vram[gb_adr_o];

	// -- palette shadow --
	reg [11:0] pal_model [0:15];

	// -- model state: what the CURRENT frame should be showing --
	reg m_game, m_wrap, m_col;
	reg [9:0] m_vx, m_vy;
	reg [1:0] m_np;
	reg [5:0] m_off;
	reg m_check = 0;
	reg m_check_rgb = 0;

	// framebuffer bit at (col,row), straight out of the VRAM array
	function fbbit;
		input integer col;
		input integer row;
		begin
			fbbit = vram[row * 20 + (col / 32)][col % 32];
		end
	endfunction

	integer px, py, c0, r0, ck, rk, k;
	integer evx, evy;
	reg [3:0] expect_idx;
	reg [3:0] mask;
	reg [3:0] raw;
	reg cur_exp;
	integer mism;
	reg [11:0] prgb;

	always @(negedge pclk) begin
		if (m_check && dut.is_visible) begin
			px = dut.hc - H_START;
			py = dut.vc - V_START;

			// viewport origin as adopted: clamp only when not wrapping
			evx = (!m_wrap && m_vx > 320) ? 320 : m_vx;
			evy = (!m_wrap && m_vy > 240) ? 240 : m_vy;
			c0 = (evx + px / 2) % 640;
			r0 = (evy + py / 2) % 480;

			raw[0] = fbbit(c0, r0);
			for (k = 1; k < 4; k = k + 1) begin
				ck = (c0 + (m_off[2 * (k - 1)]     ? 320 : 0)) % 640;
				rk = (r0 + (m_off[2 * (k - 1) + 1] ? 240 : 0)) % 480;
				raw[k] = fbbit(ck, rk);
			end
			mask = { (m_np == 2'd3), m_np[1], (m_np != 2'd0), 1'b1 };

			// cursor from the model's own coordinates
			cur_exp = cursor_test && c0[3] && !r0[2];
			expect_idx = (raw ^ {4{cur_exp}}) & mask;

			if (dut.cidx !== expect_idx || dut.color_on !== 1'b1) begin
				if (mism < 8)
					$display("FAIL: pixel (%0d,%0d) fb(%0d,%0d): index %0h, expected %0h (color_on %0b)",
						px, py, c0, r0, dut.cidx, expect_idx, dut.color_on);
				mism = mism + 1;
			end

			if (m_check_rgb) begin
				prgb = pal_model[expect_idx];
				if (red !== prgb[11] || green !== prgb[7] || blue !== prgb[3]) begin
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
			end
		end
		// blanking is black in colour, whatever the palette says
		if (m_check_rgb && !dut.is_visible) begin
			if (red !== 1'b0 || green !== 1'b0 || blue !== 1'b0) begin
				if (mism < 8)
					$display("FAIL: colour output not black in blanking (hc %0d vc %0d)",
						dut.hc, dut.vc);
				mism = mism + 1;
			end
		end
	end

	task check_eq;
		input [8*64-1:0] what;
		input [31:0] got;
		input [31:0] exp;
		begin
			if (got !== exp) begin
				$display("FAIL: %0s: got %0d, expected %0d", what, got, exp);
				errors = errors + 1;
			end
		end
	endtask

	// one payload, one toggle -- as socctl.v drives it
	task set_cfg;
		input en;
		input wrap;
		input [9:0] vx;
		input [9:0] vy;
		input col;
		input [1:0] np;
		input [5:0] off;
		begin
			@(posedge clk);
			game_en   <= en;
			game_wrap <= wrap;
			view_x    <= vx;
			view_y    <= vy;
			color_en  <= col;
			color_np  <= np;
			color_off <= off;
			view_load <= ~view_load;
		end
	endtask

	task pal_write;
		input [3:0] idx;
		input [11:0] rgb;
		begin
			@(posedge clk);
			pal_we  <= 1'b1;
			pal_idx <= idx;
			pal_rgb <= rgb;
			@(posedge clk);
			pal_we  <= 1'b0;
			pal_model[idx] = rgb;
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

	// Adopt the payload last written (at the next frame boundary), then
	// check every visible pixel of the frame after that boundary.
	task check_frame;
		input [8*64-1:0] what;
		input rgb_too;
		begin
			// Let the payload cross first. A set_cfg issued in the
			// last few pixel clocks of a frame -- which is exactly
			// where the previous check_frame left off -- is captured
			// AFTER that frame's boundary and adopted at the one
			// after. That is the hardware working as specified, not a
			// bug, but the model must not assume otherwise.
			repeat (8) @(posedge pclk);
			next_frame();
			m_game = game_en;  m_wrap = game_wrap;
			m_vx = view_x;     m_vy = view_y;
			m_col = color_en;  m_np = color_np;  m_off = color_off;
			mism = 0;
			m_check_rgb = rgb_too;
			m_check = 1;
			goto(V_STOP - 1, H_STOP - 2);
			m_check = 0;
			m_check_rgb = 0;
			if (mism != 0) begin
				$display("FAIL: %0s: %0d mismatching pixels", what, mism);
				errors = errors + 1;
			end else begin
				$display("ok:   %0s", what);
			end
		end
	endtask

	integer i, n;
	reg [31:0] seed;
	reg [11:0] e0;

	initial begin

		// a fixed pseudo-random framebuffer: every quadrant different,
		// every word different, so a fetch from the wrong row, word or
		// plane shows up as a mismatch rather than coincidentally
		// matching
		seed = 32'h1234_5678;
		for (i = 0; i < 9600; i = i + 1) begin
			seed = seed * 32'd1664525 + 32'd1013904223;
			vram[i] = seed ^ (i << 13) ^ (i * 32'h9e37_79b9);
		end

		// power-on palette, mirrored from gpu_video.v's initial block
		pal_model[0]  = 12'h000;  pal_model[1]  = 12'h00a;
		pal_model[2]  = 12'h0a0;  pal_model[3]  = 12'h0aa;
		pal_model[4]  = 12'ha00;  pal_model[5]  = 12'ha0a;
		pal_model[6]  = 12'ha50;  pal_model[7]  = 12'haaa;
		pal_model[8]  = 12'h555;  pal_model[9]  = 12'h55f;
		pal_model[10] = 12'h5f5;  pal_model[11] = 12'h5ff;
		pal_model[12] = 12'hf55;  pal_model[13] = 12'hf5f;
		pal_model[14] = 12'hff5;  pal_model[15] = 12'hfff;

		resetn = 0;
		repeat (8) @(posedge pclk);
		resetn = 1;

		// ---- 1: 16 colours, quadrants ----
		// plane 1 {dy,dx} = 01, plane 2 = 10, plane 3 = 11
		set_cfg(1, 0, 0, 0, 1, 2'd3, 6'b11_10_01);
		check_frame("16 colours, quadrant planes, view (0,0)", 1);

		// ---- 2: 4 colours, double-buffered, lower page ----
		set_cfg(1, 0, 0, 240, 1, 2'd1, 6'b00_00_01);
		check_frame("4 colours double-buffered, page (0,240)", 1);

		// ---- 3: 4 colours, side-scrolling across the seam ----
		set_cfg(1, 1, 600, 0, 1, 2'd1, 6'b00_00_10);
		check_frame("4 colours side-scroll, wrap, origin 600", 1);

		set_cfg(1, 1, 333, 0, 1, 2'd1, 6'b00_00_10);
		check_frame("4 colours side-scroll, wrap, origin 333", 0);

		// ---- 4: 8 colours, plane 3 ignored ----
		set_cfg(1, 0, 0, 0, 1, 2'd2, 6'b11_10_01);
		check_frame("8 colours, plane 3 masked", 1);

		// ---- 5: odd origin, wrap, every plane straddling ----
		set_cfg(1, 1, 137, 61, 1, 2'd3, 6'b11_10_01);
		check_frame("16 colours, wrap, origin (137,61)", 1);

		set_cfg(1, 1, 631, 470, 1, 2'd3, 6'b01_11_10);
		check_frame("16 colours, wrap, origin (631,470), shuffled offsets", 0);

		// clamp mode with an origin past the clamp: the planes follow
		// the CLAMPED origin, not the written one
		set_cfg(1, 0, 500, 400, 1, 2'd3, 6'b11_10_01);
		check_frame("16 colours, clamp, origin (500,400) -> (320,240)", 0);

		// ---- 6: cursor inverts active planes only ----
		cursor_test = 1;
		set_cfg(1, 0, 0, 0, 1, 2'd1, 6'b00_00_01);
		check_frame("cursor XOR, 4 colours", 1);
		set_cfg(1, 0, 0, 0, 1, 2'd3, 6'b11_10_01);
		check_frame("cursor XOR, 16 colours", 1);
		set_cfg(1, 0, 0, 0, 1, 2'd0, 6'b00_00_00);
		check_frame("cursor XOR, 2 colours", 1);
		cursor_test = 0;

		// ---- 7: palette writes ----
		pal_write(4'd0, 12'h123);
		pal_write(4'd1, 12'h8f0);
		pal_write(4'd2, 12'h07e);
		pal_write(4'd3, 12'hc81);
		pal_write(4'd15, 12'h808);
		set_cfg(1, 0, 0, 0, 1, 2'd3, 6'b11_10_01);
		check_frame("palette rewritten, 16 colours", 1);

		// ---- 11: mid-frame change waits for the boundary ----
		// (runs before 8-10 so the adopted state is known colour-on)
		next_frame();
		goto(V_START + 200, H_START);
		set_cfg(1, 0, 0, 0, 1, 2'd1, 6'b00_00_01);
		goto(V_START + 300, H_START + 10);
		check_eq("mid-frame: np unchanged this frame", dut.np_active, 3);
		next_frame();
		goto(V_START + 1, H_START);
		check_eq("mid-frame: np adopted next frame", dut.np_active, 1);

		// ---- 8: colour enabled, game mode off: desktop untouched ----
		set_cfg(0, 0, 0, 0, 1, 2'd3, 6'b11_10_01);
		next_frame();
		n = 0;
		goto(V_START, H_START);
		for (i = 0; i < 640 * 4; i = i + 1) begin
			@(negedge pclk);
			if (dut.is_visible) begin
				if (dut.color_on !== 1'b0 ||
				    red !== dut.pset || green !== dut.pset || blue !== dut.pset)
					n = n + 1;
			end
		end
		check_eq("desktop + colour bit: monochrome path", n, 0);

		// ---- 9: colour off in game mode: phosphor honoured ----
		video_mode = 2'd2;   // green
		set_cfg(1, 0, 0, 0, 0, 2'd3, 6'b11_10_01);
		next_frame();
		next_frame();
		n = 0;
		goto(V_START, H_START);
		for (i = 0; i < 640 * 4; i = i + 1) begin
			@(negedge pclk);
			if (dut.is_visible) begin
				if (dut.color_on !== 1'b0 || red !== 1'b0 ||
				    green !== dut.pset || blue !== 1'b0)
					n = n + 1;
			end
		end
		check_eq("game mode, colour off: green phosphor", n, 0);

		// ---- 10: colour on ignores the phosphor ----
		video_mode = 2'd3;   // paper: would invert a monochrome picture
		set_cfg(1, 0, 0, 0, 1, 2'd3, 6'b11_10_01);
		next_frame();   // let the paper mode be adopted too
		check_frame("16 colours with paper phosphor selected", 1);
		video_mode = 2'd0;

		if (errors == 0)
			$display("RESULT: PASS");
		else
			$display("RESULT: FAIL (%0d errors)", errors);

		$finish;
	end

endmodule

`ifdef TB_STUBS
// Behavioural stand-ins for the vendor primitives the DDMI path
// instantiates, so -DGPU_DDMI elaborates under iverilog. Only the
// 8-bit channel values going INTO the TMDS encoder are checked; what
// comes out of these is never looked at.
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
