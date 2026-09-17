# Zeitlos roulette.

PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- never hardcode them. Some boards are
# rv32i and a hardcoded rv32im produces a binary that traps on its
# first multiply.
include ../../common/arch.mk
CC = $(PREFIX)gcc
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)

GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# -DZ_GFX_HW_BLIT, same as wm/chess/poker: text goes through the
# hardware glyph blitter. This app does not call z_gfx_hw_font_load()
# itself -- wm is the only process that loads glyph memory, and it
# loads z_font_5x8, which is the font used here.
#
# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS, which would otherwise replace them outright and silently drop
# -Os, -march and the libc specs.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	-DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zshape.o zfont_data.o zwin.o zrng.o zfsapp.o \
	zrand.o zrand_sys.o zbank.o \
	rl_table.o rl_wheel.o rl_board.o rl_input.o roulette.o

roulette: roulette.elf roulette.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o
zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o
zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o
zshape.o:
	$(CC) $(CFLAGS) -c ../../common/zshape.c -o zshape.o
zfont_data.o:
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o
zwin.o:
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o
zrng.o:
	$(CC) $(CFLAGS) -c ../../common/zrng.c -o zrng.o

# The bank lives in a file, so this app links the filesystem wrappers.
zfsapp.o:
	$(CC) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o

zrand.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zrand.c -o zrand.o
zrand_sys.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zrand_sys.c -o zrand_sys.o
zbank.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zbank.c -o zbank.o

rl_table.o:
	$(CC) $(CFLAGS) -c rl_table.c -o rl_table.o
rl_wheel.o:
	$(CC) $(CFLAGS) -c rl_wheel.c -o rl_wheel.o
rl_board.o:
	$(CC) $(CFLAGS) -c rl_board.c -o rl_board.o
rl_input.o:
	$(CC) $(CFLAGS) -c rl_input.c -o rl_input.o
roulette.o:
	$(CC) $(CFLAGS) -c roulette.c -o roulette.o

roulette.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,roulette.map -o roulette.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble roulette.elf > roulette.dasm

