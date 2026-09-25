# Mozart ML2 -- a complete system, not a board with PMOD sockets.
# ECP5 45F, 512MB DDR3L (MT41K256M16TW-107), DDMI video, RMII ethernet,
# PT8211 audio DAC. ML1 with DDR3 in place of SDRAM; see docs/ddr3.md.
#
# Mirrors the BOARD_MOZART_ML2 block of rtl/boards.vh. See
# boards/lakritz.spec for why this is duplicated rather than read.
#
# No SDRAM_BURST, unlike ML1: that selects burst line fills for the SDRAM
# controller, and DDR3's controller answers a line fill from its own
# block register instead.
#
# DDR3 boards build the minimal BIOS (logo, DDR3 training, load, verify,
# boot -- no monitor). That is chosen by board name in sw/bios, and a
# release build passes BOARD= from `board` below, so it gets the same
# BIOS as `make BOARD=mozart_ml2`.
#
# There is no PMOD layer for this target and there should not be one:
# the hardware is fixed, so targets/mozart_ml2.spec adds nothing.

description = Mozart ML2
board       = mozart_ml2
family      = ecp5
lpf         = mozart_ml2.lpf

flash_cmd = openFPGALoader -c dirtyJtag -f -o 0 {file}

# repl is not a core app -- it and posix ship on the card image
# (release/lib/mkfatimg.py) and init starts them from there.
core_apps = wm net term console cron

defines =
    FPGA_ECP5
    PROGRAMN_PIN
    OSC48
    MEM=512
    MEM_DDR3
    MAIN_512MB
    DDR3_ROW_BITS=15
    DDR3_TRFC_NS=260
    MEM_VRAM
    MEM_ROM
    MEM_GLYPH
    ICACHE
    MONTMUL
    ICACHE_KB=8
    ICACHE_LINE_WORDS=4
    DCACHE
    DCACHE_KB=4
    DCACHE_LINE_WORDS=4
    DCACHE_WBUF=2
    GPU
    GPU_RASTER
    GPU_BLIT
    GPU_CURSOR
    GPU_DDMI
    UART0
    USB_HOST
    SPI_SDCARD
    ETH_RMII
    ETH_RX_SLOTS=4
    AUDIO
    AUDIO_PT8211
    AUDIO_MIXER
