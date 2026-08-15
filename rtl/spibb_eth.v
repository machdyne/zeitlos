/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SPI bit-bang interface for the ENC28J60 Ethernet controller (PMOD).
 * Same pattern as spibb.v (SD card): all actual SPI protocol timing
 * happens in software, this just exposes the raw pins as one
 * memory-mapped register. Kept as a separate module from spibb.v
 * (rather than generalizing that one) to avoid touching the existing,
 * working SD card driver while bringing up new, unavoidably riskier
 * networking code.
 *
 * Register (single word):
 *   write: bits [3:1] = {eth_ss, eth_sck, eth_mosi} (same bit
 *     positions spibb.v uses for the SD card, so the software SPI
 *     bit-bang routine can be near-identical to sdmm.c's).
 *   read: bit 0 = eth_miso, bits [3:1] = last-written
 *     {eth_ss,eth_sck,eth_mosi} (readback, same as spibb.v), bit 4 =
 *     eth_int (the chip's interrupt line, active low -- not used as a
 *     real interrupt in v1, just readable so software can cheaply
 *     poll it before spending an SPI transaction checking for a
 *     packet, or so a future interrupt-driven driver has it without
 *     needing new RTL).
 */

module spibb_eth_wb #()
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
	output reg eth_ss, eth_sck, eth_mosi,
	input eth_miso,
	input eth_int
);

	reg ack;
	assign wb_ack_o = ack;

	always @(posedge wb_clk_i) begin

		ack <= 0;

		if (wb_rst_i) begin
			eth_ss <= 1;	// deasserted (active low) at reset
			eth_sck <= 0;
			eth_mosi <= 0;
		end else if (wb_cyc_i && wb_stb_i && wb_we_i) begin
			{eth_ss, eth_sck, eth_mosi} <= wb_dat_i[3:1];
			ack <= 1;
		end else if (wb_cyc_i && wb_stb_i) begin
			wb_dat_o = { 27'b0, eth_int, eth_ss, eth_sck, eth_mosi, eth_miso };
			ack <= 1;
		end
	end

endmodule
