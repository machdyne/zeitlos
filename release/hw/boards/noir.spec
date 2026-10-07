# Machdyne Noir -- ECP5 45F, 256MB DDR3L, DDMI video, one USB host port,
# microSD, 3.5mm audio. No PMOD connector, no ethernet.
#
# The defines below are the BOARD_NOIR block of rtl/boards.vh,
# verbatim; `zrelease check` diffs the two. The universal section of
# rtl/boards.vh is not listed.
#
# A DDR3 board: the Makefile's DDR3_BOARDS includes noir, so the build
# adds the DDR3 sources and the BIOS is the minimal training one
# (docs/ddr3.md). No PMOD ports: one configuration, one target.

description = Machdyne Noir (ECP5-45F)
board       = noir
family      = ecp5
lpf         = noir_v0.lpf

# Matches the Makefile's FLASH line for this board (PROG/FLASH use
# $(CABLE), which defaults to dirtyJtag; the board README uses a USB
# Blaster -- `make CABLE=usb-blaster ...`).
flash_cmd = openFPGALoader -v -c dirtyJtag -f -o 0 {file}

# The 256KB DFU bootloader's user partition (docs/dfu_upgrade.md).
dfu_base = 0x040000

# net stays: there is no PHY, but the full-speed USB host drives USB
# ethernet adapters (docs/usb_ethernet.md), and net finds the NIC at
# startup from the feature CSR.
core_apps = wm net term console cron

defines =
    FPGA_ECP5
    PROGRAMN_PIN
    OSC48
    MEM=256
    MEM_DDR3
    DDR3_ROW_BITS=14
    DDR3_TRFC_NS=160
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
    GPU
    GPU_RASTER
    GPU_BLIT
    GPU_CURSOR
    GPU_DDMI
    COLOR
    UART0
    USB_CDC
    USB_HOST
    SPI_SDCARD
    AUDIO
    AUDIO_SD
    AUDIO_MIXER
