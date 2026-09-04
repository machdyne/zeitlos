
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

OBJS = zeitlos.o zobj.o zport.o zsoc.o zuart.o serial.o

serial: serial.elf serial.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zport.o:
	$(CC) $(CFLAGS) -c ../../common/zport.c -o zport.o

# z_soc_has_feature2(), which z_uart1_present() is built on.
zsoc.o:
	$(CC) $(CFLAGS) -c ../../common/zsoc.c -o zsoc.o

zuart.o:
	$(CC) $(CFLAGS) -c ../../common/zuart.c -o zuart.o

serial.o:
	$(CC) $(CFLAGS) -c serial.c -S -o serial.asm
	$(CC) $(CFLAGS) -c serial.c -o serial.o

serial.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,serial.map -o serial.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble serial.elf > serial.dasm

serial.bin: serial.elf
	@EDATA=$$($(PREFIX)nm serial.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm serial.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary serial.elf serial.data; \
	python3 ../../../tools/mkexec.py serial.data serial.bin $$(($$END - $$EDATA)); \
	rm -f serial.data

clean:
	rm -f serial.elf serial.bin serial.data serial.asm serial.dasm serial.map *.o *.d

.PHONY: serial clean

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
