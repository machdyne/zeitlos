# Sergei ML2 as a games console: lakritz_console's design on the board
# that is easier to test on (docs/profiles.md).
#
# The same console as lakritz_console -- the launcher in game mode, no
# desktop; two bitplanes of VRAM in block RAM, chunky's 256 colours in
# main memory (`VRAM_SDRAM: here DDR3, read a word at a time), T2
# writing them there; no line rasterizer (zgfx draws 2D lines itself),
# no MPU, no TRNG, no RTC, no crypto blocks, the HID-only USB -- but on
# a 45F, and with the DDR3 board's caches. What works here
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
#   -MONTMUL, -MONTMUL_REGS, -SHA256   the crypto blocks: a console has
#                  no logins or keys to speed up, and zsha256.c and
#                  z25519.c fall back to software where they are absent
#   -USB_HOST, USB_HID   the keyboard and pads, as on lakritz_console:
#                  the HID-only USB core, not the full host
#
# -- what comes --
#
#   ZEITLOS_CONSOLE, GAME_PLANES=2, GPU_T2, VRAM_SDRAM, -GPU_RASTER,
#   NO_MPU, NO_RTC, NO_TRNG (zrng seeds in software): as lakritz_console.
#   DDR3_WCOMBINE  the DDR3 controller combines stores into whole
#                  16-byte blocks (rtl/mem/ddr3_ctrl.v): T2's picture
#                  goes out at about 8 cycles a word rather than 19
#   T2_TEX_LOG2=13 T2 with textures (docs/zgl_t2.md, "Textures"): an
#                  8 KB texture memory, perspective-correct; nine DP16KD
#                  and about 1,600 COMB more
#   CPU_ZEITLOS32P the pipelined core (docs/zeitlos32p.md) in picorv32's
#                  place: on picorv32 mausrun3d's frame is about 15
#                  cycles an instruction (docs/mausrun3d.md); zeitlos32p
#                  ran Doom at 0.65 instructions a cycle against 0.14.
#                  No MPU here (NO_MPU), so no mpu_split
#   T2_TWO_LISTS=1 T2's geometry makes the next frame's records while its
#                  raster draws this one's: a frame the longer of the two,
#                  not their sum (docs/zgl_t2.md, "Overlap")
#   T2_TWO_BUFS=1  a strip drawn while the last goes out
#   T2_ROW_END=1   a triangle's row ends past its span, not its box;
#                  the three: 16 DP16KD and 232 LUT4 (synthesised)
#   CKFETCH_LINES=4 the scanout's line fetch holds four lines ahead, not
#                  two: with T2's output that busy a line was sometimes
#                  late and its start read as black (2026-11-06)

description = Sergei ML2 as a games console (console profile, T2)

base = sergei_ml2

defines =
	ZEITLOS_CONSOLE
	-GPU_CURSOR
	-KECCAK
	-COLOR_WINDOW
	-COLOR_DESKTOP
	-MONTMUL
	-MONTMUL_REGS
	-SHA256
	-USB_HOST
	USB_HID
	GAME_PLANES=2
	GPU_T2
	VRAM_SDRAM
	-GPU_RASTER
	NO_MPU
	NO_RTC
	NO_TRNG
	DDR3_WCOMBINE
	T2_TEX_LOG2=13
	CPU_ZEITLOS32P
	T2_TWO_LISTS=1
	T2_TWO_BUFS=1
	T2_ROW_END=1
	CKFETCH_LINES=4
