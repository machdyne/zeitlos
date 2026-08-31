# Sergei ML1 -- a complete system, not a board with PMOD sockets.
# ECP5 45F, 32MB SDRAM, DDMI video, RMII ethernet (FPGA drives REFCLK),
# S/PDIF optical audio out.
#
# Mirrors the BOARD_SERGEI_ML1 block of rtl/boards.vh. See
# boards/lakritz.spec for why this is duplicated rather than read.
#
# AUDIO_RATE_RESET=16 is not optional here and is not a preference:
# S/PDIF is 128 half-cells per frame, so from a 48MHz sys_clk only
# fs = 375000/N is reachable, and 16 is the divider whose half-cell is
# an exact whole number of clocks (8). rtl/boards.vh's `AUDIO_SPDIF
# note has the derivation. Any other value here is jitter.

description = Sergei ML1
board       = sergei_ml1
family      = ecp5
lpf         = sergei_ml1.lpf

flash_cmd = openFPGALoader -c dirtyJtag -f -o 0 {file}

core_apps = wm net repl term

defines =
    FPGA_ECP5
    OSC48
    MEM=32
    MEM_SDRAM
    MEM_VRAM
    MEM_ROM
    MEM_GLYPH
    ICACHE
    ICACHE_KB=8
    ICACHE_LINE_WORDS=4
    GPU
    GPU_RASTER
    GPU_BLIT
    GPU_CURSOR
    GPU_DDMI
    UART0
    USB_HID
    SPI_SDCARD
    ETH_RMII
    ETH_RMII_DRIVE_REFCLK
    ETH_RX_SLOTS=4
    AUDIO
    AUDIO_SPDIF
    AUDIO_MIXER
    AUDIO_RATE_RESET=8'd16
