
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
# See sw/apps/automate/Makefile for why these are `override`, and why
# section GC matters.
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

COMMON = zeitlos zobj zgfx zfont_data zwin zkbd zfsapp zcaption
OBJS = $(addsuffix .o,$(COMMON)) automate.o

automate: automate.elf automate.bin

$(addsuffix .o,$(COMMON)): %.o: ../../common/%.c
	$(CC) $(CFLAGS) -c $< -o $@

automate.o: automate.c
	$(CC) $(CFLAGS) -c automate.c -o automate.o

automate.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,automate.map -o automate.elf $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,automate.map -o automate.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble automate.elf > automate.dasm

automate.bin: automate.elf
	@EDATA=$$($(PREFIX)nm automate.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm automate.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary automate.elf automate.data; \
	python3 ../../../tools/mkexec.py automate.data automate.bin $$(($$END - $$EDATA)); \
	rm -f automate.data

clean:
	rm -f automate.elf automate.bin automate.data automate.asm automate.dasm automate.map *.o *.d

.PHONY: automate clean

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
