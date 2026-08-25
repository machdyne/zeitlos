
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) $(ARCH_DEFS) $(LIBC_FLAGS)
#CFLAGS = -fPIC --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(LIBC_FLAGS) #-fPIC -nostartfiles
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o hello.o
#OBJS = crt0.o zeitlos.o hello.o

hello: hello.elf hello.bin

crt0.o:
	$(AS) $(ASFLAGS) -o crt0.o ../../common/crt0.S

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

hello.o:
	$(CC) $(CFLAGS) -c hello.c -S -o hello.asm
	$(CC) $(CFLAGS) -c hello.c -o hello.o

hello.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,hello.map -o hello.elf $(OBJS) 
	$(PREFIX)objdump -S --disassemble hello.elf > hello.dasm

hello.bin: hello.elf
	@EDATA=$$($(PREFIX)nm hello.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm hello.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary hello.elf hello.data; \
	python3 ../../../tools/mkexec.py hello.data hello.bin $$(($$END - $$EDATA)); \
	rm -f hello.data

clean:
	rm -f hello.elf hello.bin hello.data hello.asm hello.dasm hello.map *.o *.d

.PHONY: hello clean

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
