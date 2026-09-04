
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
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o \
	zsoc.o zgpio.o zspi.o zmmod.o zfsapp.o zflist.o zdialog.o mmod.o

mmod: mmod.elf mmod.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

# panel.c is INCLUDED by mmod.c rather than compiled separately, so
# that sw/apps/mmod/tests/render.c can compile the panel without the
# event loop and draw it on the build machine. See that file.
zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zfont_data.o:
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

zwin.o:
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o

zwidget.o:
	$(CC) $(CFLAGS) -c ../../common/zwidget.c -o zwidget.o

# The bit-bang SPI stack. No zport: this app talks to a chip, not to
# another process.
zsoc.o:
	$(CC) $(CFLAGS) -c ../../common/zsoc.c -o zsoc.o

zgpio.o:
	$(CC) $(CFLAGS) -c ../../common/zgpio.c -o zgpio.o

zspi.o:
	$(CC) $(CFLAGS) -c ../../common/zspi.c -o zspi.o

# The device layer -- tested by sw/common/tests/test_mmod.c.
zmmod.o:
	$(CC) $(CFLAGS) -c ../../common/zmmod.c -o zmmod.o

# File I/O for READ and VERIFY.
zfsapp.o:
	$(CC) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o

# The file dialogs. zflist.o needs zfsapp.o; zdialog.o needs both.
zflist.o:
	$(CC) $(CFLAGS) -c ../../common/zflist.c -o zflist.o

zdialog.o:
	$(CC) $(CFLAGS) -c ../../common/zdialog.c -o zdialog.o

mmod.o: mmod.c panel.c
	$(CC) $(CFLAGS) -c mmod.c -o mmod.o
	$(CC) $(CFLAGS) -c mmod.c -o mmod.o

mmod.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,mmod.map -o mmod.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble mmod.elf > mmod.dasm

mmod.bin: mmod.elf
	@EDATA=$$($(PREFIX)nm mmod.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm mmod.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary mmod.elf mmod.data; \
	python3 ../../../tools/mkexec.py mmod.data mmod.bin $$(($$END - $$EDATA)); \
	rm -f mmod.data

clean:
	rm -f mmod.elf mmod.bin mmod.data mmod.asm mmod.dasm mmod.map *.o *.d

.PHONY: mmod clean

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
