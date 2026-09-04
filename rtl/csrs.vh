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
// A console at 0xf000_00xx that is really there -- as opposed to
// rtl/uart_null.v, which acks and discards.
//
// SET FOR `USB_CDC TOO, and that is the point rather than a
// convenience. The question this bit answers is "is there somewhere
// for my output to go", not "is there a 16550 behind it": software
// that checks it is deciding whether to tell the user there is no
// serial console (see rtl/uart_null.v's header), and on a `USB_CDC
// board there very much is one. Which KIND it is lives in FEATURES2
// bit 2 below, where something that genuinely cares -- an app
// offering to change the baud rate, say -- can find out.
//
// rtl/boards.vh `undef's `UART0 whenever `USB_CDC is set, so these
// two arms cannot both be taken and the `elsif is exact rather than
// a priority.
`ifdef UART0
	(32'h1 << 12) |
`elsif USB_CDC
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
// rtl/trng.v -- the entropy source. Mirrors `TRNG in rtl/boards.vh,
// universal there like `RTC. This bit is the ONLY safe first probe on
// an arbitrary bitstream: reading the TRNG's own MAGIC on a build that
// predates rtl/trng.v hits an address nothing decodes, which on this
// bus never acks and hangs the CPU. See sw/common/zrng.h.
`ifdef TRNG
	(32'h1 << 25) |
`endif
// rtl/audio.v -- the sample FIFO and DAC output stage. Mirrors `AUDIO
// in rtl/boards.vh, which is PER-BOARD rather than universal: audio
// needs pins and a DAC, so a board either has it or does not.
//
// Same hazard and same rule as Z_FEATURE_RTC and Z_FEATURE_TRNG above:
// check THIS bit before reading the audio block's own MAGIC. On a
// bitstream built before rtl/audio.v existed, 0x7000_05xx is decoded
// by nothing, and an undecoded address on this bus never acks -- so
// the probe read hangs the CPU rather than returning zero. This bit is
// at 0x7000_0008, which every bitstream ever built decodes, so asking
// here is always safe. z_audio_present() (sw/common/zaudio.h) is that
// check in the safe order.
//
// Note this bit says a FIFO and an output stage were built. It does
// NOT say which DAC is on the other end -- the register interface is
// identical either way. Read the block's own CONFIG register for that.
`ifdef AUDIO
	(32'h1 << 26) |
`endif
// Game mode -- the 320x240 pixel-doubled viewport over the same
// 640x480 framebuffer (rtl/gpu/gpu_video.v, controlled through
// rtl/socctl.v's GAME/VIEW registers). Mirrors `GAME in rtl/boards.vh,
// which defines it at the universal level like `RTC and `TRNG: it
// needs no pins and no external part, only a handful of LUTs in the
// scanout path, so every board with a GPU can have it and does.
//
// Note this bit says the BITSTREAM was built with game mode, not that
// the machine is currently in it -- that is socctl's GAME register,
// and it reads back 0 at boot on every board. Nor does this bit alone
// mean the mode is usable: a board with `GAME but no `GPU has nothing
// to scan out with, so rtl/sysctl.v ands the two together before
// handing socctl its GAME_AVAIL parameter, and software should prefer
// socctl's own `avail` bit over this one for the "can I actually do
// this" question.
//
// Unlike Z_FEATURE_RTC/_TRNG/_AUDIO above there is no hang hazard
// here and no ordering rule to follow: game mode has no address
// window of its own. It lives inside socctl, which every bitstream
// that has socctl at all already decodes and acks. The worst an old
// bitstream does is return 0 from a register it does not know, which
// GAME's own signature (see rtl/socctl.v) catches cleanly.
`ifdef GAME
	(32'h1 << 27) |
`endif
// Composite video out (rtl/gpu/gpu_video.v's `GPU_COMPOSITE). Mirrors
// the define like every bit above mirrors its own.
//
// Worth exposing separately from GPU_VGA/GPU_DDMI rather than folding
// into them, because software genuinely behaves differently here: a
// composite board is 320x240 and cannot be anything else, so an app
// that would otherwise ask "am I in game mode" should ask this too
// before assuming a 640x480 surface is visible. The desktop is still
// 640x480 -- it is just that only a quarter of it is on screen at a
// time, permanently.
`ifdef GPU_COMPOSITE
	(32'h1 << 28) |
`endif
// PAL rather than NTSC, when GPU_COMPOSITE is set. Meaningless on its
// own; check bit 28 first. The difference software can see is the
// frame rate -- 50Hz rather than 60 -- which a game pacing itself off
// z_game_wait_frame() may want to know about.
`ifdef GPU_COMPOSITE_PAL
	(32'h1 << 29) |
`endif
// ULX3S ESP32 network link (rtl/esp32_rxfifo.v + UART1 + the ESP32
// control register). Same rule as the blocks above: net checks this
// before touching any of those registers, and the highest free bit is
// used deliberately -- this is a one-board peripheral, so it stays out
// of the way of anything universal that comes later.
`ifdef ESP32_LINK
	(32'h1 << 30) |
`endif
	32'h0;

