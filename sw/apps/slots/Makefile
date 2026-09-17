# Zeitlos slots.

COMMON = ../../common
GAMES = $(COMMON)/games

PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- never hardcode them. Some boards are
# rv32i and a hardcoded rv32im produces a binary that traps on its first
# multiply.
include $(COMMON)/arch.mk
CC = $(PREFIX)gcc

GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# -DZ_GFX_HW_BLIT: text goes through the hardware glyph blitter. This
# app does not load glyph memory itself -- wm is the only process that
# does, and it loads z_font_5x8, the font used here.
#
# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS, which would otherwise replace them outright.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	-DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = $(COMMON)/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zshape.o zfont_data.o zwin.o zrng.o zfsapp.o \
	zrand.o zrand_sys.o zbank.o \
	sl_reels.o sl_game.o sl_art.o sl_board.o sl_input.o slots.o

slots: slots.elf slots.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zeitlos.c -o zeitlos.o
zobj.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zobj.c -o zobj.o
zgfx.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zgfx.c -o zgfx.o
# The handle's knob is a filled circle, which lives in zshape.c. The
# host render linked it explicitly and the target link did not, so this
# only surfaced as an undefined reference at the very end -- which is
# what a target link is for.
zshape.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zshape.c -o zshape.o
zfont_data.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zfont_data.c -o zfont_data.o
zwin.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zwin.c -o zwin.o
zrng.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zrng.c -o zrng.o
zfsapp.o:
	$(CC) $(CFLAGS) -c $(COMMON)/zfsapp.c -o zfsapp.o
zrand.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zrand.c -o zrand.o
zrand_sys.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zrand_sys.c -o zrand_sys.o
zbank.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zbank.c -o zbank.o

sl_reels.o:
	$(CC) $(CFLAGS) -c sl_reels.c -o sl_reels.o
sl_game.o:
	$(CC) $(CFLAGS) -c sl_game.c -o sl_game.o
sl_art.o:
	$(CC) $(CFLAGS) -c sl_art.c -o sl_art.o
sl_board.o:
	$(CC) $(CFLAGS) -c sl_board.c -o sl_board.o
sl_input.o:
	$(CC) $(CFLAGS) -c sl_input.c -o sl_input.o
slots.o:
	$(CC) $(CFLAGS) -c slots.c -o slots.o

slots.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,slots.map -o slots.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble slots.elf > slots.dasm

# A .zexe rather than a raw --pad-to binary: the header carries .bss as
# a NUMBER instead of writing it out as literal zeros to be read off the
# SD card on every launch. See sw/common/zexec.h.
slots.bin: slots.elf
	@EDATA=$$($(PREFIX)nm slots.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm slots.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary slots.elf slots.data; \
	python3 ../../../tools/mkexec.py slots.data slots.bin \
		$$(($$END - $$EDATA)); \
	rm -f slots.data

# Needs the RISC-V toolchain and a completed `make`. Scoped to this
# app's own objects: sw/common/zobj.c has a float in z_obj_t and pulls
# __subsf3 into every app in this tree.
APPOBJS = sl_reels.o sl_game.o sl_art.o sl_board.o sl_input.o slots.o \
	zshape.o zrand.o zbank.o

nmcheck:
	@if $(PREFIX)nm $(APPOBJS) | \
		grep -E 'U __(add|sub|mul|div|fix|float|cmp|unord)[ds]f'; then \
		echo "nmcheck: FAILED -- soft-float reached this app's objects"; \
		exit 1; \
	fi
	@echo "nmcheck: no soft-float in this app's target objects"
	@$(PREFIX)size slots.elf 2>/dev/null || true

# -- host-side tests --

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -no-pie -O2

ENGINESRC = sl_reels.c sl_game.c
TARGETSRC = sl_reels.c sl_game.c sl_board.c sl_input.c slots.c

# The software pixel primitives, so a test can actually DRAW the reels
# rather than only compute where they are.
PANELSRC = $(COMMON)/zwin.c $(COMMON)/zwidget.c $(COMMON)/zfont_data.c \
	$(COMMON)/zobj.c $(COMMON)/zeitlos.c
PANELFLAGS = $(HOSTFLAGS) -I $(COMMON)

includecheck:
	@for f in $(TARGETSRC) *.h; do \
		$(HOSTCC) -std=gnu99 -Wall -fsyntax-only $$f || exit 1; \
	done
	@echo "includecheck: every target source resolves its headers with no -I"

# Comments AND string literals are stripped first -- comments because
# prose about floating point should not read as evidence of it, string
# literals because a paytable has symbols with names.
nofloat:
	@for f in $(TARGETSRC) *.h; do \
		if $(HOSTCC) -fpreprocessed -dD -E $$f 2>/dev/null | \
			sed 's/"[^"]*"//g' | \
			grep -nE '(^|[^_[:alnum:]])(float|double)([^_[:alnum:]]|$$)'; then \
			echo "nofloat: FAILED -- floating point in $$f"; exit 1; \
		fi; \
	done
	@echo "nofloat: the reels are integer-only"

test: includecheck nofloat
	$(MAKE) -C $(GAMES) test
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/slots_reels tests/reel_test.c $(ENGINESRC)
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/slots_spin tests/spin_test.c $(ENGINESRC)
	$(HOSTCC) $(PANELFLAGS) -o /tmp/slots_layout tests/layout_test.c \
		sl_board.c $(ENGINESRC) sl_art.c $(COMMON)/zshape.c $(PANELSRC)
	/tmp/slots_reels
	/tmp/slots_spin
	/tmp/slots_layout

# LOOK AT THE OUTPUT before changing the machine.
#
#   make render              idle
#   make render WHAT=spin    mid-spin, reels at different offsets
#   make render WHAT=win     landed
#
# Two things a render caught here and no assertion would have: the
# cherry symbol read as a face, and the reels hung under the status line
# left a band of nothing taller than the reels themselves.
WHAT ?= idle

render:
	$(HOSTCC) $(PANELFLAGS) -o /tmp/slots_render tests/render.c \
		sl_board.c $(ENGINESRC) sl_art.c $(COMMON)/zshape.c $(PANELSRC)
	/tmp/slots_render /tmp/slots.pbm $(WHAT)

# The symbol tiles are generated and COMMITTED, so this must reproduce
# them byte for byte. sw/common/games/gen_cards.py drifted from its own
# output once; check with `git diff` after running it.
art:
	python3 gen_slots.py

clean:
	rm -f *.o *.d slots.elf slots.bin slots.data slots.map slots.dasm

.PHONY: slots test clean includecheck nofloat nmcheck art render

-include $(wildcard *.d)
