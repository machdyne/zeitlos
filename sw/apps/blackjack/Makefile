# sw/apps/blackjack -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = blackjack
APP_LISTING = 0
# Zeitlos blackjack.

COMMON = ../../common
GAMES = $(COMMON)/games

include $(COMMON)/arch.mk

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zrng.o zfsapp.o \
	zcard.o zdeck.o zrand.o zrand_sys.o zbank.o zcardart.o \
	bj_game.o bj_hint.o bj_board.o bj_input.o blackjack.o

zcard.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zcard.c -o zcard.o
zdeck.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zdeck.c -o zdeck.o
zrand.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zrand.c -o zrand.o
zrand_sys.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zrand_sys.c -o zrand_sys.o
zbank.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zbank.c -o zbank.o
zcardart.o:
	$(CC) $(CFLAGS) -c $(GAMES)/zcardart.c -o zcardart.o

# A .zexe rather than a raw --pad-to binary: the header carries .bss as
# a NUMBER instead of writing it out as literal zeros to be read off the
# SD card on every launch. See sw/common/zexec.h.

# -- host-side tests --

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -no-pie -O2

# Cards, the shoe and the shuffle come from sw/common/games. They were
# sw/apps/poker's until this app became their second caller, which is
# the point at which the shape of a shared thing is known rather than
# guessed. See docs/casino_bank.md.
ENGINESRC = bj_game.c bj_hint.c $(GAMES)/zcard.c $(GAMES)/zdeck.c $(GAMES)/zrand.c

TARGETSRC = bj_game.c bj_hint.c bj_board.c bj_input.c blackjack.c

includecheck:
	@for f in $(TARGETSRC) *.h; do \
		$(HOSTCC) -std=gnu99 -Wall -fsyntax-only $$f || exit 1; \
	done
	@echo "includecheck: every target source resolves its headers with no -I"

# Some boards here are rv32i with no FPU, where a single double links
# the soft-float runtime. Comments are stripped first, so prose about
# floating point does not read as evidence of it.
nofloat:
	@for f in $(TARGETSRC) *.h; do \
		if $(HOSTCC) -fpreprocessed -dD -E $$f 2>/dev/null | \
			sed 's/"[^"]*"//g' | \
			grep -nE '(^|[^_[:alnum:]])(float|double)([^_[:alnum:]]|$$)'; then \
			echo "nofloat: FAILED -- floating point in $$f"; exit 1; \
		fi; \
	done
	@echo "nofloat: the rules are integer-only"

# Runs the shared library's tests too. Blackjack shares its cards and
# its shoe with poker, so a change there that breaks them should fail
# here rather than on the device.
PANELSRC = $(COMMON)/zwin.c $(COMMON)/zwidget.c $(COMMON)/tests/zspeak_stub.c $(COMMON)/zfont_data.c \
	$(COMMON)/zobj.c $(COMMON)/zeitlos.c
PANELFLAGS = $(HOSTFLAGS) -I $(COMMON)

test: includecheck nofloat
	$(MAKE) -C $(GAMES) test
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/blackjack_rules tests/game_test.c $(ENGINESRC)
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/blackjack_hint tests/hint_test.c $(ENGINESRC)
	$(HOSTCC) $(PANELFLAGS) -o /tmp/blackjack_board tests/board_test.c \
		bj_board.c $(ENGINESRC) $(GAMES)/zcardart.c $(PANELSRC)
	/tmp/blackjack_rules
	/tmp/blackjack_hint
	/tmp/blackjack_board

# Every layer at once, under -fsanitize=address,undefined: deal, act
# through both the command line and the mouse, follow basic strategy
# half the time, redraw after every step, settle -- across every rule
# combination and one to eight decks, for 2,500 rounds.
#
# Separate from `test` because the sanitizers make it slow. Worth
# running after any change to the drawing or the parsing.
stress:
	$(HOSTCC) --std=gnu99 -g -O1 -no-pie -I $(COMMON) \
		-fsanitize=address,undefined -fno-omit-frame-pointer \
		-o /tmp/blackjack_stress tests/stress.c bj_input.c bj_board.c \
		$(ENGINESRC) $(GAMES)/zcardart.c $(PANELSRC)
	ASAN_OPTIONS=detect_leaks=0 /tmp/blackjack_stress

# Needs the RISC-V toolchain and a completed `make`. See
# sw/apps/roulette/Makefile for why this is scoped to this app's own
# objects rather than every object it links.
APPOBJS = bj_game.o bj_hint.o bj_board.o bj_input.o blackjack.o \
	zcard.o zdeck.o zrand.o zbank.o zcardart.o

nmcheck:
	@if $(PREFIX)nm $(APPOBJS) | \
		grep -E 'U __(add|sub|mul|div|fix|float|cmp|unord)[ds]f'; then \
		echo "nmcheck: FAILED -- soft-float reached this app's objects"; \
		exit 1; \
	fi
	@echo "nmcheck: no soft-float in this app's target objects"
	@$(PREFIX)size blackjack.elf 2>/dev/null || true

# LOOK AT THE OUTPUT before changing the table.
#
#   make render              a hand in play
#   make render WHAT=split   four hands, small cards
#   make render WHAT=done    the showdown, hole card up
WHAT ?= play

render:
	$(HOSTCC) $(PANELFLAGS) -o /tmp/blackjack_render tests/render.c \
		bj_board.c $(ENGINESRC) $(GAMES)/zcardart.c $(PANELSRC)
	/tmp/blackjack_render /tmp/blackjack.pbm $(WHAT)

.PHONY: test includecheck nofloat nmcheck render stress

include ../../common/app.mk