# A .zexe rather than a raw --pad-to binary: the header carries .bss as
# a NUMBER instead of writing it out as literal zeros to be read off the
# SD card on every launch. See sw/common/zexec.h.
roulette.bin: roulette.elf
	@EDATA=$$($(PREFIX)nm roulette.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm roulette.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary roulette.elf roulette.data; \
	python3 ../../../tools/mkexec.py roulette.data roulette.bin \
		$$(($$END - $$EDATA)); \
	rm -f roulette.data

# -- host-side tests --

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -no-pie -O2

COMMON = ../../common
GAMES = $(COMMON)/games

# The software pixel primitives, so zshape.c links on a build machine
# and the wheel can actually be DRAWN in a test rather than only
# computed. zgfx.c cannot be linked here -- it is register writes from
# top to bottom -- which is exactly why zshape.c is its own object.
PANELSRC = $(COMMON)/zwin.c $(COMMON)/zwidget.c $(COMMON)/zfont_data.c \
	$(COMMON)/zobj.c $(COMMON)/zeitlos.c
PANELFLAGS = $(HOSTFLAGS) -I $(COMMON)

# The table has no I/O in it at all, so the tests link the SHIPPED
# source with nothing else. -I is deliberately absent: it is the header
# search the target build actually has, and sw/apps/chess's Makefile
# records what happens when a host target quietly has more.
ENGINESRC = rl_table.c rl_wheel.c rl_board.c $(COMMON)/zshape.c

TARGETSRC = rl_table.c rl_wheel.c rl_board.c rl_input.c roulette.c

includecheck:
	@for f in $(TARGETSRC); do \
		$(HOSTCC) -std=gnu99 -Wall -fsyntax-only $$f || exit 1; \
	done
	@echo "includecheck: every target source resolves its headers with no -I"

# Some boards here are rv32i with no FPU, where a single double links
# the soft-float runtime -- tens of kilobytes, on an app that
# docs/app_runtime.md says can crash on start if it outgrows the
# loader's space. Comments are stripped first, so prose about floating
# point does not read as evidence of it.
nofloat:
	@for f in $(TARGETSRC) *.h; do \
		if $(HOSTCC) -fpreprocessed -dD -E $$f 2>/dev/null | \
			sed 's/"[^"]*"//g' | \
			grep -nE '(^|[^_[:alnum:]])(float|double)([^_[:alnum:]]|$$)'; then \
			echo "nofloat: FAILED -- floating point in $$f"; exit 1; \
		fi; \
	done
	@echo "nofloat: the table is integer-only"

# Runs the shared library's tests too. Roulette is its first caller, so
# a change there that breaks the bank should fail here rather than on
# the device.
# Runs the shared library's tests too, and sw/common/zshape.c's.
# Roulette is the first caller of both, so a change there that breaks
# the bank or the circles should fail here rather than on the device.
test: includecheck nofloat
	$(MAKE) -C $(GAMES) test
	$(HOSTCC) $(PANELFLAGS) -o /tmp/zshape_test \
		$(COMMON)/tests/shape_test.c $(COMMON)/zshape.c $(PANELSRC)
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/roulette_table \
		tests/table_test.c rl_table.c
	$(HOSTCC) $(PANELFLAGS) -o /tmp/roulette_wheel \
		tests/wheel_test.c rl_wheel.c rl_table.c $(COMMON)/zshape.c $(PANELSRC)
	$(HOSTCC) $(PANELFLAGS) -o /tmp/roulette_board \
		tests/board_test.c rl_board.c rl_wheel.c rl_table.c \
		$(COMMON)/zshape.c $(PANELSRC)
	$(HOSTCC) $(PANELFLAGS) -o /tmp/roulette_input \
		tests/input_test.c rl_input.c rl_board.c rl_wheel.c rl_table.c \
		$(COMMON)/zshape.c $(PANELSRC)
	/tmp/zshape_test
	/tmp/roulette_table
	/tmp/roulette_wheel
	/tmp/roulette_board
	/tmp/roulette_input

# Every layer at once, under -fsanitize=address,undefined: place bets
# by command line and by mouse, spin the wheel frame by frame with the
# repair redraws the real app performs, settle, repeat -- on both
# wheels, at three window sizes, for 2,500 rounds.
#
# Separate from `test` because the sanitizers make it slow, and because
# a build machine without them should still run the suite. Worth
# running after any change to the drawing or the parsing: a one-byte
# overrun is invisible on the host without it.
stress:
	$(HOSTCC) --std=gnu99 -g -O1 -no-pie -I $(COMMON) \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-o /tmp/roulette_stress tests/stress.c rl_input.c rl_board.c \
		rl_wheel.c rl_table.c $(COMMON)/zshape.c $(GAMES)/zrand.c $(PANELSRC)
	ASAN_OPTIONS=detect_leaks=0 /tmp/roulette_stress

# Needs the RISC-V toolchain and a completed `make`. The definitive
# version of what `nofloat` checks lexically: with every source compiled
# for rv32i, not one soft-float helper may be referenced.
#
# SCOPED TO THIS APP'S OWN OBJECTS. sw/common/zobj.c has a float member
# in z_obj_t and compares it with fabsf(), so zobj.o pulls __subsf3 into
# EVERY app in this tree. That is a property of the shared object system
# and a separate question from whether this app kept its own arithmetic
# integer; failing here on somebody else's float would make the check
# something people turn off.
APPOBJS = rl_table.o rl_wheel.o rl_board.o rl_input.o roulette.o \
	zshape.o zrand.o zbank.o

nmcheck:
	@if $(PREFIX)nm $(APPOBJS) | \
		grep -E 'U __(add|sub|mul|div|fix|float|cmp|unord)[ds]f'; then \
		echo "nmcheck: FAILED -- soft-float reached this app's objects"; \
		exit 1; \
	fi
	@echo "nmcheck: no soft-float in this app's target objects"
	@$(PREFIX)size roulette.elf 2>/dev/null || true

# LOOK AT THE OUTPUT before changing the wheel.
#
#   make render              one wheel, large
#   make render WHAT=small   several sizes, and an american wheel
#   make render WHAT=spin    eight frames across one spin
#   make render WHAT=board   the whole table, windowed
#   make render WHAT=game    the same, on a 320x240 game-mode page
#
# Three things that no assertion caught and one look did: the ball was
# invisible, drawn white on the lit rim rather than in the dark track
# outside it; the pockets were speckled, because a wedge filled with
# closely spaced radial lines leaves gaps as the radius grows; and the
# chips spilled two cells sideways, because the value was drawn beside
# the chip instead of inside it.
WHAT ?= wheel

render:
	$(HOSTCC) $(PANELFLAGS) -o /tmp/roulette_render \
		tests/render.c rl_board.c rl_wheel.c rl_table.c \
		$(COMMON)/zshape.c $(PANELSRC)
	/tmp/roulette_render /tmp/roulette.pbm $(WHAT)

clean:
	rm -f *.o *.d roulette.elf roulette.bin roulette.data roulette.map roulette.dasm

.PHONY: roulette test clean includecheck nofloat nmcheck render stress

-include $(wildcard *.d)
