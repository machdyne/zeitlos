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

	// word-addressable storage (32 bits per element, ADDR_WIDTH-2
	// bits of word index) -- deliberately matching rtl/mem/vram.v's
	// own array shape: a single 32-bit-wide array, part-select byte
	// writes for the individual lanes, rather than a byte-addressable
	// array written through four separate per-lane index expressions.
	reg [31:0] mem [0:(1 << (ADDR_WIDTH-2)) - 1];

	// wb_adr_i arrives already word-shifted by sysctl.v (same
	// convention gpu_blit_wb's register file uses: it's
	// wbm_adr_sel_word, i.e. the byte offset within this peripheral's
	// address-decoded region, already divided by 4) -- so the low
	// bits of wb_adr_i are directly the word index, not a byte address
	// needing another shift.
	//
	// IMPORTANT: mem[] is indexed with a part-select of wb_adr_i
	// INLINE, at each individual use below, rather than being
	// factored out into its own named `wire word_addr =
	// wb_adr_i[...]`. This isn't a style choice -- an earlier version
	// of this file DID factor it into that wire (reused across the
	// read plus all four conditional per-byte-lane writes in the same
	// clocked block, ordinary and textbook-correct Verilog), and it
	// was confirmed via direct A/B simulation testing (Icarus Verilog
	// 12.0) to reproducibly alias: reading word N back, or writing to
	// it, would land on word (N-1) instead -- while an otherwise
	// identical block indexing the array with the SAME expression
	// written inline at each use (no intermediate wire at all) did
	// not, byte-for-byte, in the exact same test. This exactly
	// matches how vram_wb (rtl/mem/vram.v) already indexes its own
	// `vram[wb_adr_i]` -- directly off the raw port signal, no
	// intermediate wire -- which was independently verified clean
	// under the identical test. See docs/gpu_blitter.md, "Bugs found
	// (and fixed)" #4 for the full writeup, including why this is
	// believed to be an Icarus-Verilog-specific code-generation quirk
	// rather than a real hardware/synthesis defect (nothing in this
	// codebase currently performs a Wishbone READ of glyph memory at
	// all -- sw/common/zgfx.c's z_gfx_hw_font_load()/
	// z_gfx_hw_icon_load() both only ever WRITE it -- so this was
	// dormant regardless), and why it's fixed here anyway rather than
	// left as a landmine for the next thing that reads this port back,
	// or for anyone writing a future testbench against this module
	// that might otherwise get fooled by it. This specific shape
	// (wire-typed intermediate memory-array index, reused across a
	// read plus multiple conditional writes in one block) is now a
	// known Icarus hazard in this project worth checking for
	// elsewhere -- gpu_raster.v's own fifo_mem was checked and does
	// NOT share it (fifo_wr_ptr/fifo_rd_ptr are plain registered
	// counters, not wire-derived part-selects).

	// port A read/write
	always @(posedge clk) begin

		wb_ack_o <= 1'b0;

		if (wb_cyc_i && wb_stb_i && !wb_ack_o) begin

			wb_ack_o <= 1'b1;
			wb_dat_o <= mem[wb_adr_i[ADDR_WIDTH-3:0]];

			if (wb_we_i) begin
				if (wb_sel_i[0]) mem[wb_adr_i[ADDR_WIDTH-3:0]][7:0]   <= wb_dat_i[7:0];
				if (wb_sel_i[1]) mem[wb_adr_i[ADDR_WIDTH-3:0]][15:8]  <= wb_dat_i[15:8];
				if (wb_sel_i[2]) mem[wb_adr_i[ADDR_WIDTH-3:0]][23:16] <= wb_dat_i[23:16];
				if (wb_sel_i[3]) mem[wb_adr_i[ADDR_WIDTH-3:0]][31:24] <= wb_dat_i[31:24];
			end

		end

	end

	// -- port B: blitter read -- byte-addressed (blit_addr is a byte
	// address, matching gpu_blit_wb's own glyph_addr_o), so pull the
	// right byte lane out of the word-oriented storage above. A
	// simple unconditional single-array-read each cycle -- also
	// indexed inline (same reasoning as port A above), and was never
	// vulnerable to the aliasing bug in the first place even before
	// this (no writes anywhere in this block), so this port's own
	// behavior/timing is otherwise unchanged.
	always @(posedge clk) begin
		blit_data <= mem[blit_addr[ADDR_WIDTH-1:2]][(blit_addr[1:0] * 8) +: 8];
	end

endmodule
