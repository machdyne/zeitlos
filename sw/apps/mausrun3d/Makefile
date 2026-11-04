# sw/apps/mausrun3d -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# Maus Run in an open 3D world, on zgl and T2 (docs/mausrun3d.md).

APP = mausrun3d

SRC_DIRS = $(COMMON_DIR)/games $(COMMON_DIR)/games/ui $(COMMON_DIR)/zgl ../mausrun
APP_CFLAGS = -I$(COMMON_DIR)/zgl

# Objects, and why each is linked:
#   zarcade, zarcade_sys   the arcade runtime: game mode, chunky, keys
#   zsfx, zcfg             linked by the runtime
#   sui                    F1: the help on the game's own screen
#   zwin ... zedit         the window and the runtime's desktop help box
#   zgl*                   zgl, its frames, lists, chunky and T2 sides
#   world, world_builtin   the map, its cells as display lists; world 1
#   models, vfont          everyone and everything; letters of lines
#   sound                  mausrun's sounds and tunes (../mausrun/sound.c)
OBJS = zeitlos.o zobj.o zgfx.o zcolor.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zdialog.o zflist.o zedit.o zfsapp.o zarcade.o zarcade_sys.o zsfx.o zcfg.o sui.o \
	zgl.o zgl_lines.o zgl_frames.o zgl_lists.o zgl_chunky.o zgl_t2.o \
	world.o world_builtin.o models.o vfont.o sound.o mausrun3d.o

# the world (a 128x128 map is 256 KB of blocks), its cells' lists, zgl's
# frame for the software fallback; 8M is the kernel's cap (a 512x512
# world will want an arena, docs/app_runtime.md)
APP_STACK = 8M

# world_builtin.c is GENERATED from sw/data/mausrun3d/world01.txt and
# committed (as mausrun's levels_builtin.c):
#   python3 gen_world.py ../../data/mausrun/levels/01.txt > ../../data/mausrun3d/world01.txt
#   python3 gen_world.py --builtin ../../data/mausrun3d/world01.txt mr3_world01 > world_builtin.c

# -- on a build host: the game, T2's model and the geometry core's ISS --
HOSTCC ?= cc
GAMES = ../../common/games
ZGL = ../../common/zgl
RENDER_SRC = mausrun3d.c world.c world_builtin.c models.c vfont.c ../mausrun/sound.c $(GAMES)/zarcade.c $(GAMES)/zsfx.c \
	$(GAMES)/tests/ar_host.c $(GAMES)/ui/sui.c ../../common/zfont_data.c \
	$(ZGL)/zgl.c $(ZGL)/zgl_lines.c $(ZGL)/zgl_frames.c $(ZGL)/zgl_lists.c $(ZGL)/zgl_t2.c \
	$(ZGL)/t2fw.c $(ZGL)/tests/t2_model.c $(ZGL)/tests/t2_raster.c $(ZGL)/tests/t2g_iss.c

tests/render_mausrun3d: tests/render.c $(RENDER_SRC) world.h models.h vfont.h $(GAMES)/zarcade.h $(GAMES)/zarcade_int.h $(ZGL)/zgl_t2.h
	$(HOSTCC) -std=gnu99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
		-DZSFX_HOSTED -DAR_HOST_TESTS -Dmain=game_main -Wno-unused-parameter \
		-I$(ZGL) -I$(GAMES) \
		-o $@ tests/render.c $(RENDER_SRC)

# the geometry core's program, for its cycles (sw/t2fw)
../../t2fw/t2fw.bin:
	$(MAKE) -C ../../t2fw

render: tests/render_mausrun3d ../../t2fw/t2fw.bin
	rm -rf tests/out
	mkdir -p tests/out
	tests/render_mausrun3d t2 tests/out
	-python3 ../pix/tests/ppm2png.py tests/out

# the CPU's side of a frame in the simulator, its cycle model on and T2
# its model: where the cycles go (tests/simbench/simbench.c)
simbench:
	$(MAKE) -C tests/simbench
	ZSIM_COST=ddr3 ZSIM_GTRI=2 ../../../sim/zsim-headless -q -m 0x4000000 -n 50000000 tests/simbench/simbench.bin

.PHONY: render simbench
CLEAN_EXTRA = tests/render_mausrun3d tests/out

include ../../common/app.mk
