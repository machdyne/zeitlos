# Zeitlos -- shared code for the games in sw/apps.
#
# There is no target build here. This directory is a set of .c files
# that apps compile into themselves, the same way they already do with
# ../zwin.c and ../zgfx.c -- see sw/apps/roulette/Makefile for how they
# are picked up. What this Makefile carries is the HOST TESTS, because
# shared code with no tests of its own becomes code nobody dares change.
#
# Anything in here must be useful to more than one app. The card
# encoding, the card art and the shoe arrived when blackjack became
# their second caller -- they were sw/apps/poker's until then, because
# promoting code for a caller that does not exist is how a shared
# directory turns into a junk drawer.
#
# What moved was the ENCODING, the ART and the SHOE. pk_eval.c stayed
# in poker: blackjack totals a hand, it does not rank one, and a poker
# hand evaluator here would be a shared file with one user.

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -no-pie -O2

# -DZBANK_NO_FS leaves out the three filesystem wrappers, which need
# syscalls no build machine has. What remains is the parser, the
# formatter and the arithmetic -- which is where anything can be wrong.
test:
	$(HOSTCC) $(HOSTFLAGS) -DZBANK_NO_FS -o /tmp/zgames_test \
		tests/games_test.c zrand.c zbank.c zcard.c zdeck.c zdice.c
	/tmp/zgames_test

# Every source an app will compile, checked with the host compiler and
# the header search the target build actually has. zrand_sys.c and the
# filesystem half of zbank.c reach into ../ for Zeitlos headers, which
# resolve relatively -- so no -I, exactly as an app gets.
includecheck:
	@$(HOSTCC) -std=gnu99 -Wall -fsyntax-only zrand.c || exit 1
	@$(HOSTCC) -std=gnu99 -Wall -fsyntax-only zrand_sys.c || exit 1
	@$(HOSTCC) -std=gnu99 -Wall -fsyntax-only zbank.c || exit 1
	@$(HOSTCC) -std=gnu99 -Wall -fsyntax-only zcard.c || exit 1
	@$(HOSTCC) -std=gnu99 -Wall -fsyntax-only zdeck.c || exit 1
	@$(HOSTCC) -std=gnu99 -Wall -fsyntax-only zcardart.c || exit 1
	@$(HOSTCC) -std=gnu99 -Wall -fsyntax-only zdice.c || exit 1
	@echo "includecheck: every games source resolves its headers with no -I"

# The card and dice tiles are generated and COMMITTED, so these must
# reproduce them byte for byte. gen_cards.py drifted from its own output
# once; check with `git diff` after running either.
art:
	python3 gen_cards.py
	python3 gen_dice.py

clean:
	rm -f *.o *.d

.PHONY: test includecheck art clean
