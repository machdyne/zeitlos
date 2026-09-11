# posix -- a Unix-shaped shell for Zeitlos, as a port provider.
#
# Phase 4 of docs/posix.md. Modelled on ../repl/Makefile, which is the
# reference for this shape, and far shorter than it: there is no
# Scheme, no editor and no pager here.

PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc

GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS -- same reasoning as every other app Makefile here, and the
# same failure if it is dropped: a binary that still links and is much
# larger than the loader has room for.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	$(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS) -I../../common

# -Werror applies to THIS APP'S OWN sources and nothing else.
#
# It was on CFLAGS at first, which also compiles the sw/common objects
# below -- and those have warnings that vary by toolchain version
# (zeitlos.c's _exit() is `noreturn` and does return; _read() has a
# maybe-uninitialized that some GCCs see and others do not). An app
# has no business failing the build over warnings in code it does not
# own, and making every app in the tree warning-clean on every
# toolchain is a different job from writing this one.
#
# So: shared code builds with warnings visible, this app's code builds
# with them fatal. The reason -Werror is here at all is on record in
# docs/zcc.md -- zcc shipped for several days with three implicit
# declarations nobody read, and a newer GCC turned them into errors.
APP_CFLAGS = $(CFLAGS) -Werror
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zport.o zline.o zfsapp.o zwin.o zgfx.o zfont_data.o \
	zconnect.o \
	vfs.o sh.o main.o

posix: posix.elf posix.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o
zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o
zport.o:
	$(CC) $(CFLAGS) -c ../../common/zport.c -o zport.o
zline.o:
	$(CC) $(CFLAGS) -c ../../common/zline.c -o zline.o
# For z_conn_handoff() -- the Z_TERM_SET_PORT message that hands a term
# window to a child and takes it back. Same helper `repl` uses for
# `telnet`; see posix.h's PX_TTY_TAG.
zconnect.o:
	$(CC) $(CFLAGS) -c ../../common/zconnect.c -o zconnect.o
zfsapp.o:
	$(CC) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o
# zwin is here for z_launch_arg_set() alone -- the mechanism the `run`
# builtin uses to hand a command line to the program it starts. It
# drags in zgfx/zfont_data, which --gc-sections then drops almost all
# of; without section GC this app is several KB larger for one call.
zwin.o:
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o
zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o
zfont_data.o:
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

vfs.o: vfs.c posix.h
	$(CC) $(APP_CFLAGS) -c vfs.c -o vfs.o
sh.o: sh.c posix.h
	$(CC) $(APP_CFLAGS) -c sh.c -o sh.o
main.o: main.c posix.h
	$(CC) $(APP_CFLAGS) -c main.c -o main.o

posix.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,posix.map -o posix.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble posix.elf > posix.dasm

posix.bin: posix.elf
	@EDATA=$$($(PREFIX)nm posix.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm posix.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary posix.elf posix.data; \
	python3 ../../../tools/mkexec.py posix.data posix.bin $$(($$END - $$EDATA)); \
	rm -f posix.data

# The shell's own test suite, built with the HOST compiler.
#
#   make test
#
# sh.c and vfs.c contain no port, message or window code at all -- that
# is main.c's whole job -- so they can be driven directly against a
# host filesystem. See tests/run.sh for why that split was worth
# arranging rather than testing this only on hardware.
test:
	@$(MAKE) -C tests run

clean:
	rm -f posix.elf posix.bin posix.data posix.map posix.dasm *.o *.d
	@$(MAKE) -C tests clean

.PHONY: posix test clean
-include $(wildcard *.d)
