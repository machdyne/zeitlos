/*
 * DEBUG
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Debug interface.
 *
 */

module debug_wb #()
(
	input wb_clk_i,
	input wb_rst_i,
	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output [31:0] wb_dat_o,
	input wb_we_i,
	input [3:0] wb_sel_i,
	input wb_stb_i,
	output wb_ack_o,
	input wb_cyc_i,
	output reg led,
	output reg [7:0] leds,
);

	assign wb_ack_o = wb_cyc_i && wb_stb_i;
	assign wb_dat_o = wb_adr_i == 1 ? { 24'h0, leds } : { 31'h0, led };

	always @(posedge wb_clk_i) begin

      if (wb_rst_i) begin
         led <= 1;
         leds <= 8'h00;
      end else if (wb_cyc_i && wb_stb_i && wb_we_i) begin
			if (wb_adr_i == 0) led <= wb_dat_i[0];
			if (wb_adr_i == 1) leds <= wb_dat_i[7:0];
		end
	end

endmodule
