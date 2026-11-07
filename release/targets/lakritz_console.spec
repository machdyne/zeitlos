# Lakritz as a games console: the console profile (docs/profiles.md).
#
# It boots to a launcher in game mode -- no desktop, no dock -- and plays
# what is in /apps in 320x240 game mode: in two bitplanes for the
# arcade games -- the blitter as on every board, from block RAM at full
# speed (monochrome for now: the runtime's colour starts at 16) -- or in chunky game
# mode, 256 colours, with T2, the engine that runs the whole 3D frame
# (docs/zgl_t2.md), for the 3D ones -- whichever the game asks for.
#
# -- what goes, for room --
#
#   -SPI_ETH     no network: a console is not a terminal
#   -USB_HOST    the HID block instead (keyboards and gamepads), which
#                is a fraction of the host controller's fabric
#   -GPU_CURSOR  no desktop pointer
#
# -- what comes --
#
#   ZEITLOS_CONSOLE   the profile (FEATURES3 bits 1:0 = 1)
#   GAME_PLANES=2     two planes of VRAM in block RAM (10 DP16KD)
#   GAME_CHUNKY       chunky game mode, 256 colours
#   GPU_T2            T2 in the triangle engine's place
#   VRAM_SDRAM        chunky's picture in SDRAM: 256 colours take 40 of
#                     the 25F's 56 block RAMs as VRAM, and T2 needs 30
#                     of its own, so the planes stay in block RAM and
#                     the chunky picture is in main memory, read out a
#                     line ahead of the beam (docs/profiles.md,
#                     "Lakritz"; rtl/gpu/gpu_ckfetch.v)
#
#   -USB_CDC          the serial console on PMOD A instead (pmods =
#                     uart@a: UART0, a USB-serial adapter on pins 2/3)
#   NO_RTC            no wall clock: a console needs no date
#   NO_MPU            one game at a time: no memory protection
#   -GPU_RASTER       no line rasterizer: T2 draws the 3D lines, zgfx
#                     the 2D ones in software (sysctl answers its
#                     registers, so nothing waits on it for ever)
#   NO_TRNG           randomness seeded in software
#   -DCACHE           no data cache (about 10% of CPU speed on
#                     data-heavy code); the hardware audio mixer stays,
#                     which saves the CPU about 15% while music plays
#   PICORV32_REGS_LUTRAM, AUDIO_FIFO_LOG2=9, T2_STRIP_H=6
#                     block RAM: 56 of 56 (65 without)
#
# Measured (yowasp yosys 0.70 synth_ecp5 -abc9, nextpnr-ecp5 packing):
# TRELLIS_COMB 22,359 of 24,288 (92%) here, where the same flow put the
# previous spec at 24,738 against the 25,549 a release build reported --
# so about 95% in a release build. The lakritz board build is at 98%.
# docs/profiles.md, "Lakritz".

# CKFETCH_LINES=4: the scanout's line fetch four lines ahead -- without
#   a data cache the CPU keeps SDRAM busy, and lines were fetched late
#   (whole lines black, flashing; 2026-11-06). About 120 LUT4.
# NO_DDMI_AUDIO: the console's sound over HDMI left out -- with it the
#   build placed at 24,560 of 24,288 LUT4 (101%); it is about 930.
description = Lakritz as a games console (console profile, T2)

base  = lakritz
pmods = uart@a

defines =
	ZEITLOS_CONSOLE
	-SPI_ETH
	-USB_HOST
	USB_HID
	-GPU_CURSOR
	GAME_PLANES=2
	GAME_CHUNKY
	GPU_T2
	VRAM_SDRAM
	-USB_CDC
	NO_RTC
	NO_MPU
	-GPU_RASTER
	NO_TRNG
	PICORV32_REGS_LUTRAM
	-DCACHE
	AUDIO_FIFO_LOG2=9
	T2_STRIP_H=6
	CKFETCH_LINES=4
	NO_DDMI_AUDIO
