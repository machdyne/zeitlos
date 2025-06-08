/*
 * Zeitlos SOC GPU
 * Copyright (c) 2021 Lone Dynamics Corporation. All rights reserved.
 *
 * Video timing generator for 1024x768@75Hz
 *
 */

module gpu_video #(

	parameter [10:0] h_disp = 1024,
	parameter [10:0] h_front_porch = 24,
	parameter [10:0] h_pulse_width = 136,
	parameter [10:0] h_back_porch = 144,
	parameter [10:0] h_line = 1328,
	parameter [10:0] v_disp = 768,
	parameter [10:0] v_front_porch = 3,
	parameter [10:0] v_pulse_width = 6,
	parameter [10:0] v_back_porch = 29,
	parameter [10:0] v_frame = 806

) (

	input pixel,

	input clk,
	input pclk,
	input bclk,
	input resetn,

	output red,
	output green,
	output blue,
	output hsync,
	output vsync,

	output [3:0] dvi_p,

	output [3:0] dac,

	output reg [9:0] x,
	output reg [9:0] y,
	output is_visible,

	output reg [14:0] gb_adr_o,
	input [31:0] gb_dat_i,

);

	reg [10:0] hc;
	reg [10:0] vc;

   wire [9:0] hx = x >> 1;
   wire [9:0] hy = y >> 1;

`ifdef GPU_PIXEL_DOUBLE
   wire pset = is_visible && (hline[hx] || pixel);
`else
   wire pset = is_visible && (hline[x] || pixel);
`endif

	assign red = pset;
	assign green = pset;
	assign blue = pset;

`ifdef GPU_DDMI

	wire [1:0] out_tmds_red;
	wire [1:0] out_tmds_green;
	wire [1:0] out_tmds_blue;
	wire [1:0] out_tmds_clk;

	ODDRX1F ddr0_clock (.D0(out_tmds_clk   [0] ), .D1(out_tmds_clk   [1] ), .Q(dvi_p[3]), .SCLK(bclk), .RST(0));
	ODDRX1F ddr0_red   (.D0(out_tmds_red   [0] ), .D1(out_tmds_red   [1] ), .Q(dvi_p[2]), .SCLK(bclk), .RST(0));
	ODDRX1F ddr0_green (.D0(out_tmds_green [0] ), .D1(out_tmds_green [1] ), .Q(dvi_p[1]), .SCLK(bclk), .RST(0));
	ODDRX1F ddr0_blue  (.D0(out_tmds_blue  [0] ), .D1(out_tmds_blue  [1] ), .Q(dvi_p[0]), .SCLK(bclk), .RST(0));

	gpu_ddmi #() gpu_ddmi_i
	(
		.pclk(pclk),
		.tmds_clk(bclk),
		.in_vga_red({ red, 7'b0 }),
		.in_vga_green({ green, 7'b0 }),
		.in_vga_blue({ blue, 7'b0 }),
		.in_vga_blank(!is_visible),
		.in_vga_vsync(vsync),
		.in_vga_hsync(hsync),
		.out_tmds_red(out_tmds_red),
		.out_tmds_green(out_tmds_green),
		.out_tmds_blue(out_tmds_blue),
		.out_tmds_clk(out_tmds_clk)
	);

`endif

`ifdef GPU_COMPOSITE

	// TODO

`endif

	// video timing

	parameter [10:0] h_disp_start = h_front_porch + h_pulse_width + h_back_porch;
	parameter [10:0] h_disp_stop = h_disp_start + h_disp;

	parameter [10:0] v_disp_start = v_front_porch + v_pulse_width + v_back_porch;
	parameter [10:0] v_disp_stop = v_disp_start + v_disp;

	assign is_visible = (hc >= h_disp_start && vc >= v_disp_start &&
		hc < h_disp_stop && vc < v_disp_stop);

	assign hsync = (hc < h_front_porch) ||
		(hc >= h_front_porch + h_pulse_width);
	assign vsync = (vc < v_front_porch) ||
		(vc >= v_front_porch + v_pulse_width);

	reg refill;
	reg refill_toggle;
	reg [1:0] refill_sync;
	wire refill_synced = refill_sync[1] ^ refill_sync[0];

	always @(posedge clk) begin
		refill <= refill_synced;
		if (!resetn) begin
			refill_sync <= 0;
		end else begin
			refill_sync <= {refill_sync[0], refill_toggle}; 
		end
	end

	always @(posedge pclk) begin

		if (!resetn) begin
			hc <= 0;
			vc <= 0;
			refill_toggle <= 0;
		end else if (hc == h_disp_stop - 1) begin
			refill_toggle <= ~refill_toggle;
			hc <= 0;
			if (vc == v_disp_stop - 1) begin
				vc <= 0;
			end else begin
				vc <= vc + 1;
				if (vc > v_disp_start) y <= vc - v_disp_start; else y <= 0;
			end
		end else begin
			hc <= hc + 1;
			if (hc > h_disp_start) x <= hc - h_disp_start; else x <= 0;
		end

	end

	reg [4:0] refill_words;

`ifdef GPU_PIXEL_DOUBLE
	reg [511:0] hline;
`else
	reg [1023:0] hline;
`endif

	always @(posedge clk) begin
		if (refill) begin
`ifdef GPU_PIXEL_DOUBLE
			refill_words <= 16;
`else
			refill_words <= 32;
`endif
		end else if (refill_words) begin
`ifdef GPU_PIXEL_DOUBLE
			gb_adr_o <= (hy << 4) + (refill_words - 1);
`else
			gb_adr_o <= (y << 5) + (refill_words - 1);
`endif
			hline <= { hline, gb_dat_i };
			refill_words <= refill_words - 1;
		end
	end

endmodule
