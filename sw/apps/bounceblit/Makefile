
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI)  $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = bounceblit.o

bounceblit: bounceblit.elf bounceblit.bin

bounceblit.o:
	$(CC) $(CFLAGS) -c bounceblit.c -S -o bounceblit.asm
	$(CC) $(CFLAGS) -c bounceblit.c -o bounceblit.o

bounceblit.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,bounceblit.map -o bounceblit.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble bounceblit.elf > bounceblit.dasm

bounceblit.bin: bounceblit.elf
	@EDATA=$$($(PREFIX)nm bounceblit.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm bounceblit.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary bounceblit.elf bounceblit.data; \
	python3 ../../../tools/mkexec.py bounceblit.data bounceblit.bin $$(($$END - $$EDATA)); \
	rm -f bounceblit.data

clean:
	rm -f bounceblit.elf bounceblit.bin bounceblit.data bounceblit.asm bounceblit.dasm bounceblit.map bounceblit.o *.o *.d

.PHONY: bounceblit clean

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
