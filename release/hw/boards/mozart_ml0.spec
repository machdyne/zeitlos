# Mozart ML0 -- a complete system, not a board with PMOD sockets.
# ECP5 25F (the Sechzig ML0 module: the ML1 on the smaller die), 32MB SDRAM, DDMI video, RMII ethernet, PT8211 audio DAC.
#
# Mirrors the BOARD_MOZART_ML0 block of rtl/boards.vh. See
# boards/lakritz.spec for why this is duplicated rather than read.
#
# There is no PMOD layer for this target and there should not be one:
# the hardware is fixed, so there is exactly one Mozart configuration
# and targets/mozart_ml0.spec adds nothing.

description = Mozart ML0
board       = mozart_ml0
family      = ecp5
# The ML1 carrier pin file: the ML0 module has the ML1's ball map.
lpf         = mozart_ml1.lpf

flash_cmd = openFPGALoader -c dirtyJtag -f -o 0 {file}

# repl is not a core app -- it and posix ship on the card image
# (release/lib/mkfatimg.py) and init starts them from there.
core_apps = wm net term console cron

defines =
    FPGA_ECP5
    PROGRAMN_PIN
    OSC48
    MEM=32
    MEM_SDRAM
    MEM_VRAM
    MEM_ROM
    MEM_GLYPH
    ICACHE
    ICACHE_KB=4
    ICACHE_LINE_WORDS=4
    DCACHE
    DCACHE_KB=4
    DCACHE_LINE_WORDS=4
    DCACHE_WBUF=0
    SDRAM_BURST
    GPU
    GPU_RASTER
    GPU_BLIT
    GPU_CURSOR
    GPU_DDMI
    COLOR
    UART0
    USB_HOST
    SPI_SDCARD
    ETH_RMII
    ETH_RX_SLOTS=4
    AUDIO
    AUDIO_PT8211
    AUDIO_MIXER
