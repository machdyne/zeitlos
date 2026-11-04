# sw/apps/gldemo -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# zgl's demo and test scene (docs/gldemo_app.md, docs/zgl.md).

APP = gldemo

SRC_DIRS = $(COMMON_DIR)/games $(COMMON_DIR)/games/ui $(COMMON_DIR)/zgl
APP_CFLAGS = -I$(COMMON_DIR)/zgl

OBJS = zeitlos.o zobj.o zgfx.o zcolor.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zdialog.o zflist.o zedit.o zfsapp.o zarcade.o zarcade_sys.o zsfx.o zcfg.o sui.o \
	zgl.o zgl_lines.o zgl_frames.o zgl_lists.o zgl_chunky.o zgl_t2.o gldemo.o

# stack and heap: zgl's frame (1,024 primitives, 72 bytes each), the
# sphere's display list and the arcade runtime; and on a `VRAM_SDRAM
# console the chunky picture in main memory (76,800 bytes)
APP_STACK = 512K

HOSTCC ?= cc
GAMES = ../../common/games
RENDER_SRC = gldemo.c $(GAMES)/zarcade.c $(GAMES)/zsfx.c $(GAMES)/tests/ar_host.c \
	$(GAMES)/ui/sui.c ../../common/zfont_data.c ../../common/zgl/zgl.c \
	../../common/zgl/zgl_lines.c ../../common/zgl/zgl_frames.c ../../common/zgl/zgl_lists.c

tests/render_gldemo: tests/render.c $(RENDER_SRC) $(GAMES)/zarcade.h $(GAMES)/zarcade_int.h
	$(HOSTCC) -std=gnu99 -Wall -Wextra -O1 -g -fsanitize=address,undefined \
		-DZSFX_HOSTED -DAR_HOST_TESTS -Dmain=game_main -Wno-unused-parameter \
		-Wno-missing-field-initializers -I../../common/zgl -I$(GAMES) \
		-o $@ tests/render.c $(RENDER_SRC)

render: tests/render_gldemo
	mkdir -p tests/out
	tests/render_gldemo tests/out
	-python3 ../pix/tests/ppm2png.py tests/out

.PHONY: render
CLEAN_EXTRA = tests/render_gldemo tests/out

include ../../common/app.mk
