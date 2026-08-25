
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI)  $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = bounce.o

bounce: bounce.elf bounce.bin

bounce.o:
	$(CC) $(CFLAGS) -c bounce.c -S -o bounce.asm
	$(CC) $(CFLAGS) -c bounce.c -o bounce.o

bounce.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,bounce.map -o bounce.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble bounce.elf > bounce.dasm

bounce.bin: bounce.elf
	@EDATA=$$($(PREFIX)nm bounce.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm bounce.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary bounce.elf bounce.data; \
	python3 ../../../tools/mkexec.py bounce.data bounce.bin $$(($$END - $$EDATA)); \
	rm -f bounce.data

clean:
	rm -f bounce.elf bounce.bin bounce.data bounce.asm bounce.dasm bounce.map bounce.o *.o *.d

.PHONY: bounce clean

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
