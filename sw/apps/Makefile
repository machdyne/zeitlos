# One list, two rules.
#
# This used to be two hand-maintained lists of `cd <dir> && make` lines
# that had to agree with each other and with what is actually on disk.
# They had stopped agreeing: both named `ping` and `pong`, neither of
# which exists any more, so plain `make` in this directory failed at the
# third line and `make apps` from the top level failed with it. That is
# survivable when you build one app at a time by hand; it stops any
# automated build dead, which is how it was found.
#
# APPS is now the single list. Adding an app means adding one word.
APPS = \
	gpudemo \
	gpu3d \
	wm \
	hello_win \
	draw \
	view \
	text \
	hex \
	files \
	info \
	settings \
	calc \
	clock \
	read \
	net \
	term \
	logic \
	mmod \
	portdemo \
	serial \
	repl \
	space3d \
	audiotest \
	play \
	track \
	midi \
	gamedemo

all:
	@for a in $(APPS); do \
		echo "==> $$a"; \
		$(MAKE) -C $$a || exit 1; \
	done

clean:
	@for a in $(APPS); do \
		$(MAKE) -C $$a clean || exit 1; \
	done

# `|| exit 1` on both loops, deliberately. A bare `for ... done` in a
# recipe returns the status of the LAST iteration, so without it a
# failure in any app but the final one is swallowed and make reports
# success -- which for a release build means shipping an archive built
# around a stale or missing .bin.

.PHONY: all clean
