
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS. A command-line assignment replaces the makefile's CFLAGS
# outright -- `make CFLAGS+=-DFOO=1` appends to the command-line value,
# not to this one -- silently dropping -Os, -march, section GC, the
# libc specs and any -D this app needs to build correctly. The result
# still builds and links; it just produces a much larger, slower binary
# that can overrun the space the loader has for it and crash on start.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zport.o portdemo.o

portdemo: portdemo.elf portdemo.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zport.o:
	$(CC) $(CFLAGS) -c ../../common/zport.c -o zport.o

portdemo.o:
	$(CC) $(CFLAGS) -c portdemo.c -S -o portdemo.asm
	$(CC) $(CFLAGS) -c portdemo.c -o portdemo.o

portdemo.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,portdemo.map -o portdemo.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble portdemo.elf > portdemo.dasm

portdemo.bin: portdemo.elf
	@EDATA=$$($(PREFIX)nm portdemo.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm portdemo.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary portdemo.elf portdemo.data; \
	python3 ../../../tools/mkexec.py portdemo.data portdemo.bin $$(($$END - $$EDATA)); \
	rm -f portdemo.data

clean:
	rm -f portdemo.elf portdemo.bin portdemo.data portdemo.asm portdemo.dasm portdemo.map *.o *.d

.PHONY: portdemo clean

# Header dependency tracking.
#
# CFLAGS has carried -MD all along, so gcc has been writing .d files
# listing every header each object depends on -- but nothing included
# them, and the object rules below have no prerequisites at all
# (`wm.o:` rather than `wm.o: wm.c`). The effect is that once an object
# file exists, make considers it permanently up to date: editing a .c
# or a .h rebuilds nothing, and the link silently produces a binary
# from stale objects.
#
# That is not a theoretical problem -- it has twice produced hardware
# that behaved like a broken feature when the real cause was simply
# that the change was never compiled in. Including the .d files makes
# every object depend on its own source and headers, so `make` alone is
# correct and `make clean` stops being a required ritual.
-include $(wildcard *.d)
-include $(wildcard */*.d)
-include $(wildcard */*/*.d)
