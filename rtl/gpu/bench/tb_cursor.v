`timescale 1ns / 1ps

// Self-checking testbench for rtl/gpu/gpu_video.v's hardware cursor
// (docs/cursor.md).
//
// Two checks at every visible pixel of every checked frame:
//
//   SAME AS BEFORE. A second gpu_video, built without the cursor
//   (CURSOR_AVAIL 0), gets its pointer from the comparator cursor the
//   new one replaced (gpu_cursor_ref.v, the old gpu_cursor.v renamed)
//   through the `pixel` input, exactly as rtl/sysctl.v used to wire
//   it. Both see the same VRAM, mode and pointer. The monochrome
//   output (pset) and the colour index (cidx) must be identical,
//   pixel for pixel, for the power-on X and Z: in desktop mode at the
//   centre, every corner and edge, half off each side; with the cursor
//   hidden; in game mode (pixel-doubled), across the wrap seam, in
//   paper mode, and in 16 colours.
//
//   THE SHAPE. A model of the cell -- dx = x - curs_x, dy = y -
//   curs_y, row dy + 8 of the slot, bit dx + 8 -- checks the cursor
//   bit itself (dut.c_d, one clock behind x as `pixel` always was),
//   for the power-on shapes, the 9x9 pair in slots 2 and 3, and shapes
//   written through curs_we while the screen is being drawn.
//
// Running it (same trailing-comma patch as the other video benches,
// see rtl/gpu/bench/README.md):
//
//   sed 's/^\tinput \[31:0\] gb_dat_i,$/\tinput [31:0] gb_dat_i/' \
//       rtl/gpu/gpu_video.v > /tmp/gpu_video_fix.v
//   iverilog -g2005 -o /tmp/tb_cursor.out rtl/gpu/bench/tb_cursor.v \
//       rtl/gpu/bench/gpu_cursor_ref.v /tmp/gpu_video_fix.v
//   vvp /tmp/tb_cursor.out
//
// About sixty frames: a few minutes.

module tb_cursor;

	localparam H_START = 160, V_START = 45;

	reg clk = 0;
	reg pclk = 0;
	reg bclk = 0;
	reg resetn = 0;

	reg [1:0] video_mode = 2'd0;

	reg view_load = 0;
	reg game_en = 0;
	reg game_wrap = 0;
	reg [9:0] view_x = 0;
	reg [9:0] view_y = 0;
	reg color_en = 0;
	reg [1:0] color_np = 0;
	reg [5:0] color_off = 0;

	reg [9:0] curs_x = 320;
	reg [9:0] curs_y = 240;
	reg curs_alt = 0;
	reg curs_hide = 0;
	reg [3:0] curs_slots = 4'b0100;
	reg curs_we = 0;
	reg [5:0] curs_waddr = 0;
	reg [31:0] curs_wdata = 0;

	always #10.417 clk  = ~clk;
	always #19.841 pclk = ~pclk;

	// -- the new cursor --
	wire [9:0] x, y;
	wire [14:0] gb_adr_o;
	reg [31:0] gb_dat_i = 0;
	wire red, green, blue, hsync, vsync, is_visible, in_vblank;
	wire [3:0] dvi_p, dac;
	wire [15:0] frame_ctr;

	gpu_video #(.COLOR_AVAIL(1'b1), .CURSOR_AVAIL(1'b1)) dut (
		.pixel(1'b0),
		.curs_x(curs_x), .curs_y(curs_y),
		.curs_alt(curs_alt), .curs_hide(curs_hide),
		.curs_slots(curs_slots),
		.curs_we(curs_we), .curs_waddr(curs_waddr), .curs_wdata(curs_wdata),
		.clk(clk), .pclk(pclk), .bclk(bclk), .resetn(resetn),
		.video_mode(video_mode),
		.view_load(view_load), .game_en(game_en), .game_wrap(game_wrap),
		.view_x(view_x), .view_y(view_y),
		.color_en(color_en), .color_np(color_np), .color_off(color_off),
		.pal_we(1'b0), .pal_idx(4'd0), .pal_rgb(12'd0),
		.frame_ctr(frame_ctr), .in_vblank(in_vblank),
		.red(red), .green(green), .blue(blue), .hsync(hsync), .vsync(vsync),
		.dvi_p(dvi_p), .dac(dac),
		.x(x), .y(y), .is_visible(is_visible),
		.gb_adr_o(gb_adr_o), .gb_dat_i(gb_dat_i)
	);

	// -- the old one, as the reference --
	wire [9:0] rx, ry;
	wire [14:0] r_gb_adr_o;
	reg [31:0] r_gb_dat_i = 0;
	wire r_red, r_green, r_blue, r_hsync, r_vsync, r_is_visible, r_in_vblank;
	wire [3:0] r_dvi_p, r_dac;
	wire [15:0] r_frame_ctr;
	wire ref_pix;

	gpu_cursor_ref rc (
		.pclk(pclk), .pixel(ref_pix),
		.gpu_x(rx), .gpu_y(ry),
		.curs_x(curs_x), .curs_y(curs_y),
		.curs_alt(curs_alt), .curs_hide(curs_hide)
	);

	gpu_video #(.COLOR_AVAIL(1'b1)) rv (
		.pixel(ref_pix),
		.curs_x(10'd0), .curs_y(10'd0), .curs_alt(1'b0), .curs_hide(1'b0),
		.curs_slots(4'd0), .curs_we(1'b0), .curs_waddr(6'd0), .curs_wdata(32'd0),
		.clk(clk), .pclk(pclk), .bclk(bclk), .resetn(resetn),
		.video_mode(video_mode),
		.view_load(view_load), .game_en(game_en), .game_wrap(game_wrap),
		.view_x(view_x), .view_y(view_y),
		.color_en(color_en), .color_np(color_np), .color_off(color_off),
		.pal_we(1'b0), .pal_idx(4'd0), .pal_rgb(12'd0),
		.frame_ctr(r_frame_ctr), .in_vblank(r_in_vblank),
		.red(r_red), .green(r_green), .blue(r_blue), .hsync(r_hsync), .vsync(r_vsync),
		.dvi_p(r_dvi_p), .dac(r_dac),
		.x(rx), .y(ry), .is_visible(r_is_visible),
		.gb_adr_o(r_gb_adr_o), .gb_dat_i(r_gb_dat_i)
	);

	// -- VRAM, shared --
	reg [31:0] vram [0:9599];
	always @(posedge clk) begin
		gb_dat_i <= vram[gb_adr_o];
		r_gb_dat_i <= vram[r_gb_adr_o];
	end

	// -- the shape model: what each slot holds --
	reg [15:0] shape [0:63];

	// x and y as the cursor register sampled them: one clock behind
	reg [9:0] px1, py1;
	always @(posedge pclk) begin
		px1 <= x;
		py1 <= y;
	end

	integer dx, dy, sl;
	reg want;
	reg check_ref = 0;
	reg use_ref = 1;
	reg check_shape = 0;
	integer bad_ref = 0, bad_shape = 0, seen = 0, errors = 0;

	always @(negedge pclk) begin
		if (is_visible && check_ref) begin
			if (dut.pset !== rv.pset || dut.cidx !== rv.cidx) begin
				if (bad_ref < 8)
					$display("FAIL: (%0d,%0d) fb (%0d,%0d): pset %b/%b cidx %h/%h, pointer (%0d,%0d)",
						dut.hc - H_START, dut.vc - V_START, x, y,
						dut.pset, rv.pset, dut.cidx, rv.cidx, curs_x, curs_y);
				bad_ref = bad_ref + 1;
			end
		end
		if (is_visible && (check_ref || check_shape) && dut.c_d)
			seen = seen + 1;
		if (is_visible && check_shape) begin
			dx = px1 - curs_x + 8;
			dy = py1 - curs_y + 8;
			sl = curs_alt ? curs_slots[3:2] : curs_slots[1:0];
			want = !curs_hide && dx >= 0 && dx < 16 && dy >= 0 && dy < 16 &&
				shape[sl * 16 + dy][dx];
			if (dut.c_d !== want) begin
				if (bad_shape < 8)
					$display("FAIL: shape: fb (%0d,%0d), pointer (%0d,%0d), slot %0d: %b, expected %b",
						px1, py1, curs_x, curs_y, sl, dut.c_d, want);
				bad_shape = bad_shape + 1;
			end
		end
	end

	// -- frames --
	task next_frame;
		begin
			@(posedge in_vblank);
		end
	endtask

	// Change things at the start of vertical blanking; check the frame
	// after the one that adopts them (the viewport and colour are
	// adopted at a frame boundary).
	task frame;
		input [9:0] cx;
		input [9:0] cy;
		input alt;
		input hide;
		input [80*8:1] what;
		integer b0, s0;
		begin
			check_ref = 0;
			check_shape = 0;
			curs_x = cx;
			curs_y = cy;
			curs_alt = alt;
			curs_hide = hide;
			next_frame;
			b0 = bad_ref + bad_shape;
			s0 = seen;
			check_ref = use_ref;
			check_shape = 1;
			next_frame;
			check_ref = 0;
			check_shape = 0;
			if (bad_ref + bad_shape != b0) begin
				$display("  %0s: pointer (%0d,%0d)%0s%0s: FAILED", what, cx, cy,
					alt ? " busy" : "", hide ? " hidden" : "");
				errors = errors + 1;
			end else if (!hide && seen == s0 && cx < 640 && cy < 480) begin
				$display("  %0s: pointer (%0d,%0d): no cursor pixel seen", what, cx, cy);
				errors = errors + 1;
			end else
				$display("  ok  %0s: pointer (%0d,%0d)%0s%0s, %0d cursor pixels",
					what, cx, cy, alt ? " busy" : "", hide ? " hidden" : "", seen - s0);
		end
	endtask

	task mode;
		input g;
		input w;
		input [9:0] vx;
		input [9:0] vy;
		input c;
		begin
			game_en = g;
			game_wrap = w;
			view_x = vx;
			view_y = vy;
			color_en = c;
			color_np = c ? 2'd3 : 2'd0;
			color_off = c ? 6'b11_10_01 : 6'd0;
			view_load = ~view_load;
			next_frame;
		end
	endtask

	// one shape row, written through the port while the screen draws
	task put_row;
		input [5:0] a;
		input [15:0] bits;
		begin
			@(posedge clk);
			curs_waddr <= a;
			curs_wdata <= { 16'hdead, bits };	// the top half is not used
			curs_we <= 1'b1;
			@(posedge clk);
			curs_we <= 1'b0;
			shape[a] = bits;
		end
	endtask

	integer i, j;
	reg [31:0] r;

	initial begin
		r = 32'h1234_5678;
		for (i = 0; i < 9600; i = i + 1) begin
			r = r ^ (r << 13); r = r ^ (r >> 17); r = r ^ (r << 5);
			vram[i] = r;
		end
		// the model's power-on shapes: what tools/gen_cursor_init.py
		// put in gpu_video.v, read back from the DUT's own block RAM
		for (i = 0; i < 64; i = i + 1)
			shape[i] = dut.cbuf[i][15:0];

		#200 resetn = 1;
		next_frame;
		next_frame;

		$display("desktop, X and Z against the old cursor:");
		frame(320, 240, 0, 0, "centre X");
		frame(320, 240, 1, 0, "centre Z");
		frame(0, 0, 0, 0, "top left");
		frame(1, 1, 1, 0, "one in");
		frame(2, 2, 0, 0, "two in");
		frame(639, 479, 0, 0, "bottom right");
		frame(638, 478, 1, 0, "one in, bottom right");
		frame(639, 0, 1, 0, "top right");
		frame(0, 479, 0, 0, "bottom left");
		frame(31, 100, 0, 0, "on a word edge");
		frame(32, 101, 1, 0, "past a word edge");
		frame(400, 300, 0, 1, "hidden");

		$display("paper:");
		video_mode = 2'd3;
		frame(100, 50, 0, 0, "paper X");
		frame(101, 51, 1, 0, "paper Z");
		video_mode = 2'd0;

		$display("game mode, pixel-doubled:");
		mode(1, 0, 100, 60, 0);
		frame(260, 180, 0, 0, "game X");
		frame(100, 60, 1, 0, "game, the origin");
		mode(1, 1, 600, 0, 0);
		frame(1, 120, 0, 0, "wrap, just past the seam");
		frame(638, 120, 1, 0, "wrap, just before it");

		$display("16 colours:");
		mode(1, 0, 0, 0, 1);
		frame(160, 120, 0, 0, "colour X");
		frame(2, 2, 1, 0, "colour Z, top left");
		mode(0, 0, 0, 0, 0);

		$display("the shapes:");
		use_ref = 0;
		curs_slots = 4'b11_10;		// the 9x9 pair
		frame(300, 200, 0, 0, "slot 2 (9x9 X)");
		frame(5, 470, 1, 0, "slot 3 (9x9 Z), bottom left");
		// a shape of one's own, written while the screen is drawn
		next_frame;
		@(negedge in_vblank);
		repeat (2000) @(posedge clk);
		for (i = 0; i < 16; i = i + 1) begin
			r = r ^ (r << 13); r = r ^ (r >> 17); r = r ^ (r << 5);
			put_row(6'd32 + i, r[15:0]);
			repeat (37) @(posedge clk);
			put_row(6'd48 + i, ~r[31:16]);
			repeat (1000) @(posedge clk);
		end
		frame(320, 240, 0, 0, "slot 2, written");
		frame(8, 8, 1, 0, "slot 3, written, top left");
		frame(639, 479, 0, 0, "slot 2, bottom right");
		curs_slots = 4'b01_00;
		use_ref = 1;
		frame(330, 250, 0, 0, "back to X");

		if (errors == 0 && bad_ref == 0 && bad_shape == 0)
			$display("tb_cursor: all checks passed");
		else
			$display("tb_cursor: %0d check(s) failed (%0d against the old cursor, %0d against the shape)",
				errors, bad_ref, bad_shape);
		$finish;
	end

endmodule
