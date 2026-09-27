# sw/apps/calc -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = calc

# calc_core.o is the entry state machine, kept free of any I/O so the
# test suite below can build it for the host -- see calc_test.c.
# zfix.o is the arithmetic itself, shared with sw/apps/sheet
# (sw/common/zfix.c).
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfix.o calc_core.o calc.o

calc_core.o: calc_core.c calc_core.h
	$(CC) $(CFLAGS) -c calc_core.c -o calc_core.o

# Host build of the arithmetic, with the host's own compiler -- not
# $(CC), which is the cross compiler and produces something this
# machine cannot run.
#
#   make test
#
# Worth running after ANY change to calc_core.c. A calculator that is
# subtly wrong is worse than no calculator, because nobody checks its
# answers -- which is why the arithmetic is in a file with no I/O in
# it at all.
test:
	cc --std=gnu99 -Wall -Wextra -I ../../common \
		-o calc_test calc_test.c calc_core.c ../../common/zfix.c
	./calc_test

.PHONY: test

CLEAN_EXTRA = calc_test

include ../../common/app.mk
