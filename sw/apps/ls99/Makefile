# sw/apps/ls99 -- a virtual Zwölf module, started by bench for each
# `module` in its netlist (docs/ls99.md). Build rules are shared:
# sw/common/app.mk, docs/build.md.
#
#   make            build ls99.elf and ls99.bin
#   make test       end to end on a host: the module (with the real
#                   interpreter and Sechs core) on bench's own core --
#                   the grow light over 50 virtual hours, a panel on a
#                   TCA9535, its Sechs side. No toolchain or board.
#
# One interpreter plays every module: built with the largest program
# area and pin count, told at start which machine it is (BASIC_PROFILE).

APP = ls99
APP_MAIN_RULE = 0

E = ../../ext/basic
B = ../bench
# WAIT and SLEEP in pieces of a minute, not 100ms: at x3600 an hour was
# 36,000 pieces, more than the CPU could do. A halt still stops a piece
# at once (ls99_app.c's sleep returns on HALT or Ctrl-C).
BASIC_DEFS = -DBASIC_PROFILE -DHW_PINS=7 -DMAX_PROG=32768 -DBASIC_WAIT_PIECE=60000

OBJS = zeitlos.o zobj.o zport.o zwin.o zgfx.o zfont_data.o zfsapp.o zgpio.o zi2c.o zi2cx.o \
	basic_interp.o sechs_core.o ls99.o ls99_app.o

basic_interp.o: $(E)/basic.c $(E)/basic.h
	$(CC) $(CFLAGS) $(BASIC_DEFS) -c $(E)/basic.c -o basic_interp.o
sechs_core.o: $(E)/sechs/sechs.c $(E)/sechs/sechs.h
	$(CC) $(CFLAGS) -c $(E)/sechs/sechs.c -o sechs_core.o
ls99.o: ls99.c ls99.h $(E)/basic.h
	$(CC) $(CFLAGS) $(BASIC_DEFS) -c ls99.c -o ls99.o
ls99_app.o: ls99_app.c ls99.h ../../common/zbench.h
	$(CC) $(CFLAGS) $(BASIC_DEFS) -c ls99_app.c -o ls99_app.o

# -- host tests ------------------------------------------------------

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -O1 -g $(BASIC_DEFS)
BENCH_CORE = $(B)/core.c $(B)/netlist.c $(B)/parts/parts.c $(B)/parts/tca9535.c \
	$(B)/parts/simple.c $(B)/parts/module.c $(B)/parts/gpio.c

BIN_TEST = /tmp/ls99_test

$(BIN_TEST): tests/test_ls99.c ls99.c ls99.h $(E)/basic.c $(E)/sechs/sechs.c $(BENCH_CORE)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_ls99.c ls99.c $(E)/basic.c $(E)/sechs/sechs.c $(BENCH_CORE)

test: $(BIN_TEST)
	@$(BIN_TEST)

.PHONY: test

CLEAN_EXTRA = $(BIN_TEST)

include ../../common/app.mk
