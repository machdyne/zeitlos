# sw/apps/bench -- the bench: virtual parts, nets and I2C buses, from
# netlists, its examples in /data/bench/examples (docs/bench.md). Build rules are shared:
# sw/common/app.mk, docs/build.md.
#
#   make            build bench.elf and bench.bin
#   make test       host tests: netlists, nets, buses, each part against
#                   its datasheet, and the drawing. No toolchain or board.

APP = bench
APP_MAIN_RULE = 0

# The stack and the malloc heap share this (docs/executables.md). The
# canvas is .bss and costs none of it; the netlist is read into the
# heap, and 16KB less the stack left too little margin for a long one.
APP_STACK = 32K

CORE = core.c netlist.c draw.c parts/parts.c parts/tca9535.c parts/simple.c parts/module.c parts/gpio.c
CORE_H = core.h netlist.h draw.h

# zdialog.o is the open icon's file dialog: it needs zflist.o (the list),
# zedit.o (the name field) and zspeak.o (widgets announce themselves), as
# in sw/apps/view.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zflist.o zdialog.o zedit.o zfsapp.o zport.o zgpio.o zi2c.o \
	core.o netlist.o draw.o parts.o tca9535.o simple.o module.o gpio.o bench_app.o

core.o: core.c $(CORE_H)
	$(CC) $(CFLAGS) -c core.c -o core.o
netlist.o: netlist.c $(CORE_H)
	$(CC) $(CFLAGS) -c netlist.c -o netlist.o
draw.o: draw.c $(CORE_H)
	$(CC) $(CFLAGS) -c draw.c -o draw.o
parts.o: parts/parts.c core.h
	$(CC) $(CFLAGS) -c parts/parts.c -o parts.o
tca9535.o: parts/tca9535.c core.h
	$(CC) $(CFLAGS) -c parts/tca9535.c -o tca9535.o
simple.o: parts/simple.c core.h
	$(CC) $(CFLAGS) -c parts/simple.c -o simple.o
module.o: parts/module.c core.h
	$(CC) $(CFLAGS) -c parts/module.c -o module.o
gpio.o: parts/gpio.c core.h
	$(CC) $(CFLAGS) -c parts/gpio.c -o gpio.o
bench_app.o: bench_app.c $(CORE_H) ../../common/zbench.h
	$(CC) $(CFLAGS) -c bench_app.c -o bench_app.o

# -- host tests ------------------------------------------------------

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -O1 -g

BIN_TEST_CORE = /tmp/bench_test_core
BIN_TEST_DRAW = /tmp/bench_test_draw
BIN_TEST_PROTO = /tmp/bench_test_proto

$(BIN_TEST_CORE): tests/test_core.c $(CORE) $(CORE_H)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_core.c $(CORE) ../../common/zfont_data.c

$(BIN_TEST_DRAW): tests/test_draw.c $(CORE) $(CORE_H)
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_draw.c $(CORE) ../../common/zfont_data.c

$(BIN_TEST_PROTO): tests/test_proto.c ../../common/zbench.h
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_proto.c

test: $(BIN_TEST_CORE) $(BIN_TEST_DRAW) $(BIN_TEST_PROTO)
	@$(BIN_TEST_CORE)
	@$(BIN_TEST_DRAW)
	@$(BIN_TEST_PROTO)

.PHONY: test

CLEAN_EXTRA = $(BIN_TEST_CORE) $(BIN_TEST_DRAW) $(BIN_TEST_PROTO)

include ../../common/app.mk
