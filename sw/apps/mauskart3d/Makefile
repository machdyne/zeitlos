# sw/apps/mauskart3d -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# A kart race on zgl and T2, the mouse in a matchbox (docs/mauskart3d.md).

APP = mauskart3d

SRC_DIRS = $(COMMON_DIR)/games $(COMMON_DIR)/games/ui $(COMMON_DIR)/zgl ../mausrun ../mausrun3d
APP_CFLAGS = -I$(COMMON_DIR)/zgl

# Objects, and why each is linked:
#   zarcade, zarcade_sys   the arcade runtime: game mode, chunky, keys
#   zsfx, zcfg             linked by the runtime
#   sui                    F1: the help on the game's own screen
#   zwin ... zedit         the window and the runtime's desktop help box
#   zgl*                   zgl, its frames, lists, chunky and T2 sides
#   track, karts           the Meadow Ring and the karts, as display lists
#   vfont                  letters of lines (../mausrun3d/vfont.c)
#   sound, sprites         mausrun's sounds and tunes; its tile drawings
OBJS = zeitlos.o zobj.o zgfx.o zcolor.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zdialog.o zflist.o zedit.o zfsapp.o zarcade.o zarcade_sys.o zsfx.o zcfg.o sui.o \
	zgl.o zgl_lines.o zgl_frames.o zgl_lists.o zgl_chunky.o zgl_t2.o \
	track.o karts.o vfont.o sound.o sprites.o mauskart3d.o

APP_STACK = 4M

# -- on a build host: the game, T2's model and the geometry core's ISS --
HOSTCC ?= cc
GAMES = ../../common/games
ZGL = ../../common/zgl
RENDER_SRC = mauskart3d.c track.c karts.c ../mausrun3d/vfont.c ../mausrun/sound.c ../mausrun/sprites.c \
	$(GAMES)/zarcade.c $(GAMES)/zsfx.c \
	$(GAMES)/tests/ar_host.c $(GAMES)/ui/sui.c ../../common/zfont_data.c \
	$(ZGL)/zgl.c $(ZGL)/zgl_lines.c $(ZGL)/zgl_frames.c $(ZGL)/zgl_lists.c $(ZGL)/zgl_t2.c \
	$(ZGL)/t2fw.c $(ZGL)/tests/t2_model.c $(ZGL)/tests/t2_raster.c $(ZGL)/tests/t2g_iss.c

tests/render_mauskart3d: tests/render.c $(RENDER_SRC) mauskart3d.h karts.h $(GAMES)/zarcade.h $(ZGL)/zgl_t2.h
	$(HOSTCC) -std=gnu99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
		-DZSFX_HOSTED -DAR_HOST_TESTS -Dmain=game_main -Wno-unused-parameter \
		-I$(ZGL) -I$(GAMES) \
		-o $@ tests/render.c $(RENDER_SRC)

../../t2fw/t2fw_tex.bin:
	$(MAKE) -C ../../t2fw

render: tests/render_mauskart3d ../../t2fw/t2fw_tex.bin
	rm -rf tests/out
	mkdir -p tests/out
	tests/render_mauskart3d t2 tests/out
	-python3 ../pix/tests/ppm2png.py tests/out

.PHONY: render
CLEAN_EXTRA = tests/render_mauskart3d tests/out

include ../../common/app.mk
