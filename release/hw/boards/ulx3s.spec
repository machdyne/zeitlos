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

# PMOD ports: the ULX3S J2 header carries the second USB HID port in
# this build, so there is no free PMOD port to declare. If one is
# wired up later, add a pmod.<port> ball map here and the existing
# PMOD specs work unchanged.

# NO DEVICE HERE -- each variant is its own target, because one PCB
# and one .lpf cover 12F/25F/45F/85F and the only difference in the
# build is nextpnr's --<device>. See release/targets/ulx3s_*.spec.
#
# The Makefile defaults to `DEVICE ?= 25k`, so a target that forgets
# to set it silently builds a 25F bitstream. That is why the targets
# set it explicitly rather than relying on the default even for 25F.

# Matches the Makefile's own FLASH line for this board.
flash_cmd = openFPGALoader -v -b ulx3s -f -o 0 {file}

core_apps = wm net repl term

defines =
	FPGA_ECP5
	OSC25
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
	UART1
	USB_HID
	USB_HID_SENS_SHIFT=3
	SPI_SDCARD
	ESP32_LINK
	AUDIO
	AUDIO_SPDIF
	AUDIO_MIXER
	AUDIO_RATE_RESET=8'd16
