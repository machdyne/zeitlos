# ULX3S 85K -- ECP5 85F, 32MB SDRAM, DDMI video, USB HID, microSD,
# S/PDIF audio, and networking over the on-board ESP32.
#
# Mirrors the BOARD_ULX3S block of rtl/boards.vh. See
# boards/lakritz.spec for why this is duplicated rather than read, and
# `zrelease check` for the diff that keeps the two honest.
#
# OSC25, not OSC48: the ULX3S has a 25MHz crystal and rtl/pll0_25.v
# multiplies it to the same 48MHz sys_clk every other board runs at.
# Everything downstream is therefore identical -- including the S/PDIF
# divider below, which is why AUDIO_RATE_RESET is the same 16 as
# sergei_ml1's.
#
# ESP32_LINK is the third NIC in the tree. sw/apps/net links all three
# drivers and picks one at runtime from the feature CSR, so nothing
# here or in the target spec selects it.

description = ULX3S
board       = ulx3s
family      = ecp5
lpf         = ulx3s.lpf

# PMOD port j1: header J1, pins 1-12.
#
# Pins 1-4 are power and ground on both rows, so a 12-pin PMOD
# plugged into that end of the header is powered. The signals are
# GP0-GP3 (pins 6, 8, 10, 12) and GN0-GN3 (pins 5, 7, 9, 11). The
# map below puts PMOD pins 1-4 on the GP row.
#
# This end of the header, and not one that includes GP10. GP10
# (C4, J1 pin 30) is WIFI_GPIO27, wired to the onboard ESP32, and
# GP11-GP13 / GN11-GN13 are the ESP32's other GPIO. Pins 1-12 of
# J2 are GP14-GP17 / GN14-GN17, the analog inputs. GP26/GN26,
# which this build uses as USB HID port 1, are J2 pins 34 and 33.
# None of the eight balls below is constrained in boards/ulx3s.lpf.
pmod.j1 =
	1=B9   2=A9   3=A10  4=B11
	7=C10  8=B10  9=A11  10=C11

# NO DEVICE HERE -- each variant is its own target, because one PCB
# and one .lpf cover 12F/25F/45F/85F and the only difference in the
# build is nextpnr's --<device>. See release/targets/ulx3s_*.spec.
#
# The Makefile defaults to `DEVICE ?= 25k`, so a target that forgets
# to set it silently builds a 25F bitstream. That is why the targets
# set it explicitly rather than relying on the default even for 25F.

# Matches the Makefile's own FLASH line for this board.
flash_cmd = openFPGALoader -v -b ulx3s -f -o 0 {file}

# repl is not a core app -- it and posix ship on the card image
# (release/lib/mkfatimg.py) and init starts them from there.
core_apps = wm net term console cron

defines =
	FPGA_ECP5
	OSC25
	MEM=32
	MEM_SDRAM
	MEM_VRAM
	MEM_ROM
	MEM_GLYPH
	MONTMUL
	MONTMUL_REGS
	SHA256
	ICACHE
	ICACHE_KB=4
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
	GPU_DDMI
	UART0
	UART1
	USB_HID
	USB_HID_SENS_SHIFT=3
	SPI_SDCARD
	ESP32_LINK
	ESP32_RXFIFO_BITS=13
	AUDIO
	AUDIO_SPDIF
	AUDIO_MIXER
	AUDIO_RATE_RESET=8'd16
