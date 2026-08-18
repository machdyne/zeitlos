/*
 * Zeitlos SOC GPU
 * Copyright (c) 2021 Lone Dynamics Corporation. All rights reserved.
 *
 * Cursor.
 *
 */

module gpu_cursor #()
(

	input pclk,
	output reg pixel,

	input [9:0] gpu_x,
	input [9:0] gpu_y,

	input [9:0] curs_x,
	input [9:0] curs_y,

);

	// the sprite is a 5x5 diamond of points, each an offset (-2..+2)
	// from the cursor position. computing those offsets directly in
	// curs_x/y's own [9:0] unsigned width wraps at the 0/1023
	// boundary -- e.g. curs_x - 2 when curs_x < 2 wraps around to
	// ~1021 instead of going negative -- which can land back inside
	// gpu_x/gpu_y's valid range and draw a stray pixel on the
	// opposite edge of the screen as the cursor crosses 0. compute
	// each offset with extra headroom bits (signed) instead, and only
	// match it if it's still non-negative -- an offset that goes
	// off-screen just doesn't draw that one point of the sprite,
	// rather than wrapping to the other side.
	//
	// note: curs_x/curs_y are compared against gpu_x/gpu_y directly,
	// unscaled -- both now range 0..639/0..479 (native 640x480, see
	// rtl/gpu/gpu_video.v and rtl/usb_hid.v's own curs_x/curs_y clamp
	// bounds), so no scaling is needed at all. An earlier revision
	// divided them by 2 on the theory that curs_x/y's native range
	// was double the framebuffer resolution (true back when the
	// framebuffer was 512x384 displayed via 2x pixel-doubling at
	// 1024x768) -- that's gone now, this module's own logic never
	// needed to change for it, only the upstream ranges did.
	wire signed [11:0] cx = { 2'b00, curs_x };
	wire signed [11:0] cy = { 2'b00, curs_y };

	wire signed [11:0] xm2 = cx - 12'sd2;
	wire signed [11:0] xm1 = cx - 12'sd1;
	wire signed [11:0] xp1 = cx + 12'sd1;
	wire signed [11:0] xp2 = cx + 12'sd2;

	wire signed [11:0] ym2 = cy - 12'sd2;
	wire signed [11:0] ym1 = cy - 12'sd1;
	wire signed [11:0] yp1 = cy + 12'sd1;
	wire signed [11:0] yp2 = cy + 12'sd2;

	always @(posedge pclk) begin

		if (
			(xm2 >= 0 && ym2 >= 0 && gpu_x == xm2[9:0] && gpu_y == ym2[9:0]) ||
			(xm1 >= 0 && ym1 >= 0 && gpu_x == xm1[9:0] && gpu_y == ym1[9:0]) ||
			(gpu_x == curs_x && gpu_y == curs_y) ||
			(gpu_x == xp1[9:0]    && gpu_y == yp1[9:0]) ||
			(gpu_x == xp2[9:0]    && gpu_y == yp2[9:0]) ||
			(gpu_x == xp2[9:0]    && ym2 >= 0 && gpu_y == ym2[9:0]) ||
			(gpu_x == xp1[9:0]    && ym1 >= 0 && gpu_y == ym1[9:0]) ||
			(xm1 >= 0 && gpu_x == xm1[9:0] && gpu_y == yp1[9:0]) ||
			(xm2 >= 0 && gpu_x == xm2[9:0] && gpu_y == yp2[9:0])
		)
			pixel <= 1'b1;
		else
			pixel <= 1'b0;

	end

endmodule
