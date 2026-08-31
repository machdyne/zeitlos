# Mozart ML1 -- a complete system, not a board with PMOD sockets.
# ECP5 45F, 32MB SDRAM, DDMI video, RMII ethernet, PT8211 audio DAC.
#
# Mirrors the BOARD_MOZART_ML1 block of rtl/boards.vh. See
# boards/lakritz.spec for why this is duplicated rather than read.
#
# There is no PMOD layer for this target and there should not be one:
# the hardware is fixed, so there is exactly one Mozart configuration
# and targets/mozart_ml1.spec adds nothing.

description = Mozart ML1
board       = mozart_ml1
family      = ecp5
lpf         = mozart_ml1.lpf

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
    ETH_RX_SLOTS=4
    AUDIO
    AUDIO_PT8211
    AUDIO_MIXER
