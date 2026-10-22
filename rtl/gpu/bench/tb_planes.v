`timescale 1ns / 1ps

// Self-checking testbench for rtl/gpu/gpu_video.v with more than four
// bitplanes (GAME_PLANES 6 or 8; docs/color.md, "More than four
// planes"), and for the palette in the cursor's block RAM.
//
// tb_color.v's method, at six or eight planes: a VRAM of one and a half
// or two sheets filled with a fixed pseudo-random pattern, a model that
// works out from first principles which bit every plane shows at every
// visible pixel -- planes 4..7 in the second sheet, where planes 0..3
// are in the first, rows taken mod 240 at six planes -- and a check of
// EVERY visible pixel of a whole frame per configuration: the index
// on screen (cidx_o) and the colour out of the palette.
//
// Checked here:
//
//   1. all planes, the four quadrants in each sheet, view (0,0), with
//      the power-on palette (entry i = EGA colour i mod 16).
//   2. all planes, wrap, an odd origin, shuffled offsets: every plane
//      straddling words and wrapping on both axes.
//   3. clamp mode with an origin past the clamp.
//   4. five planes: plane 5 and up read as 0.
//   5. four planes on this build: the second sheet unused.
//   6. the cursor inverts every active plane (index ^ 2^n-1).
//   7. palette writes above entry 15 land, including the top entry,
//      and an entry's bits above the palette's size are ignored.
//   8. colour enabled, game mode off: the desktop path, untouched.
//   9. with -DGPU_DDMI (and tb_color.v's stubs): the 8-bit TMDS
//      inputs are each 4-bit channel replicated.
//
// Running it -- the trailing-comma patch as the other video benches:
//
//   sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
//       rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
//   iverilog -g2005 -DNPLANES=8 -o /tmp/tb_planes8.out \
//       rtl/gpu/bench/tb_planes.v /tmp/gpu_video_fix.v
//   vvp /tmp/tb_planes8.out
//
// and -DNPLANES=6 for six. About twenty whole frames each: a few
// minutes. For DDMI add -DGPU_DDMI -DTB_STUBS and rtl/gpu/gpu_ddmi.v,
// rtl/gpu/tmds_encoder.v and rtl/gpu/ddmi_island.v, as tb_color.v.

`ifndef NPLANES
`define NPLANES 8
`endif

