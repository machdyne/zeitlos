# chip8 -- CHIP-8 / SUPER-CHIP / XO-CHIP emulator
#
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

PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS -- a command-line assignment replaces the makefile's outright,
# silently dropping -Os, -march and the libc specs and producing a much
# larger binary that can overrun the space the loader has for it.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	$(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o \
	zfsapp.o zflist.o zdialog.o zedit.o \
	core.o quirks.o render.o sound.o config.o disasm.o debug.o chip8.o

chip8: chip8.elf chip8.bin

zeitlos.o: ../../common/zeitlos.c ../../common/zeitlos.h
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o: ../../common/zobj.c ../../common/zobj.h
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zgfx.o: ../../common/zgfx.c ../../common/zgfx.h
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

# zgfx.c references the font tables whenever anything draws text, and
# they live in their own translation unit -- so this is needed even
# though chip8 itself draws no text of its own.
zfont_data.o: ../../common/zfont_data.c ../../common/zfont.h
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

zwin.o: ../../common/zwin.c ../../common/zwin.h
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o

zwidget.o: ../../common/zwidget.c ../../common/zwidget.h
	$(CC) $(CFLAGS) -c ../../common/zwidget.c -o zwidget.o

zfsapp.o: ../../common/zfsapp.c ../../common/zfsapp.h
	$(CC) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o

zflist.o: ../../common/zflist.c ../../common/zflist.h
	$(CC) $(CFLAGS) -c ../../common/zflist.c -o zflist.o

zdialog.o: ../../common/zdialog.c ../../common/zdialog.h
	$(CC) $(CFLAGS) -c ../../common/zdialog.c -o zdialog.o

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
	$(CC) $(CFLAGS) -c chip8.c -S -o chip8.asm
	$(CC) $(CFLAGS) -c chip8.c -o chip8.o

chip8.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,chip8.map -o chip8.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble chip8.elf > chip8.dasm

chip8.bin: chip8.elf
	@EDATA=$$($(PREFIX)nm chip8.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm chip8.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary chip8.elf chip8.data; \
	python3 ../../../tools/mkexec.py chip8.data chip8.bin $$(($$END - $$EDATA)); \
	rm -f chip8.data

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

clean:
	rm -f chip8.elf chip8.bin chip8.data chip8.asm chip8.dasm chip8.map *.o *.d
	rm -f $(BIN_TEST_CORE) $(BIN_TEST_RENDER) $(BIN_TEST_CONFIG) $(BIN_RUN)
	rm -rf /tmp/chip8-pgm

.PHONY: chip8 test suite suite-pgm goldens roms check clean

# Header dependency tracking. CFLAGS carries -MD, so gcc writes .d
# files listing every header each object depends on; including them
# here is what makes editing a header actually rebuild what uses it,
# rather than linking silently against stale objects.
-include $(wildcard *.d)
-include $(wildcard */*.d)

# sw/common/zedit.c -- the single-line text field.
#
# zdialog.c owned it privately until sw/apps/web's URL bar became its
# second caller. Every app that links the dialog needs this too: a
# shared widget is only shared if the build says so.
zedit.o:
	$(CC) $(CFLAGS) -c ../../common/zedit.c -o zedit.o
