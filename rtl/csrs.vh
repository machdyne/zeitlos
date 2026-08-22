// rtl/boards.vh's per-board `MEM (total main RAM, megabytes) is
// optional -- defaulted here to 1 (matching the original hardcoded
// assumption every board effectively had before `MEM/rtl/csrs.v
// existed) if a board block doesn't set it, rather than leaving it
// undefined and breaking the build. See docs/csrs.md.
`ifndef MEM
`define MEM 1
`endif

// feature bits exposed via rtl/csrs.v's FEATURES register -- KEEP IN
// SYNC with sw/common/zsoc.h's Z_FEATURE_* constants (bit position is
// the only thing that has to match; see csrs.v's own header comment
// for why there's no single shared source for both sides). Each term
// mirrors one of rtl/boards.vh's own `ifdef flags directly -- this is
// deliberately just a bit-for-bit exposure of "was this `ifdef
// active in this build", nothing computed or inferred beyond that.
localparam CSR_FEATURES =
`ifdef MEM_SRAM
	(32'h1 << 0) |
`endif
`ifdef MEM_SDRAM
	(32'h1 << 1) |
`endif
`ifdef MEM_VRAM
	(32'h1 << 2) |
`endif
`ifdef MEM_QQSPI
	(32'h1 << 3) |
`endif
`ifdef MEM_ROM
	(32'h1 << 4) |
`endif
`ifdef MEM_GLYPH
	(32'h1 << 5) |
`endif
`ifdef GPU
	(32'h1 << 6) |
`endif
`ifdef GPU_RASTER
	(32'h1 << 7) |
`endif
`ifdef GPU_BLIT
	(32'h1 << 8) |
`endif
`ifdef GPU_CURSOR
	(32'h1 << 9) |
`endif
`ifdef GPU_VGA
	(32'h1 << 10) |
`endif
`ifdef GPU_DDMI
	(32'h1 << 11) |
`endif
`ifdef UART0
	(32'h1 << 12) |
`endif
`ifdef USB_HID
	(32'h1 << 13) |
`endif
`ifdef SPI_SDCARD
	(32'h1 << 14) |
`endif
`ifdef SPI_ETH
	(32'h1 << 15) |
`endif
`ifdef SPI_FLASH
	(32'h1 << 16) |
`endif
`ifdef ETH_RMII
	(32'h1 << 17) |
`endif
`ifdef LED_RGB
	(32'h1 << 18) |
`endif
`ifdef LED_DEBUG
	(32'h1 << 19) |
`endif
	32'h0;
