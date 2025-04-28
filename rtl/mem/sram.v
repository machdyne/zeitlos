/*
 * SRAM
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Currently only supports 32-bit SRAM on Obst.
 *
 */

module sram_wb #()
(
	input wb_clk_i,
	input wb_rst_i,
	input [29:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output [31:0] wb_dat_o,
	input wb_we_i,
	input [3:0] wb_sel_i,
	input wb_stb_i,
	output reg wb_ack_o,
	input wb_cyc_i,
	inout [31:0] sram_d,
	output [18:0] sram_a,
	output [1:0] sram_cs,
	output [1:0] sram_oe,
	output [1:0] sram_we,
	output [1:0] sram_ub,
	output [1:0] sram_lb,
);

	wire write = wb_cyc_i && wb_we_i;
	wire read = wb_cyc_i && ~wb_we_i;

	wire [1:0] wr = { write && (|wb_sel_i[3:2]), write && (|wb_sel_i[1:0]) };
	wire [1:0] rd = { read && (|wb_sel_i[3:2]), read && (|wb_sel_i[1:0]) };

	wire [1:0] cs = { wb_sel_i[3] || wb_sel_i[2], wb_sel_i[1] || wb_sel_i[0] };

	assign sram_cs = 2'b00;
	assign sram_oe = { wb_we_i, wb_we_i };

	assign wb_dat_o = sram_d;

	assign sram_d = write ? wb_dat_i : 32'hzzzz_zzzz;
	assign sram_a = wb_adr_i;

	assign sram_we = { !(wr[1] && !wb_clk_i), !(wr[0] && !wb_clk_i) };

	assign sram_ub = { cs[1] ? !wb_sel_i[3] : 1'b0, cs[0] ? !wb_sel_i[1] : 1'b0 };
	assign sram_lb = { cs[1] ? !wb_sel_i[2] : 1'b0, cs[0] ? !wb_sel_i[0] : 1'b0 };

	reg ack_pending;

	always @(posedge wb_clk_i) begin

		if (wb_rst_i) begin
			wb_ack_o <= 1'b0;
			ack_pending <= 1'b0;
		end else begin

			if (wb_ack_o) begin
				wb_ack_o <= 1'b0;
			end else if (ack_pending) begin
				wb_ack_o <= 1'b1;
				ack_pending <= 1'b0;
			end else begin
				ack_pending <= 1'b1;
			end

		end

	end

endmodule
