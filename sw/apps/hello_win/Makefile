
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
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -DZ_GFX_HW_BLIT $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o hello_win.o

hello_win: hello_win.elf hello_win.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zfont_data.o:
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

zwin.o:
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o

hello_win.o:
	$(CC) $(CFLAGS) -c hello_win.c -S -o hello_win.asm
	$(CC) $(CFLAGS) -c hello_win.c -o hello_win.o

hello_win.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,hello_win.map -o hello_win.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble hello_win.elf > hello_win.dasm

hello_win.bin: hello_win.elf
	@EDATA=$$($(PREFIX)nm hello_win.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm hello_win.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary hello_win.elf hello_win.data; \
	python3 ../../../tools/mkexec.py hello_win.data hello_win.bin $$(($$END - $$EDATA)); \
	rm -f hello_win.data

clean:
	rm -f hello_win.elf hello_win.bin hello_win.data hello_win.asm hello_win.dasm hello_win.map *.o *.d

.PHONY: hello_win clean

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
