/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SPI bit-bang interface; requires a dedicated SPI bus.
 *
 */

module spibb_wb #()
(
	input wb_clk_i,
	input wb_rst_i,
	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o,
	input wb_we_i,
	input [3:0] wb_sel_i,
	input wb_stb_i,
	output wb_ack_o,
	input wb_cyc_i,
	output reg led,
	output reg [7:0] leds,
	output reg sd_ss, sd_sck, sd_mosi,
	input sd_miso
);

	reg ack;
	assign wb_ack_o = ack;

	always @(posedge wb_clk_i) begin

		ack <= 0;

      if (wb_rst_i) begin
         led <= 1;
         leds <= 8'h00;
      end else if (wb_cyc_i && wb_stb_i && wb_we_i) begin
			{sd_ss, sd_sck, sd_mosi} <= wb_dat_i[3:1];
			ack <= 1;
      end else if (wb_cyc_i && wb_stb_i) begin
			wb_dat_o = { 28'b0, sd_ss, sd_sck, sd_mosi, sd_miso };
			ack <= 1;
		end
	end

endmodule
