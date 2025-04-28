/*
 * BRAM
 * Copyright (c) 2023 Lone Dynamics Corporation. All rights reserved.
 *
 * 8KB BRAM as 2K 32-bit words
 *
 * This is mapped at 0x0000_0000.
 *
 * This is where the firmware (first stage bootloader / BIOS) is stored.
 * It's also used for interrupt handlers and the interrupt stack.
 *
 */

module bram_wb #()
(
	input wb_clk_i,
	input wb_rst_i,
	input [29:0] wb_adr_i,
	input [31:0] wb_dat_i,
	output [31:0] wb_dat_o,
	input wb_we_i,
	input [3:0] wb_sel_i,
	input wb_stb_i,
	output wb_ack_o,
	input wb_cyc_i,
);

	reg [31:0] ram [0:2047];
	initial $readmemh("sw/bios/bios_seed.hex", ram);

	reg [31:0] dat_o;
	reg ack_o;

	assign wb_ack_o = ack_o;
	assign wb_dat_o = dat_o;

	always @(posedge wb_clk_i) begin
		ack_o <= 0;
		if (wb_cyc_i && wb_stb_i && !ack_o) begin
			if (wb_we_i) begin
				if (wb_sel_i[0]) ram[wb_adr_i][7:0] <= wb_dat_i[7:0];
				if (wb_sel_i[1]) ram[wb_adr_i][15:8] <= wb_dat_i[15:8];
				if (wb_sel_i[2]) ram[wb_adr_i][23:16] <= wb_dat_i[23:16];
				if (wb_sel_i[3]) ram[wb_adr_i][31:24] <= wb_dat_i[31:24];
			end else begin
				dat_o <= ram[wb_adr_i];
			end
			ack_o <= 1;
		end
	end

endmodule
