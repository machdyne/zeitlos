/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SPI Flash Read-Only Controller
 *
 */

module spiflashro_wb #()
(
	input wb_clk_i,
	input wb_rst_i,

	input [31:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output reg [31:0] wb_dat_o,
	input [3:0] wb_sel_i,
	input wb_we_i,
	input wb_cyc_i,
	input wb_stb_i,
	output wb_ack_o,

	output reg ss,
	output sck,
	output mosi,
	input miso,
);

	assign wb_ack_o = ack;
	wire valid = wb_cyc_i && wb_stb_i && !wb_we_i && !ack;

	reg ack;
	reg [3:0] state;

	localparam [3:0]
		STATE_IDLE			= 4'd0,
		STATE_INIT			= 4'd1,
		STATE_START			= 4'd2,
		STATE_CMD			= 4'd3,
		STATE_ADDR			= 4'd4,
		STATE_WAIT			= 4'd5,
		STATE_XFER			= 4'd6,
		STATE_END			= 4'd7;

	reg [31:0] buffer;
	reg [5:0] xfer_bits;

	reg mosi_do;
	reg sck_do;

	assign mosi = valid ? mosi_do : 1'bz;
	assign sck = valid ? sck_do : 1'bz;

	always @(posedge wb_clk_i) begin

		if (wb_rst_i) begin

			ack <= 0;
			ss <= 1;
			sck_do <= 0;

			xfer_bits <= 0;

			state <= STATE_IDLE;

		end else if (valid && state == STATE_IDLE) begin

			state <= STATE_INIT;
			xfer_bits <= 0;

		end else if (!valid && ack) begin

			ack <= 0;

		end else if (xfer_bits) begin

			mosi_do <= buffer[31];

			if (sck_do) begin
				sck_do <= 0;
			end else begin
				sck_do <= 1;
				buffer <= { buffer, miso };
				xfer_bits <= xfer_bits - 1;
			end

		end else case (state)

			STATE_IDLE: begin
				ss <= 1;
			end

			STATE_INIT: begin
				sck_do <= 0;
				state <= STATE_START;
			end

			STATE_START: begin
				ss <= 0;
				state <= STATE_CMD;
			end

			STATE_CMD: begin
				buffer[31:24] <= 8'h03;
				xfer_bits <= 8;
				state <= STATE_ADDR;
			end

			STATE_ADDR: begin
				buffer[31:8] <= wb_adr_i[23:0];
				xfer_bits <= 24;
				state <= STATE_XFER;
			end

			STATE_XFER: begin
				xfer_bits <= 32;
				state <= STATE_END;
			end

			STATE_END: begin
				wb_dat_o <= {
					buffer[7:0], buffer[15:8], buffer[23:16], buffer[31:24]
				};
				ack <= 1;
				state <= STATE_IDLE;
			end

		endcase

	end

endmodule
