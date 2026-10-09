# Machdyne Minze -- ECP5 12F, 32MB SDRAM, VGA, one USB host port,
# microSD, one PMOD port, USB-C.
#
# The defines below are the BOARD_MINZE block of rtl/boards.vh,
# verbatim; `zrelease check` diffs the two. The universal section of
# rtl/boards.vh is not listed.
#
# The block describes the board with GPIO on its PMOD, which is what
# boards/minze_v1.lpf constrains and targets/minze_gpio.spec plugs in.

description = Machdyne Minze (ECP5-12F)
board       = minze
family      = ecp5
lpf         = minze_v1.lpf

# From the schematic (pcb/minze_v1.pdf), matching the board repo's
# PMOD_A pins and LiteX's PMODA connector.
pmod.a =
	1=B11  2=B12  3=B13  4=B14
	7=A11  8=A12  9=A13  10=A14

# Matches the Makefile's FLASH line (PROG/FLASH use $(CABLE), default
# dirtyJtag; the board README uses a USB Blaster).
flash_cmd = openFPGALoader -v -c dirtyJtag -f -o 0 {file}

# The 256KB DFU bootloader's user partition (docs/dfu_upgrade.md).
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
    ICACHE
    MONTMUL
    MONTMUL_REGS
    SHA256
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
    GPU_VGA
    UART0
    USB_CDC
    USB_HID
    SPI_SDCARD
    GPIO_PORT0
