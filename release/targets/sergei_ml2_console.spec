# Sergei ML2 as a games console: lakritz_console's design on the board
# that is easier to test on (docs/profiles.md).
#
# The same console as lakritz_console -- the launcher in game mode, no
# desktop; two bitplanes of VRAM in block RAM, chunky's 256 colours in
# main memory (`VRAM_SDRAM: here DDR3, read a word at a time), T2
# writing them there; no line rasterizer (zgfx draws 2D lines itself),
# no MPU, no TRNG, no RTC -- but on a 45F, so nothing else has to give,
# and with the DDR3 board's own caches and peripherals. What works here
# should work on Lakritz, bar the memory controller: SDRAM there, with
# four-word bursts.
#
# -- what stays --
#
#   ETH_RMII       the network, unlike lakritz_console: this is the board
#                  software is updated on over the wire while the
#                  console is being tested (docs/update.md)
#
# -- what goes --
#
#   -GPU_CURSOR    no desktop pointer
#   -KECCAK        room for T2 (docs/zgl_t2.md: T2 with Keccak did not
#                  route on this board)
#   -COLOR_WINDOW, -COLOR_DESKTOP   the colour window and desktop colour
#                  read the second VRAM sheet, which is not there
#
# -- what comes --
#
#   ZEITLOS_CONSOLE, GAME_PLANES=2, GPU_T2, VRAM_SDRAM, -GPU_RASTER,
#   NO_MPU, NO_RTC: as lakritz_console. The TRNG stays: this board's
#   MONTMUL hangs off it (rtl/sysctl.v), and there is room.
#   DDR3_WCOMBINE  the DDR3 controller combines stores into whole
#                  16-byte blocks (rtl/mem/ddr3_ctrl.v): T2's picture
#                  goes out at about 8 cycles a word rather than 19

description = Sergei ML2 as a games console (console profile, T2)

base = sergei_ml2

defines =
	ZEITLOS_CONSOLE
	-GPU_CURSOR
	-KECCAK
	-COLOR_WINDOW
	-COLOR_DESKTOP
	GAME_PLANES=2
	GPU_T2
	VRAM_SDRAM
	-GPU_RASTER
	NO_MPU
	NO_RTC
	DDR3_WCOMBINE
