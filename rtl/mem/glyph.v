/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Glyph memory: a small dual-port BRAM holding font bitmap data for
 * the hardware glyph blitter (see rtl/gpu/gpu_blit.v). Software loads
 * font data in via the Wishbone slave port at boot/font-change time;
 * the blitter reads it back via a separate, direct (non-Wishbone)
 * port with no bus arbitration, since glyph reads should never have
 * to wait on anything else.
 *
 * Layout: one byte per glyph row, MSB-first (bit 7 = leftmost pixel --
 * matching sw/data/font/*.mem and sw/common/zfont_data.c), addressed
 * as glyph_index * font_height + row. See docs/window_manager.md,
 * "hardware glyph blitting" for the full convention this needs to
 * match on the software side.
 *
 * Default size (4096 bytes) comfortably fits either existing font
 * (96 glyphs * 16 rows = 1536 bytes for the 8x16 font; 96 * 12 = 1152
 * for the 6x12 font) with room to spare for a future 6x6 font or a
 * larger character set later.
 */

module glyph_mem #(
	parameter ADDR_WIDTH = 12   // 2^12 = 4096 bytes
)
(
	input wire clk,

	// -- port A: Wishbone slave, CPU read/write (font loading) --
	input  wire        wb_cyc_i,
	input  wire        wb_stb_i,
	input  wire        wb_we_i,
	input  wire [3:0]  wb_sel_i,
	input  wire [31:0] wb_adr_i,
	input  wire [31:0] wb_dat_i,
	output reg         wb_ack_o,
	output reg  [31:0] wb_dat_o,

	// -- port B: direct read-only port for the blitter --
	// registered (1 cycle latency, standard synchronous BRAM read):
	// present blit_addr, blit_data is valid the following cycle.
	input  wire [ADDR_WIDTH-1:0] blit_addr,
	output reg  [7:0]            blit_data
);

	reg [7:0] mem [0:(1 << ADDR_WIDTH) - 1];

	// -- port A: byte-addressable (within each 32-bit word via
	// wb_sel_i), word-indexed addressing --
	// wb_adr_i arrives already word-shifted by sysctl.v (same
	// convention gpu_blit_wb's register file uses: it's
	// wbm_adr_sel_word, i.e. the byte offset within this peripheral's
	// address-decoded region, already divided by 4) -- so the low
	// bits of wb_adr_i are directly the word index, not a byte address
	// needing another shift.

	wire [ADDR_WIDTH-3:0] word_addr = wb_adr_i[ADDR_WIDTH-3:0];

	always @(posedge clk) begin

		wb_ack_o <= 1'b0;

		if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin

			wb_ack_o <= 1'b1;

			if (wb_we_i) begin
				if (wb_sel_i[0]) mem[{word_addr, 2'b00}] <= wb_dat_i[7:0];
				if (wb_sel_i[1]) mem[{word_addr, 2'b01}] <= wb_dat_i[15:8];
				if (wb_sel_i[2]) mem[{word_addr, 2'b10}] <= wb_dat_i[23:16];
				if (wb_sel_i[3]) mem[{word_addr, 2'b11}] <= wb_dat_i[31:24];
			end else begin
				wb_dat_o <= {
					mem[{word_addr, 2'b11}], mem[{word_addr, 2'b10}],
					mem[{word_addr, 2'b01}], mem[{word_addr, 2'b00}]
				};
			end

		end

	end

	// -- port B: blitter read --
	always @(posedge clk) begin
		blit_data <= mem[blit_addr];
	end

endmodule
