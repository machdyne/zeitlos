/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DMA Controller
 *
 */

module dma #()
(

	input wb_rst_i,
	input wb_clk_i,

	output reg [31:0] wbm_adr_o,
	output reg [31:0] wbm_dat_o,
	input [31:0] wbm_dat_i,
	output reg wbm_we_o,
	output reg [3:0] wbm_sel_o,
	output reg wbm_stb_o,
	input wbm_ack_i,
	output reg wbm_cyc_o,

);

	always @(posedge wb_clk_i) begin

		if (wb_rst_i) begin

		end else begin

			// DMA state machine
			// see picorv32_wb for an example

		end

	end

endmodule
