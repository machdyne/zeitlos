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
// CPU extensions. Unlike every bit above these describe the CPU core
// rather than a peripheral, and they exist for a specific reason:
// software compiled for rv32im running on a bitstream WITHOUT M does
// not degrade, it dies -- every mul/div is an illegal instruction.
// Exposing these lets the BIOS say so in one clear line at boot
// instead of leaving a mystery hang. See sw/common/zsoc.h's
// z_soc_check_cpu_arch().
`ifdef CPU_MUL
	(32'h1 << 20) |
`endif
`ifdef CPU_DIV
	(32'h1 << 21) |
`endif
`ifdef CPU_MUL_FAST
	(32'h1 << 22) |
`endif
// WHICH core, not just what it can do. Set when rtl/boards.vh selects
// rtl/cpu/zeitlos32 instead of picorv32; clear means picorv32, which
// is also what every bitstream built before zeitlos32 existed reports,
// so "clear" and "old build" agree rather than conflicting.
//
// This is worth a bit of its own because the two cores are drop-in
// compatible by design: the same kernel binary runs on either, so
// there is otherwise NOTHING in a running system that says which one
// you are on. Chasing a bug for an afternoon on the wrong assumption
// about that is a specific and avoidable waste.
`ifdef CPU_ZEITLOS32
	(32'h1 << 23) |
`endif
// rtl/rtc.v -- the wall clock. Mirrors `RTC in rtl/boards.vh like
// every bit above mirrors its own define; boards.vh defines it at the
// universal level, so this is set on every board by default and clear
// only where somebody deliberately commented it out.
//
// This bit is what software checks BEFORE touching the RTC's own
// registers, and the order is load-bearing rather than stylistic.
// Within a build the two answers agree -- no RTC means csrs.v absorbs
// its window and the MAGIC read comes back 0 either way. Across
// builds they do not: on a bitstream predating rtl/rtc.v entirely,
// 0x7000_03xx is decoded by nothing on an `ICACHE board, and an
// undecoded address on this bus never acks, so the CPU HANGS on that
// read (see rtl/cache.v's own note on the same hazard). This bit sits
// at 0x7000_0008, which every bitstream ever built decodes, so asking
// here is always safe. See z_rtc_available() in sw/common/zrtc.h.
`ifdef RTC
	(32'h1 << 24) |
`endif
// ULX3S ESP32 link (rtl/esp32_rxfifo.v, UART1, esp32 control) --
// moved from bit 20 when the CPU/RTC bits above took 20-24.
`ifdef ESP32_LINK
	(32'h1 << 25) |
`endif
	32'h0;
