
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -DZ_GFX_HW_BLIT $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zsoc.o audiotest.o

audiotest: audiotest.elf audiotest.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zsoc.o:
	$(CC) $(CFLAGS) -c ../../common/zsoc.c -o zsoc.o

audiotest.o:
	$(CC) $(CFLAGS) -c audiotest.c -S -o audiotest.asm
	$(CC) $(CFLAGS) -c audiotest.c -o audiotest.o

audiotest.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,audiotest.map -o audiotest.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble audiotest.elf > audiotest.dasm

audiotest.bin: audiotest.elf
	@EDATA=$$($(PREFIX)nm audiotest.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm audiotest.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary audiotest.elf audiotest.data; \
	python3 ../../../tools/mkexec.py audiotest.data audiotest.bin $$(($$END - $$EDATA)); \
	rm -f audiotest.data

clean:
	rm -f audiotest.elf audiotest.bin audiotest.data audiotest.asm audiotest.dasm audiotest.map *.o *.d

.PHONY: audiotest clean

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
