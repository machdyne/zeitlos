# zcc, the Zeitlos C compiler, built as an ORDINARY Zeitlos app.
#
# Same shape as every other app here: sw/common's runtime, newlib, and
# sw/common/riscv-app.ld with newlib's crt0 as the entry point.
#
# It was not always. The first version had its own linker script and a
# hand-written _start, linking libz instead of the C library -- not a
# design decision, but an artefact of a development machine whose
# RISC-V toolchain had no newlib and so could not link a normal app at
# all. Every assumption that entry stub made was untested and two of
# them crashed a real system. See port_dev.c's header.
#
# libz is still built, and zcc still needs it -- but as DATA. libz.bin
# is the runtime zcc embeds into the programs it produces, read from
# the card at run time and not linked into zcc. `make` here builds it
# so that a whole-tree build produces both.

PREFIX ?= $(RISCV_PREFIX)
include ../../common/arch.mk
CC = $(PREFIX)gcc

GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	$(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS) -I../../common -I.
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# -Werror on THIS APP'S OWN sources only. The sw/common objects below
# have warnings that vary by toolchain version (zeitlos.c's _exit() is
# `noreturn` and does return), and an app has no business failing the
# build over code it does not own. The reason -Werror is here at all
# is in docs/zcc.md -- zcc shipped for days with three implicit
# declarations nobody read.
APP_CFLAGS = $(CFLAGS) -Werror

OBJS = zeitlos.o zobj.o zport.o zfsapp.o zwin.o zgfx.o zfont_data.o \
	main.o lex.o cpp.o parse.o emit.o util.o port_dev.o

zcc: zcc.elf zcc.bin libz.data

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o
zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o
zport.o:
	$(CC) $(CFLAGS) -c ../../common/zport.c -o zport.o
zfsapp.o:
	$(CC) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o
# zwin for z_launch_arg_take() alone -- how `posix` hands zcc a command
# line. It pulls in zgfx/zfont_data, which --gc-sections then drops
# almost all of.
zwin.o:
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o
zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o
zfont_data.o:
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

main.o: main.c zcc.h zcc_port.h
	$(CC) $(APP_CFLAGS) -c main.c -o main.o
lex.o: lex.c zcc.h zcc_port.h
	$(CC) $(APP_CFLAGS) -c lex.c -o lex.o
cpp.o: cpp.c zcc.h zcc_port.h
	$(CC) $(APP_CFLAGS) -c cpp.c -o cpp.o
parse.o: parse.c zcc.h zcc_port.h
	$(CC) $(APP_CFLAGS) -c parse.c -o parse.o
emit.o: emit.c zcc.h zcc_port.h
	$(CC) $(APP_CFLAGS) -c emit.c -o emit.o
util.o: util.c zcc.h zcc_port.h
	$(CC) $(APP_CFLAGS) -c util.c -o util.o
port_dev.o: port_dev.c zcc.h zcc_port.h
	$(CC) $(APP_CFLAGS) -DZCC_HOSTED=0 -c port_dev.c -o port_dev.o

zcc.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,zcc.map -o zcc.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble zcc.elf > zcc.dasm

zcc.bin: zcc.elf
	@EDATA=$$($(PREFIX)nm zcc.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm zcc.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary zcc.elf zcc.data; \
	python3 ../../../tools/mkexec.py zcc.data zcc.bin $$(($$END - $$EDATA)); \
	rm -f zcc.data

# The runtime zcc embeds into what it builds. A separate artefact with
# its own Makefile, not linked here -- see the header comment.
libz.data:
	$(MAKE) -C libz

clean:
	rm -f zcc.elf zcc.bin zcc.data zcc.map zcc.dasm *.o *.d
	$(MAKE) -C libz clean
	rm -rf build

.PHONY: zcc libz.data clean
-include $(wildcard *.d)
