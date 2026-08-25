
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) $(ARCH_DEFS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = blinky.o

blinky: blinky.elf blinky.bin

blinky.o:
	$(CC) $(CFLAGS) -c blinky.c -S -o blinky.asm
	$(CC) $(CFLAGS) -c blinky.c -o blinky.o

blinky.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,blinky.map -o blinky.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble blinky.elf > blinky.dasm

blinky.bin:
	$(PREFIX)objcopy -O binary blinky.elf blinky.bin

clean:
	rm -f blinky.elf blinky.bin blinky.asm blinky.dasm blinky.map blinky.o *.o *.d

.PHONY: blinky clean

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
