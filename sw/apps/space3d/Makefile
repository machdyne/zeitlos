# sw/apps/space3d -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = space3d

# -march/-mabi on the LINK line too, not just for compiling: gcc uses
# them to pick which multilib's libc/libgcc to search, and omitting
# them silently falls back to the toolchain default -- which may assume
# an ISA the configured PicoRV32 doesn't have. See docs/user_input.md,
# "Debugging notes".

SRC_DIRS = $(COMMON_DIR)/games $(COMMON_DIR)/games/ui

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
	space3d.o

# the game itself on the runtime's software framebuffer (tests/render.c)
HOSTCC ?= cc
GAMES = ../../common/games
RENDER_SRC = space3d.c $(GAMES)/zarcade.c $(GAMES)/zsfx.c $(GAMES)/tests/ar_host.c \
	$(GAMES)/ui/sui.c ../../common/zfont_data.c

tests/render_space3d: tests/render.c $(RENDER_SRC) $(GAMES)/zarcade.h $(GAMES)/zarcade_int.h
	$(HOSTCC) -std=gnu99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
		-DZSFX_HOSTED -DAR_HOST_TESTS -Dmain=game_main -Wno-unused-parameter \
		-o $@ tests/render.c $(RENDER_SRC)

render: tests/render_space3d
	mkdir -p tests/out
	tests/render_space3d tests/out
	-python3 ../pix/tests/ppm2png.py tests/out

.PHONY: render
CLEAN_EXTRA = tests/render_space3d tests/out

include ../../common/app.mk