module tb_planes;

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

	wire red, green, blue, hsync, vsync, is_visible;
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

	gpu_video #(.COLOR_AVAIL(1'b1), .GAME_PLANES(NP)) dut (
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
	reg [31:0] vram [0:WORDS-1];
	always @(posedge clk)
		gb_dat_i <= (gb_adr_o < WORDS) ? vram[gb_adr_o] : 32'hxxxxxxxx;

	// -- palette shadow: power-on is EGA's 16, repeated --
	reg [11:0] pal_model [0:255];
	reg [11:0] ega [0:15];

	reg m_wrap;
	reg [9:0] m_vx, m_vy;
	reg [2:0] m_np;
	reg [5:0] m_off;
	reg m_check = 0;
	reg m_check_rgb = 0;

	// plane k's bit at viewport-relative framebuffer position (c0, r0)
	function pbit;
		input integer k;
		input integer c0;
		input integer r0;
		integer j, ck, rk, base;
		begin
			j = k % 4;
			ck = c0;
			rk = r0;
			if (j > 0) begin
				if (m_off[2 * (j - 1)])     ck = ck + 320;
				if (m_off[2 * (j - 1) + 1]) rk = rk + 240;
			end
			ck = ck % 640;
			rk = rk % 480;
			if (k >= 4) begin
				rk = rk % S1_ROWS;
				base = 9600;
			end else begin
				base = 0;
			end
			pbit = vram[base + rk * 20 + ck / 32][ck % 32];
		end
	endfunction

	integer px, py, c0, r0, k;
	integer evx, evy;
	reg [7:0] expect_idx;
	reg [7:0] mask;
	reg [7:0] raw;
	reg cur_exp;
	integer mism;
	reg [11:0] prgb;

	always @(negedge pclk) begin
		if (m_check && dut.is_visible) begin
			px = dut.hc - H_START;
			py = dut.vc - V_START;

			evx = (!m_wrap && m_vx > 320) ? 320 : m_vx;
			evy = (!m_wrap && m_vy > 240) ? 240 : m_vy;
			c0 = (evx + px / 2) % 640;
			r0 = (evy + py / 2) % 480;

			for (k = 0; k < 8; k = k + 1) begin
				raw[k] = (k < NP) ? pbit(k, c0, r0) : 1'b0;
				mask[k] = (k < NP) && (k <= m_np);
			end

			cur_exp = cursor_test && c0[3] && !r0[2];
			expect_idx = (raw ^ {8{cur_exp}}) & mask;

			if (dut.cidx_o !== expect_idx || dut.color_on !== 1'b1) begin
				if (mism < 8)
					$display("FAIL: pixel (%0d,%0d) fb(%0d,%0d): index %0h, expected %0h (color_on %0b)",
						px, py, c0, r0, dut.cidx_o, expect_idx, dut.color_on);
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

	task set_cfg;
		input en;
		input wrap;
		input [9:0] vx;
		input [9:0] vy;
		input col;
		input [2:0] np;
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
		input rgb_too;
		begin
			repeat (8) @(posedge pclk);
			next_frame();
			m_wrap = game_wrap;
			m_vx = view_x;     m_vy = view_y;
			m_np = color_np;   m_off = color_off;
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

	initial begin

		seed = 32'h8765_4321;
		for (i = 0; i < WORDS; i = i + 1) begin
			seed = seed * 32'd1664525 + 32'd1013904223;
			vram[i] = seed ^ (i << 11) ^ (i * 32'h9e37_79b9);
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

		$display("GAME_PLANES %0d", NP);

		// ---- 1: every plane, quadrants, power-on palette ----
		set_cfg(1, 0, 0, 0, 1, NP - 1, 6'b11_10_01);
		check_frame("all planes, quadrants, view (0,0), power-on palette", 1);

		// ---- 2: wrap, odd origin, shuffled offsets ----
		set_cfg(1, 1, 137, 61, 1, NP - 1, 6'b11_10_01);
		check_frame("all planes, wrap, origin (137,61)", 1);
		set_cfg(1, 1, 631, 470, 1, NP - 1, 6'b01_11_10);
		check_frame("all planes, wrap, origin (631,470), shuffled offsets", 0);

		// ---- 3: clamp past the clamp ----
		set_cfg(1, 0, 500, 400, 1, NP - 1, 6'b11_10_01);
		check_frame("all planes, clamp, origin (500,400) -> (320,240)", 0);

		// ---- 4: five planes ----
		set_cfg(1, 0, 0, 0, 1, 3'd4, 6'b11_10_01);
		check_frame("five planes: plane 5 and up masked", 1);

		// ---- 5: four planes on this build ----
		set_cfg(1, 0, 0, 0, 1, 3'd3, 6'b11_10_01);
		check_frame("four planes: the second sheet unused", 1);

		// ---- 6: cursor inverts every active plane ----
		cursor_test = 1;
		set_cfg(1, 0, 0, 0, 1, NP - 1, 6'b11_10_01);
		check_frame("cursor XOR, all planes", 1);
		cursor_test = 0;

		// ---- 7: palette writes above 15 ----
		for (i = 0; i < NPAL; i = i + 1) begin
			seed = seed * 32'd1664525 + 32'd1013904223;
			pal_write(i, seed[23:12]);
		end
		pal_write(NPAL - 1, 12'hf0f);
		pal_write(8'd0, 12'h123);
		// an entry above the palette's size wraps onto entry 16
		// (6 planes) -- on 8 it is an ordinary entry
		pal_write(8'd80, 12'h0f0);
		set_cfg(1, 1, 200, 100, 1, NP - 1, 6'b11_10_01);
		check_frame("palette rewritten, every entry", 1);

		// ---- 8: colour enabled, game mode off: desktop untouched ----
		set_cfg(0, 0, 0, 0, 1, NP - 1, 6'b11_10_01);
		next_frame();
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
