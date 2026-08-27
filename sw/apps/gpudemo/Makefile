PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI)  $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zwin.o gpudemo.o

gpudemo: gpudemo.elf gpudemo.bin

zeitlos.o: ../../common/zeitlos.c ../../common/zeitlos.h
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o: ../../common/zobj.c ../../common/zobj.h
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zgfx.o: ../../common/zgfx.c ../../common/zgfx.h
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zwin.o: ../../common/zwin.c ../../common/zwin.h
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o

gpudemo.o: gpudemo.c
	$(CC) $(CFLAGS) -c gpudemo.c -S -o gpudemo.asm
	$(CC) $(CFLAGS) -c gpudemo.c -o gpudemo.o

gpudemo.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,gpudemo.map -o gpudemo.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble gpudemo.elf > gpudemo.dasm

gpudemo.bin: gpudemo.elf
	@EDATA=$$($(PREFIX)nm gpudemo.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm gpudemo.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary gpudemo.elf gpudemo.data; \
	python3 ../../../tools/mkexec.py gpudemo.data gpudemo.bin $$(($$END - $$EDATA)); \
	rm -f gpudemo.data

clean:
	rm -f gpudemo.elf gpudemo.bin gpudemo.data gpudemo.asm gpudemo.dasm gpudemo.map *.o *.d

.PHONY: gpudemo clean

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
