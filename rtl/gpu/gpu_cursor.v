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

	always @(posedge pclk) begin

		if (
			(gpu_x == curs_x - 2 && gpu_y == curs_y - 2) ||
			(gpu_x == curs_x - 1 && gpu_y == curs_y - 1) ||
			(gpu_x == curs_x     && gpu_y == curs_y    ) ||
			(gpu_x == curs_x + 1 && gpu_y == curs_y + 1) ||
			(gpu_x == curs_x + 2 && gpu_y == curs_y + 2) ||
			(gpu_x == curs_x + 2 && gpu_y == curs_y - 2) ||
			(gpu_x == curs_x + 1 && gpu_y == curs_y - 1) ||
			(gpu_x == curs_x - 1 && gpu_y == curs_y + 1) ||
			(gpu_x == curs_x - 2 && gpu_y == curs_y + 2)
		)
			pixel <= 1'b1;
		else
			pixel <= 1'b0;

	end

endmodule
