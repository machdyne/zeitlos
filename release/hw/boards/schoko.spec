# Machdyne Schoko -- ECP5 45F, 32MB SDRAM, DDMI and VGA video, one USB
# host port, microSD, two PMOD ports.
#
# The defines below are the BOARD_SCHOKO block of rtl/boards.vh,
# verbatim; `zrelease check` diffs the two. The universal section of
# rtl/boards.vh is not listed (see konfekt.spec).
#
# The block describes the board WITH a Langkatze in PMOD A and GPIO on
# PMOD B, which is what boards/schoko_v1.lpf constrains and what
# targets/schoko_langkatze_gpio.spec plugs in. The console is USB
# CDC-ACM on the USB-C socket, so neither PMOD is needed for it.

description = Machdyne Schoko (ECP5-45F)
board       = schoko
family      = ecp5
lpf         = schoko_v1.lpf

# PMOD ports, from the schematic (pcb/schoko_v1.pdf: PMOD1 is A, PMOD2
# is B). The board's own schoko_v1.lpf lists PMOD_B3 twice; the second
# is pin 4, B10.
pmod.a =
	1=A2   2=A3   3=A4   4=A5
	7=B3   8=B4   9=B5   10=A6

pmod.b =
	1=A12  2=A11  3=B11  4=B10
	7=A9   8=A10  9=B8   10=B9

# Matches the Makefile's FLASH line for this board (PROG/FLASH use
# $(CABLE), which defaults to dirtyJtag; the board README uses a USB
# Blaster -- `make CABLE=usb-blaster ...`).
flash_cmd = openFPGALoader -v -c dirtyJtag -f -o 0 {file}

# The 256KB DFU bootloader's user partition (docs/dfu_upgrade.md).
# Boards that still carry the original 1MB bootloader need it updated
# first; the -dfu.bin will not fit behind the old one.
dfu_base = 0x040000

defines =
    FPGA_ECP5
    PROGRAMN_PIN
    OSC48
    MEM=32
    MEM_SDRAM
    MEM_VRAM
    MEM_ROM
    MEM_GLYPH
    LED_RGB
    ICACHE
    MONTMUL
    MONTMUL_REGS
    SHA256
    ICACHE_KB=8
    ICACHE_LINE_WORDS=4
    DCACHE
    DCACHE_KB=4
    DCACHE_LINE_WORDS=4
    DCACHE_WBUF=2
    SDRAM_BURST
    GPU
    GPU_RASTER
    GPU_BLIT
    GPU_CURSOR
    GPU_VGA
    GPU_DDMI
    COLOR
    UART0
    USB_CDC
    USB_HOST
    SPI_SDCARD
    SPI_ETH
    GPIO_PORT0
