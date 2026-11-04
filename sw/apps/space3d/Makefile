# sw/apps/space3d -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = space3d

# -march/-mabi on the LINK line too, not just for compiling: gcc uses
# them to pick which multilib's libc/libgcc to search, and omitting
# them silently falls back to the toolchain default -- which may assume
# an ISA the configured PicoRV32 doesn't have. See docs/user_input.md,
# "Debugging notes".

SRC_DIRS = $(COMMON_DIR)/games $(COMMON_DIR)/games/ui $(COMMON_DIR)/zgl
APP_CFLAGS = -I$(COMMON_DIR)/zgl

# Objects, and why each is linked:
#   zarcade, zarcade_sys   the arcade runtime: the window, full screen,
#                          colour and the colour window (docs/arcade.md)
#   zsfx, zcfg             linked by the runtime; space3d makes no sound
#   sui                    F1: the help on the game's own screen
#   zwin, zwidget, zspeak,
#   zdialog, zflist, zedit the window, and the runtime's desktop help box
#                          (unused while sui is linked)
#   zgfx, zcolor, zfont_data  the blitter, the rasterizer, colour, text
OBJS = zeitlos.o zobj.o zgfx.o zcolor.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zdialog.o zflist.o zedit.o zfsapp.o zarcade.o zarcade_sys.o zsfx.o zcfg.o sui.o \
	zgl.o zgl_lines.o zgl_frames.o zgl_lists.o zgl_chunky.o zgl_t2.o space3d.o

# stack and heap: the solid look's frame (zgl, MAX_PRIMS of 72 bytes) and
# the wireframe's two display lists. The default 16K had room for the
# lists only, and the solid look said "no memory" and stayed wireframe.
APP_STACK = 128K

# the game itself on the runtime's software framebuffer (tests/render.c)
HOSTCC ?= cc
GAMES = ../../common/games
RENDER_SRC = space3d.c $(GAMES)/zarcade.c $(GAMES)/zsfx.c $(GAMES)/tests/ar_host.c \
	$(GAMES)/ui/sui.c ../../common/zfont_data.c ../../common/zgl/zgl.c ../../common/zgl/zgl_lines.c \
	../../common/zgl/zgl_frames.c ../../common/zgl/zgl_lists.c

tests/render_space3d: tests/render.c $(RENDER_SRC) $(GAMES)/zarcade.h $(GAMES)/zarcade_int.h
	$(HOSTCC) -std=gnu99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
		-DZSFX_HOSTED -DAR_HOST_TESTS -Dmain=game_main -Wno-unused-parameter \
		-I../../common/zgl -I$(GAMES) \
		-o $@ tests/render.c $(RENDER_SRC)

render: tests/render_space3d
	mkdir -p tests/out
	tests/render_space3d tests/out
	-python3 ../pix/tests/ppm2png.py tests/out

# game mode with chunky: software and T2 (its model), held to each other
ZGL = ../../common/zgl
CHUNKY_SRC = $(RENDER_SRC) $(ZGL)/zgl_t2.c $(ZGL)/t2fw.c $(ZGL)/tests/t2_model.c $(ZGL)/tests/t2_raster.c

tests/render_chunky: tests/render_chunky.c $(CHUNKY_SRC) $(GAMES)/zarcade.h $(GAMES)/zarcade_int.h $(ZGL)/zgl_t2.h
	$(HOSTCC) -std=gnu99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
		-DZSFX_HOSTED -DAR_HOST_TESTS -Dmain=game_main -Wno-unused-parameter \
		-I../../common/zgl -I$(GAMES) \
		-o $@ tests/render_chunky.c $(CHUNKY_SRC)

render-chunky: tests/render_chunky
	mkdir -p tests/out_soft tests/out_t2
	tests/render_chunky soft tests/out_soft
	tests/render_chunky t2 tests/out_t2
	python3 tests/ppm_diff.py tests/out_soft tests/out_t2
	-python3 ../pix/tests/ppm2png.py tests/out_t2

.PHONY: render render-chunky
CLEAN_EXTRA = tests/render_space3d tests/out tests/render_chunky tests/out_soft tests/out_t2

include ../../common/app.mk
