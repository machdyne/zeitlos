# sw/apps/basic -- the BASIC computer (docs/basic_app.md). Build rules are
# shared: sw/common/app.mk, docs/build.md.
#
#   make            build basic.elf and basic.bin
#   make test       host tests: the screen, BASIC programs through the
#                   real interpreter with the graphics extensions, and the
#                   terminal's bytes (basic0). No toolchain or board needed.
#
# The interpreter is sw/ext/basic (Machdyne BASIC, a submodule), built with
# its extension interface on (BASIC_EXT: bext.c) and room for programs of
# up to 32KB (MAX_PROG; BASIC 1 promises at least 1KB).

APP = basic
APP_MAIN_RULE = 0

BASIC_DEFS = -DBASIC_EXT -DMAX_PROG=32768

SRC_DIRS = $(COMMON_DIR)/games $(COMMON_DIR)/games/ui

# Objects, and why each is linked:
#   zarcade, zarcade_sys   the arcade runtime: the window, full screen,
#                          colour and the colour window (docs/arcade.md)
#   zsfx, zcfg             linked by the runtime; BASIC makes no sound
#   sui                    F1: the help on BASIC's own screen
#   zwin, zwidget, zspeak,
#   zdialog, zflist, zedit the window, and the runtime's desktop help box
#                          (unused while sui is linked)
#   zfont_data             the 8x8 font the screen draws with
#   zgfx, zcolor           the blitter and colour
#   zfsapp                 the files in /data/basic
#   zport                  basic0, for a terminal
#   zgpio, zi2c, zi2cx     I2C on pins 3 and 4, through bench
OBJS = zeitlos.o zobj.o zport.o zgfx.o zcolor.o zfont_data.o zwin.o zwidget.o \
	zspeak.o zfsapp.o zdialog.o zflist.o zedit.o zgpio.o zi2c.o zi2cx.o \
	zarcade.o zarcade_sys.o zsfx.o zcfg.o sui.o \
	basic_interp.o bscreen.o bedit.o bterm.o bext.o basic_app.o

basic_interp.o: ../../ext/basic/basic.c ../../ext/basic/basic.h
	$(CC) $(CFLAGS) $(BASIC_DEFS) -c ../../ext/basic/basic.c -o basic_interp.o

bscreen.o: bscreen.c bscreen.h
	$(CC) $(CFLAGS) -c bscreen.c -o bscreen.o

bedit.o: bedit.c bedit.h bscreen.h
	$(CC) $(CFLAGS) -c bedit.c -o bedit.o

bterm.o: bterm.c bterm.h
	$(CC) $(CFLAGS) -c bterm.c -o bterm.o

bext.o: bext.c bplat.h bscreen.h ../../ext/basic/basic.h
	$(CC) $(CFLAGS) $(BASIC_DEFS) -c bext.c -o bext.o

basic_app.o: basic_app.c bplat.h bscreen.h bedit.h bterm.h ../../ext/basic/basic.h \
		../../common/games/zarcade.h
	$(CC) $(CFLAGS) $(BASIC_DEFS) -c basic_app.c -o basic_app.o

# -- host tests ------------------------------------------------------

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -O1 -g $(BASIC_DEFS)

BIN_TEST_SCREEN = /tmp/basic_test_screen
BIN_TEST_BASIC  = /tmp/basic_test_basic
BIN_TEST_TERM   = /tmp/basic_test_term
HOST_SRC = bscreen.c ../../common/zfont_data.c

$(BIN_TEST_SCREEN): tests/test_screen.c bscreen.c bscreen.h
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_screen.c $(HOST_SRC)

$(BIN_TEST_BASIC): tests/test_basic.c tests/host.c bext.c bedit.c bscreen.c bplat.h \
		../../ext/basic/basic.c ../../ext/basic/basic.h
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_basic.c tests/host.c bext.c \
		$(HOST_SRC) ../../ext/basic/basic.c

$(BIN_TEST_TERM): tests/test_term.c bterm.c bterm.h bedit.c bedit.h
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_term.c bterm.c bedit.c $(HOST_SRC)

test: $(BIN_TEST_SCREEN) $(BIN_TEST_BASIC) $(BIN_TEST_TERM)
	@$(BIN_TEST_SCREEN)
	@$(BIN_TEST_BASIC)
	@$(BIN_TEST_TERM)

# the app itself on the runtime's software framebuffer, driven by a
# script (tests/render.c): every frame against a full repaint
GAMES = ../../common/games
RENDER_SRC = basic_app.c bscreen.c bedit.c bext.c ../../ext/basic/basic.c \
	$(GAMES)/zarcade.c $(GAMES)/zsfx.c $(GAMES)/tests/ar_host.c $(GAMES)/ui/sui.c \
	../../common/zfont_data.c

tests/render_basic: tests/render.c $(RENDER_SRC) bplat.h bscreen.h bedit.h \
		$(GAMES)/zarcade.h $(GAMES)/zarcade_int.h
	$(HOSTCC) $(HOSTFLAGS) -fsanitize=address,undefined -DZSFX_HOSTED -DAR_HOST_TESTS \
		-Dmain=game_main -Wno-unused-parameter -o $@ tests/render.c $(RENDER_SRC)

render: tests/render_basic
	mkdir -p tests/out
	AR_HOST_VERIFY=1 tests/render_basic tests/out
	-python3 ../pix/tests/ppm2png.py tests/out

.PHONY: test render

CLEAN_EXTRA = $(BIN_TEST_SCREEN) $(BIN_TEST_BASIC) $(BIN_TEST_TERM) tests/render_basic tests/out

include ../../common/app.mk
