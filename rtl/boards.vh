// UNIVERSAL CONFIG
// ----------------

`define DEBUG
`define ARBITER

// RTC: the wall clock (rtl/rtc.v) -- seconds since the Unix epoch plus
// a 1/1024s fraction, set over the network by sw/apps/net's SNTP
// client and read by sw/apps/clock. See docs/rtc.md.
//
// Universal rather than per-board because there is no board-specific
// reason to want it or not want it: it needs no pins, no external
// part and no board support of any kind, just a prescaler and a
// counter clocked from sys_clk. Every board can have one, so every
// board does by default.
//
// Comment it out to reclaim the logic on a board that is genuinely
// tight -- roughly a 32-bit counter, a 24-bit prescaler and a small
// register file. Software copes: rtl/csrs.v's own FEATURE bit (see
// rtl/csrs.vh) goes clear, z_rtc_available() (sw/common/zrtc.h)
// answers false, net skips its NTP client entirely and sw/apps/clock
// says on screen that this bitstream has no clock. Nothing hangs and
// nothing has to be rebuilt differently -- unlike `CPU_MUL above,
// this is not a switch the software half has to agree with.
//
// NOT the same thing as rtl/sysctl.v's `rtc_ctr`, despite the name
// they share. That is the ~732Hz KTIMER divider, it counts ticks
// since boot, it is unconditional, and it is unaffected by this
// define. Only one of the two knows what a date is.
`define RTC

// RV32IM: hardware multiply and divide (rtl/cpu/picorv32/picorv32.v's
// ENABLE_FAST_MUL/ENABLE_MUL/ENABLE_DIV). Universal rather than
// per-board because the alternative -- some boards with M, some
// without -- means the software has to be built differently per board
// too, and a bitstream/binary mismatch here is not a graceful failure:
// every `mul` becomes an illegal instruction. See docs/muldiv.md.
//
// `CPU_MUL_FAST uses the DSP-backed multiplier (2 cycles). It is the
// one to watch in the nextpnr timing report -- picorv32 instantiates
// picorv32_pcpi_fast_mul with EXTRA_MUL_FFS=0, i.e. a full unpipelined
// 32x32 multiply, which is the most likely thing in this design to
// limit Fmax. If timing gets tight, comment it out and leave `CPU_MUL
// defined: that selects the sequential shift-add multiplier instead
// (~32 cycles, still roughly an order of magnitude faster than the
// libgcc software routine it replaces) with no timing risk at all.
`define CPU_MUL
`define CPU_MUL_FAST
`define CPU_DIV

// CPU core selection. Undefined (the default) means picorv32, exactly
// as before. Defining this selects rtl/cpu/zeitlos32 instead -- an
// experimental in-house RV32IM core that implements the same
// interrupt ABI, so sw/bios/boot_picorv32.S and sw/bios/custom_ops.S
// are unchanged either way and no software needs rebuilding to switch.
//
// This is a ONE LINE switch on purpose: zeitlos32 is developed
// alongside everything else rather than as a branch, and being able
// to A/B the two cores against an otherwise identical bitstream is
// what makes a mystery bug tractable ("is it my scheduler or my
// core?" is an expensive question to keep asking).
//
// `CPU_MUL / `CPU_MUL_FAST / `CPU_DIV above apply to both cores.
// Note `CPU_MUL_FAST on GateMate: rtl/../Makefile passes -nomult to
// synth_gatemate, so the DSP multiplier lands in LUTs there. See
// docs/zeitlos32.md.
//
//`define CPU_ZEITLOS32

// BOARD CONFIG
// ------------
//
// `MEM is total main RAM in megabytes -- read by rtl/csrs.v (see
// docs/csrs.md) into a runtime-readable register, so software
// (sw/bios/bios.c, sw/os/mem.c) can size itself off the real number
// instead of a hardcoded assumption that only ever matched Obst (the
// first board this ran on). If a board block below doesn't set it,
// rtl/sysctl.v defaults it to 1 (matching that original hardcoded
// assumption) rather than leaving it undefined -- see that file's own
// `ifndef MEM guard.

`ifdef BOARD_OBST

`define FPGA_ECP5
`define OSC48
`define MEM 1				// note that some Obst boards have 2MB SRAM
`define MEM_SRAM
`define MEM_VRAM
//`define MEM_QQSPI
`define MEM_ROM
`define MEM_GLYPH
`define LED_RGB
//`define LED_DEBUG
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_VGA
`define UART0
`define USB_HID
`define SPI_SDCARD
`define SPI_ETH

`elsif BOARD_LAKRITZ

`define FPGA_ECP5
`define OSC48
`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
`define USB_HID
`define SPI_SDCARD
`define ICACHE
`define ICACHE_KB 4
`define ICACHE_LINE_WORDS 4

`elsif BOARD_MOZART_ML1

`define FPGA_ECP5
`define OSC48
`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
`define USB_HID
`define SPI_SDCARD
`define ETH_RMII

`elsif BOARD_LEBKUCHEN

`define FPGA_GATEMATE
`define OSC48
`define MEM 8
//`define MEM_QQSPI
//`define MEM_QQSPI_SINGLE
`define MEM_VRAM
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_VGA
`define UART0
`define USB_HID
`define SPI_SDCARD
`define SPI_FLASH

`elsif BOARD_KOLSCH

`define FPGA_GATEMATE
`define OSC48
`define MEM 64
`define MEM_SDRAM
`define MEM_VRAM
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_VGA
`define UART0
`define USB_HID
//`define SPI_SDCARD
//`define SPI_FLASH

`elsif BOARD_ULX3S

`define FPGA_ECP5
`define OSC25
`define MEM 32
`define MEM_SDRAM
`define MEM_VRAM
`define MEM_ROM
`define MEM_GLYPH
`define GPU
`define GPU_RASTER
`define GPU_BLIT
`define GPU_CURSOR
`define GPU_DDMI
`define UART0
`define UART1
`define USB_HID
`define SPI_SDCARD
`define ESP32_LINK

`endif
