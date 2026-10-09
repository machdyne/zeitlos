# Machdyne Klinge -- ECP5 25F, 512MB DDR3L, two RMII ethernet ports, two
# microSD slots, USB-C. Headless: no video, no USB host, no audio.
#
# The defines below are the BOARD_KLINGE block of rtl/boards.vh,
# verbatim; `zrelease check` diffs the two. The universal section of
# rtl/boards.vh is not listed.
#
# A DDR3 board (the Makefile's DDR3_BOARDS): DDR3 sources and the
# minimal training BIOS. No PMOD ports.

description = Machdyne Klinge (ECP5-25F)
board       = klinge
family      = ecp5
lpf         = klinge_v1.lpf

# The Makefile defaults DEVICE to 25k, which is what Klinge ships with;
# say so here so a release image never depends on that default.
make_vars = DEVICE=25k

# Matches the Makefile's FLASH line (PROG/FLASH use $(CABLE), default
# dirtyJtag; the board README uses a USB Blaster).
flash_cmd = openFPGALoader -v -c dirtyJtag -f -o 0 {file}

# The 256KB DFU bootloader's user partition (docs/dfu_upgrade.md).
dfu_base = 0x040000

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
    MONTMUL_REGS
    SHA256
    ICACHE_KB=4
    ICACHE_LINE_WORDS=4
    DCACHE
    DCACHE_KB=4
    DCACHE_LINE_WORDS=4
    DCACHE_WBUF=2
    GPU_RASTER
    GPU_BLIT
    UART0
    USB_CDC
    SPI_SDCARD
    ETH_RMII
    ETH_RMII_DRIVE_REFCLK
    ETH_RX_SLOTS=4
