# Sergei ML2 -- a complete system, not a board with PMOD sockets.
# ECP5 45F, 512MB DDR3L (MT41K256M16TW-107), DDMI video, RMII ethernet
# (FPGA drives REFCLK), S/PDIF optical audio out. Sergei's carrier with
# the Sechzig ML2 module, whose memory choices are Mozart ML2's; see
# docs/ddr3.md.
#
# Mirrors the BOARD_SERGEI_ML2 block of rtl/boards.vh. See
# boards/lakritz.spec for why this is duplicated rather than read.
#
# No SDRAM_BURST, unlike Sergei ML1: that selects burst line fills for
# the SDRAM controller, and DDR3's answers a line fill from its own
# block register. DDR3 boards build the minimal BIOS, chosen by board
# name, and a release build passes BOARD= from `board` below, so it
# gets the same BIOS as `make BOARD=sergei_ml2`.
#
# AUDIO_RATE_RESET=16 for the same reason as Sergei ML1: see that spec.

description = Sergei ML2
board       = sergei_ml2
family      = ecp5
lpf         = sergei_ml2.lpf

flash_cmd = openFPGALoader -c dirtyJtag -f -o 0 {file}

# repl is not a core app -- it and posix ship on the card image
# (release/lib/mkfatimg.py) and init starts them from there.
core_apps = wm net term console cron

# PMOD port, as on Sergei ML1 -- six pins, four signals, pin 1 shared
# with the optical output -- on the ML2 module's balls. Pins 2-4 are
# DEDUCED from Mozart ML2's audio balls, not measured: see the ball map
# in boards/sergei_ml2.lpf.
pmod.a =
	1=A13  2=E10  3=E9  4=D9

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
    ETH_RMII_DRIVE_REFCLK
    ETH_RX_SLOTS=4
    AUDIO
    AUDIO_SPDIF
    AUDIO_MIXER
    AUDIO_RATE_RESET=8'd16
