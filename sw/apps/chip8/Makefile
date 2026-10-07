# sw/apps/chip8 -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = chip8
APP_MAIN_RULE = 0
# chip8 -- CHIP-8 / SUPER-CHIP / XO-CHIP emulator
#   make            build chip8.elf and chip8.bin
#   make test       host unit tests (core + renderer). No toolchain needed.
#   make roms       download the test-suite ROMs
#   make suite      run them against tests/goldens.txt
#   make suite-pgm  ... and write a PGM of each result to /tmp/chip8-pgm
#   make goldens    regenerate tests/goldens.txt
#   make check      test + suite
#   make clean
#
# The host targets need only `cc`. `make test` is the one that must
# always pass: core.c and render.c include nothing from sw/common
# precisely so the guest machine and the display expansion can be
# checked without a board.

# Objects, and why each is linked:
#
#   zedit.o: sw/common/zedit.c -- the single-line text field.
#
#   zdialog.c owned it privately until sw/apps/web's URL bar became its
#   second caller. Every app that links the dialog needs this too: a
#   shared widget is only shared if the build says so.
OBJS = zeitlos.o zobj.o zgfx.o zcolor.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zflist.o zdialog.o zedit.o \
	core.o quirks.o render.o sound.o config.o disasm.o debug.o chip8.o

# zgfx.c references the font tables whenever anything draws text, and
# they live in their own translation unit -- so this is needed even
# though chip8 itself draws no text of its own.

core.o: core.c core.h
	$(CC) $(CFLAGS) -c core.c -o core.o

quirks.o: quirks.c core.h
	$(CC) $(CFLAGS) -c quirks.c -o quirks.o

render.o: render.c render.h core.h
	$(CC) $(CFLAGS) -c render.c -o render.o

sound.o: sound.c sound.h core.h
	$(CC) $(CFLAGS) -c sound.c -o sound.o

config.o: config.c config.h core.h
	$(CC) $(CFLAGS) -c config.c -o config.o

disasm.o: disasm.c disasm.h
	$(CC) $(CFLAGS) -c disasm.c -o disasm.o

debug.o: debug.c debug.h disasm.h core.h
	$(CC) $(CFLAGS) -c debug.c -o debug.o

chip8.o: chip8.c core.h render.h sound.h config.h debug.h
	$(CC) $(CFLAGS) -c chip8.c -S -MF /dev/null -o chip8.asm
	$(CC) $(CFLAGS) -c chip8.c -o chip8.o

# -- host tests ------------------------------------------------------
#
# -Wextra as well as -Wall, unlike the target build. core.c and
# render.c are pure computation over arrays with no OS to hide behind,
# so a sign-compare or an unused result in them is a real bug rather
# than toolchain noise.

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -O1 -g

PURE = core.c quirks.c render.c config.c disasm.c

BIN_TEST_CORE   = /tmp/chip8_test_core
BIN_TEST_RENDER = /tmp/chip8_test_render
BIN_TEST_CONFIG = /tmp/chip8_test_config
BIN_RUN         = /tmp/chip8_run_rom

$(BIN_TEST_CORE): tests/test_core.c core.h $(PURE)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_core.c core.c quirks.c

$(BIN_TEST_RENDER): tests/test_render.c render.h core.h $(PURE)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_render.c $(PURE)

$(BIN_TEST_CONFIG): tests/test_config.c config.h disasm.h core.h $(PURE)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_config.c config.c disasm.c quirks.c

$(BIN_RUN): tests/run_rom.c core.h $(PURE)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/run_rom.c core.c quirks.c

test: $(BIN_TEST_CORE) $(BIN_TEST_RENDER) $(BIN_TEST_CONFIG)
	@$(BIN_TEST_CORE)
	@$(BIN_TEST_RENDER)
	@$(BIN_TEST_CONFIG)

roms:
	@sh tests/fetch_roms.sh

# Exits 77 when the ROMs have not been downloaded -- "skipped", not
# "failed", the same convention sw/apps/hex/tests uses for its own host
# requirements.
suite: $(BIN_RUN)
	@sh tests/suite.sh; s=$$?; [ $$s = 77 ] && exit 0 || exit $$s

suite-pgm: $(BIN_RUN)
	@sh tests/suite.sh --pgm /tmp/chip8-pgm; s=$$?; \
		[ $$s = 77 ] && exit 0 || exit $$s

goldens: $(BIN_RUN)
	@sh tests/suite.sh --write

check: test suite

.PHONY: test suite suite-pgm goldens roms check

CLEAN_EXTRA = $(BIN_TEST_CORE) $(BIN_TEST_RENDER) $(BIN_TEST_CONFIG) $(BIN_RUN) /tmp/chip8-pgm

include ../../common/app.mk
