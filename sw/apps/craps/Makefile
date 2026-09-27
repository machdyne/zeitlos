# sw/apps/craps -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = craps
APP_LISTING = 0
# Zeitlos craps.

COMMON = ../../common
GAMES = $(COMMON)/games

include $(COMMON)/arch.mk

OBJS = zeitlos.o zobj.o zgfx.o zshape.o zfont_data.o zwin.o zrng.o zfsapp.o \
	zrand.o zrand_sys.o zbank.o zdice.o \
	cr_table.o cr_game.o cr_board.o cr_input.o craps.o

zrand.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zrand.c -o zrand.o
zrand_sys.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zrand_sys.c -o zrand_sys.o
zbank.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zbank.c -o zbank.o
zdice.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zdice.c -o zdice.o

# A .zexe rather than a raw --pad-to binary: the header carries .bss as
# a NUMBER instead of writing it out as literal zeros. See zexec.h.

# Scoped to this app's own objects: sw/common/zobj.c has a float in
# z_obj_t and pulls __subsf3 into every app in this tree.
APPOBJS = cr_table.o cr_game.o cr_board.o cr_input.o craps.o \
	zshape.o zrand.o zbank.o zdice.o

nmcheck:
	@if $(PREFIX)nm $(APPOBJS) | \
		grep -E 'U __(add|sub|mul|div|fix|float|cmp|unord)[ds]f'; then \
		echo "nmcheck: FAILED -- soft-float reached this app's objects"; \
		exit 1; \
	fi
	@echo "nmcheck: no soft-float in this app's target objects"
	@$(PREFIX)size craps.elf 2>/dev/null || true

# -- host-side tests --

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -no-pie -O2

ENGINESRC = cr_table.c cr_game.c
TARGETSRC = cr_table.c cr_game.c cr_board.c cr_input.c craps.c

# The software pixel primitives, so a test can actually DRAW the felt.
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
# literals because the bets have names.
nofloat:
	@for f in $(TARGETSRC) *.h; do \
		if $(HOSTCC) -fpreprocessed -dD -E $$f 2>/dev/null | \
			sed 's/"[^"]*"//g' | \
			grep -nE '(^|[^_[:alnum:]])(float|double)([^_[:alnum:]]|$$)'; then \
			echo "nofloat: FAILED -- floating point in $$f"; exit 1; \
		fi; \
	done
	@echo "nofloat: the table is integer-only"

test: includecheck nofloat
	$(MAKE) -C $(GAMES) test
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/craps_edge tests/edge_test.c $(ENGINESRC)
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/craps_game tests/game_test.c \
		$(ENGINESRC) $(GAMES)/zrand.c
	$(HOSTCC) $(PANELFLAGS) -o /tmp/craps_board tests/board_test.c \
		cr_board.c $(ENGINESRC) $(GAMES)/zdice.c $(COMMON)/zshape.c \
		$(PANELSRC)
	/tmp/craps_edge
	/tmp/craps_game
	/tmp/craps_board

# LOOK AT THE OUTPUT before changing the felt.
#
#   make render WHAT=comeout|point|rolling
#
# Two things a render caught and no assertion would have: a third of the
# window empty below the proposition strip, and the free odds having no
# spot on the felt at all.
WHAT ?= point

render:
	$(HOSTCC) $(PANELFLAGS) -o /tmp/craps_render tests/render.c \
		cr_board.c $(ENGINESRC) $(GAMES)/zdice.c $(COMMON)/zshape.c \
		$(PANELSRC)
	/tmp/craps_render /tmp/craps.pbm $(WHAT)

.PHONY: test includecheck nofloat nmcheck render

include ../../common/app.mk