// -- FEATURES2 (rtl/csrs.v word 3) --
//
// The continuation of CSR_FEATURES above, which is full: bits 0
// through 30 are assigned and bit 31 is the last one left. GPIO and a
// general-purpose UART1 arrived together and needed two bits, so the
// register file grew a second word rather than spending the last bit
// and facing the same question again a phase later. BIT 31 OF
// CSR_FEATURES IS STILL FREE AND SHOULD STAY THAT WAY -- it is the
// escape hatch for something that genuinely has to be reachable from
// a single 32-bit read.
//
// Only the LOW 16 BITS are carried: rtl/csrs.v puts a signature in the
// top half so software can tell "this bitstream has no GPIO" from
// "this bitstream predates the register", which reads back as the same
// zero otherwise. When these sixteen fill, FEATURES3 goes at word 4
// with the same shape -- words 4-7 are reserved for that.
//
// KEEP IN SYNC with sw/common/zsoc.h's Z_FEATURE2_* constants, exactly
// as CSR_FEATURES is kept in sync with its Z_FEATURE_* ones. Bit
// position is the only thing that has to match and nothing checks that
// it does.
localparam CSR_FEATURES2 =
// rtl/gpio.v with at least one port that has pins. NOT set merely
// because the block exists -- it always exists now, on every board, so
// a bit meaning "the block is instantiated" would be a constant 1 and
// would tell software nothing. This bit means "there is somewhere for
// a pin to go", which is the question an app actually has.
//
// Software should still prefer gpio.v's own CONFIG register for the
// port COUNT: this bit says at least one, that register says how many.
`ifdef GPIO_PORT0
	(32'h1 << 0) |
`endif
// A second 16550 (rtl/ext/uart16550) at 0xf000_0100, available to
// software as a general-purpose serial port. UART0 is the console and
// is never this.
//
// EXPLICITLY NOT SET WHEN `ESP32_LINK IS, even though that board has a
// UART1 and always has. On the ULX3S that UART is soldered to the
// on-board ESP32 and is sw/apps/net's data plane; it has no header, no
// connector and no second owner. A bit that said "this board has a
// serial port you can open" would be true of the gateware and false of
// the board, and the app that believed it would take the network down
// to say hello to nothing. `ESP32_LINK (CSR_FEATURES bit 30) is how
// software finds that UART, and it always was.
//
// So this bit means specifically: there is a second 16550 AND it is
// yours. See docs/uart1.md.
`ifdef UART1
`ifndef ESP32_LINK
	(32'h1 << 1) |
`endif
`endif
// The console at 0xf000_00xx is rtl/usb_cdc_uart.v -- a CDC-ACM USB
// device with a 16550 register map painted on the front -- rather
// than a real rtl/ext/uart16550. See docs/usb_cdc.md.
//
// CSR_FEATURES bit 12 is set either way and is the bit to check for
// "is there a console at all". This one exists for the narrower
// question, and there is exactly one kind of software that has it:
// anything that would otherwise offer to change the line settings.
// DLL/DLM/LCR are stored and read back on a `USB_CDC board and change
// nothing, so a baud-rate control there is a dial connected to
// nought -- better to grey it out than to let it lie.
//
// It also tells a user staring at `info` why their USB-UART PMOD is
// not the thing they are typing into, which is a real question on a
// board where both connectors look equally plausible.
`ifdef USB_CDC
	(32'h1 << 2) |
`endif
	32'h0;
