PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI)  $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = gpublit.o

gpublit: gpublit.elf gpublit.bin

gpublit.o:
	$(CC) $(CFLAGS) -c gpublit.c -S -o gpublit.asm
	$(CC) $(CFLAGS) -c gpublit.c -o gpublit.o

gpublit.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,gpublit.map -o gpublit.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble gpublit.elf > gpublit.dasm

gpublit.bin:
	$(PREFIX)objcopy -O binary gpublit.elf gpublit.bin

clean:
	rm -f gpublit.elf gpublit.bin gpublit.asm gpublit.dasm gpublit.map gpublit.o *.o *.d

.PHONY: gpublit clean

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
