# Machdyne Obst -- ECP5 12F, 1MB SRAM, VGA video, USB HID, microSD.
#
# The defines below are the BOARD_OBST block of rtl/boards.vh,
# verbatim. They are duplicated here rather than read out of that file
# because a target has to be able to REMOVE one -- see
# targets/obst_uart_gpio.spec, which drops `SPI_ETH to free PMOD B --
# which an additive -D on the yosys command line cannot express.
#
# `zrelease check` diffs this list against rtl/boards.vh's own
# BOARD_OBST block and complains if they have drifted.
#
# NOT listed here: `RTC, `TRNG, `GAME, `CPU_MUL, `CPU_MUL_FAST,
# `CPU_DIV, `ARBITER. Those are the universal section of rtl/boards.vh,
# they stay outside the ZSPEC guard, and boards.vh explains for each
# one why it is not a per-board choice.
#
# -- What is different about this board --
#
# It is an ECP5 12F with 1MB of SRAM, and both of those matter here in
# a way they do not on Lakritz. The 12F is a quarter of the fabric and
# the memory is a thirty-second of the RAM, so anything optional is a
# real decision rather than a rounding error -- which is exactly why
# the GPIO target below removes the ethernet rather than adding to it.

description = Machdyne Obst (ECP5-12F)
board       = obst
family      = ecp5
lpf         = obst_v0.lpf

# PMOD ports: which FPGA ball each PMOD pin lands on.
#
# From the board's own obst_v0.lpf (github.com/machdyne/obst), which
# names the balls as PMOD_A1..PMOD_B10 rather than by function. Note
# that boards/obst_v0.lpf in THIS tree only ever recorded pins 1-4 of
# port A, because the USB-UART PMOD is all that had ever been plugged
# into it; pins 7-10 are real and are what a GPIO port uses.
#
# A target that plugs something into a port takes over EVERY ball in
# it -- the generator drops any base-.lpf constraint landing on one,
# on the grounds that whatever the board put there assumed a different
# PMOD. That is what resolves ETH_SS/MOSI/MISO/SCLK/INT and the DBG
# LED bar when GPIO wants port B.
pmod.a =
	1=E5   2=D4   3=B5   4=C6
	7=E4   8=C4   9=B6   10=D6

pmod.b =
	1=C8   2=B9   3=A10  4=C10
	7=B8   8=A9   9=B10  10=D10

# Matches the Makefile's own FLASH line for this board (PROG/FLASH use
# $(CABLE), which defaults to dirtyJtag).
#
# Obst also ships a DFU bootloader and can be programmed over USB-C
# with `dfu-util -a 0 -D <file>` -- see the board's README. That is
# often the easier route since it needs no JTAG cable, but it is not
# what goes in the release notes, because the release image is a full
# flash image written at an offset and the JTAG path is the one the
# rest of this tree uses.
flash_cmd = openFPGALoader -v -c dirtyJtag -f -o 0 {file}

# No `net` by default. This board has 1MB of main memory and the
# ethernet is a PMOD that is not always plugged in; the GPIO target
# below removes `SPI_ETH entirely, and an app that starts, reads the
# feature CSR and immediately gives up is still an app that was
# fetched from flash and given a pid. Targets that keep the NIC can
# add it back.
core_apps = wm repl term

defines =
    FPGA_ECP5
    OSC48
    MEM=1
    MEM_SRAM
    MEM_VRAM
    MEM_ROM
    MEM_GLYPH
    LED_RGB
    GPU
    GPU_RASTER
    GPU_BLIT
    GPU_CURSOR
    GPU_VGA
    UART0
    USB_HID
    SPI_SDCARD
    SPI_ETH
    AUDIO
    AUDIO_SD
    AUDIO_MIXER
