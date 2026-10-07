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

# Objects, and why each is linked:
#   zfont_data.o: the 8x8 font the screen draws with, and zgfx's fonts.
#   zcolor.o: full screen in colour (docs/color.md).
#   zfsapp.o: the files in /basic.
OBJS = zeitlos.o zobj.o zport.o zgfx.o zcolor.o zfont_data.o zwin.o zfsapp.o zgpio.o zi2c.o zi2cx.o \
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

basic_app.o: basic_app.c bplat.h bscreen.h bedit.h bterm.h ../../ext/basic/basic.h
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

.PHONY: test

CLEAN_EXTRA = $(BIN_TEST_SCREEN) $(BIN_TEST_BASIC) $(BIN_TEST_TERM)

include ../../common/app.mk
