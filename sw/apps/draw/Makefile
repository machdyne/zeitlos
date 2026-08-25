
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI)  $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = draw.o

draw: draw.elf draw.bin

draw.o:
	$(CC) $(CFLAGS) -c draw.c -S -o draw.asm
	$(CC) $(CFLAGS) -c draw.c -o draw.o

draw.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,draw.map -o draw.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble draw.elf > draw.dasm

draw.bin:
	$(PREFIX)objcopy -O binary draw.elf draw.bin

clean:
	rm -f draw.elf draw.bin draw.asm draw.dasm draw.map draw.o *.o *.d

.PHONY: draw clean

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
