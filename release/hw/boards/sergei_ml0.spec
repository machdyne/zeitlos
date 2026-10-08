# Sergei ML0 -- a complete system, not a board with PMOD sockets.
# ECP5 25F (the Sechzig ML0 module: the ML1 on the smaller die), 32MB SDRAM, DDMI video, RMII ethernet (FPGA drives REFCLK),
# S/PDIF optical audio out.
#
# Mirrors the BOARD_SERGEI_ML0 block of rtl/boards.vh. See
# boards/lakritz.spec for why this is duplicated rather than read.
#
# AUDIO_RATE_RESET=16 is not optional here and is not a preference:
# S/PDIF is 128 half-cells per frame, so from a 48MHz sys_clk only
# fs = 375000/N is reachable, and 16 is the divider whose half-cell is
# an exact whole number of clocks (8). rtl/boards.vh's `AUDIO_SPDIF
# note has the derivation. Any other value here is jitter.

description = Sergei ML0
board       = sergei_ml0
family      = ecp5
# The ML1 carrier pin file: the ML0 module has the ML1's ball map.
lpf         = sergei_ml1.lpf

flash_cmd = openFPGALoader -c dirtyJtag -f -o 0 {file}

# repl is not a core app -- it and posix ship on the card image
# (release/lib/mkfatimg.py) and init starts them from there.
core_apps = wm net term console cron

# PMOD port. SIX pins, four of them signals -- so a GPIO port here is
# half width (release/hw/pmods/gpio4.spec).
#
# Pin 1 is A13, which the base .lpf also uses for the optical S/PDIF
# output. A target that plugs something into this port takes over
# every ball in it, so the generator drops that constraint
# automatically -- but `AUDIO_SPDIF still has to be removed for the
# PORT to go away too. See targets/sergei_ml0.spec, which does
# exactly that.
pmod.a =
	1=A13  2=R12  3=T13  4=T14

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
    ETH_RMII_DRIVE_REFCLK
    ETH_RX_SLOTS=4
    AUDIO
    AUDIO_SPDIF
    AUDIO_MIXER
    AUDIO_RATE_RESET=8'd16
