# Machdyne Lakritz -- ECP5 25F, 32MB SDRAM, DDMI video, USB HID, microSD.
#
# The defines below are the BOARD_LAKRITZ block of rtl/boards.vh,
# verbatim. They are duplicated here rather than read out of that file
# because a target has to be able to REMOVE one (see
# targets/lakritz_langkatze.spec), which an additive -D on the yosys
# command line cannot do.
#
# `zrelease check` diffs this list against rtl/boards.vh's own
# BOARD_LAKRITZ block and complains if they have drifted, so the
# duplication is checked rather than merely hoped about.
#
# NOT listed here: `RTC, `TRNG, `GAME, `CPU_MUL, `CPU_MUL_FAST,
# `CPU_DIV, `ARBITER. Those are the universal section of
# rtl/boards.vh, they stay outside the ZSPEC guard, and boards.vh
# explains for each one why it is not a per-board choice.

description = Machdyne Lakritz (ECP5-25F)
board       = lakritz
family      = ecp5
lpf         = lakritz_v0.lpf

# PMOD ports: which FPGA ball each PMOD pin lands on.
#
# The board owns this and a PMOD spec owns the pin->function map, so a
# PMOD is described once and works on any board that declares a port.
# Pins are the PMOD standard 1-4 and 7-10; 5, 6, 11 and 12 are ground
# and power and never appear here.
#
# A target that plugs something into a port takes over EVERY ball in
# it -- the generator drops any base-.lpf constraint landing on one, on
# the grounds that whatever the board put there assumed a different
# PMOD. That is what resolves UART0_TX/RX on B12/B13 when Langkatze
# wants those balls for MOSI and MISO.
pmod.a =
	1=B11  2=B12  3=B13  4=B14
	7=A11  8=A12  9=A13  10=A14

# Written into the release notes and README.txt, one line per target.
# {file} is substituted with the image filename.
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
    ICACHE_KB=4
    ICACHE_LINE_WORDS=4
    GPU
    GPU_RASTER
    GPU_BLIT
    GPU_CURSOR
    GPU_DDMI
    UART0
    USB_HID
    SPI_SDCARD
    AUDIO
    AUDIO_SD
    AUDIO_MIXER
