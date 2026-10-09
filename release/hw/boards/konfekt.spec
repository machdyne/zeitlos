# Machdyne Konfekt -- ECP5 12F, 32MB SDRAM, DDMI video, one USB host
# port, microSD, 3.5mm audio. No PMOD connector.
#
# The defines below are the BOARD_KONFEKT block of rtl/boards.vh,
# verbatim; `zrelease check` diffs the two.
#
# NOT listed here: the universal section of rtl/boards.vh (`RTC,
# `TRNG, `GAME, `MPU, `VMOUSE, `CPU_MUL, `CPU_MUL_FAST, `CPU_DIV,
# `ARBITER), which is not a per-board choice.
#
# No PMOD ports: there is nothing to plug in, so there is exactly one
# Konfekt configuration and targets/konfekt.spec is three lines.

description = Machdyne Konfekt (ECP5-12F)
board       = konfekt
family      = ecp5
lpf         = konfekt_v0.lpf

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
    UART0
    USB_CDC
    USB_HID
    SPI_SDCARD
    AUDIO
    AUDIO_SD
