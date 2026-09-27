# sw/apps/slots -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = slots
APP_LISTING = 0
# Zeitlos slots.

COMMON = ../../common
GAMES = $(COMMON)/games

include $(COMMON)/arch.mk

# Objects, and why each is linked:
#
#   zshape.o: The handle's knob is a filled circle, which lives in zshape.c. The
#   host render linked it explicitly and the target link did not, so this
#   only surfaced as an undefined reference at the very end -- which is
#   what a target link is for.
OBJS = zeitlos.o zobj.o zgfx.o zshape.o zfont_data.o zwin.o zrng.o zfsapp.o \
	zrand.o zrand_sys.o zbank.o \
	sl_reels.o sl_game.o sl_art.o sl_board.o sl_input.o slots.o

zrand.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zrand.c -o zrand.o
zrand_sys.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zrand_sys.c -o zrand_sys.o
zbank.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zbank.c -o zbank.o

# A .zexe rather than a raw --pad-to binary: the header carries .bss as
# a NUMBER instead of writing it out as literal zeros to be read off the
# SD card on every launch. See sw/common/zexec.h.

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
PANELSRC = $(COMMON)/zwin.c $(COMMON)/zwidget.c $(COMMON)/tests/zspeak_stub.c $(COMMON)/zfont_data.c \
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

.PHONY: test includecheck nofloat nmcheck art render

include ../../common/app.mk
