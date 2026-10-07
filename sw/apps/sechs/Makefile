# sw/apps/sechs -- Sechs modules on a PMOD, from the posix shell
# (docs/sechs_app.md). Build rules are shared: sw/common/app.mk,
# docs/build.md.
#
#   make            build sechs.elf and sechs.bin
#   make test       host tests: every command against a simulated module.
#                   No toolchain or board needed.
#
# The protocol is the Sechs master library of Machdyne BASIC (a submodule)
# (sw/ext/basic/tools/sechs/sechsm.c): the same code as Linux's sechsctl.

APP = sechs
APP_MAIN_RULE = 0

SECHSM = ../../ext/basic/tools/sechs

OBJS = zeitlos.o zobj.o zport.o zwin.o zgfx.o zfont_data.o zgpio.o zi2c.o zi2cx.o zfsapp.o \
	sechsm.o sechs_cmd.o sechs_app.o

sechsm.o: $(SECHSM)/sechsm.c $(SECHSM)/sechsm.h
	$(CC) $(CFLAGS) -c $(SECHSM)/sechsm.c -o sechsm.o

sechs_cmd.o: sechs_cmd.c sechs_cmd.h $(SECHSM)/sechsm.h
	$(CC) $(CFLAGS) -c sechs_cmd.c -o sechs_cmd.o

sechs_app.o: sechs_app.c sechs_cmd.h
	$(CC) $(CFLAGS) -c sechs_app.c -o sechs_app.o

# -- host tests ------------------------------------------------------

HOSTCC ?= cc
HOSTFLAGS = -std=gnu99 -Wall -Wextra -O1 -g

BIN_TEST = /tmp/sechs_test

$(BIN_TEST): tests/test_sechs.c sechs_cmd.c sechs_cmd.h $(SECHSM)/sechsm.c $(SECHSM)/sechsm.h
	$(HOSTCC) $(HOSTFLAGS) -o $@ tests/test_sechs.c sechs_cmd.c $(SECHSM)/sechsm.c

test: $(BIN_TEST)
	@$(BIN_TEST)

.PHONY: test

CLEAN_EXTRA = $(BIN_TEST)

include ../../common/app.mk
