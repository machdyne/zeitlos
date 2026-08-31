
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
# See sw/apps/repl/Makefile for the full reasoning behind these two --
# in short, sw/common is shared by every app, so each links whole
# objects for the sake of a fraction of them, and section GC drops the
# rest. Measured saving on this app is noted in docs/scheme_api.md's
# memory-budget section. Set GC_SECTIONS=0 to disable.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS. A command-line assignment replaces the makefile's CFLAGS
# outright -- `make CFLAGS+=-DFOO=1` appends to the command-line value,
# not to this one -- silently dropping -Os, -march, section GC, the
# libc specs and any -D this app needs to build correctly. The result
# still builds and links; it just produces a much larger, slower binary
# that can overrun the space the loader has for it and crash on start.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zkbd.o dock_icons.o win_icons.o wm.o

# built with -DZ_GFX_HW_BLIT, same as hello_win/term -- the dock's
# icon labels (see wm.c's draw_dock()) go through the hardware glyph
# blitter. wm is now the ONLY process on the board that ever calls
# z_gfx_hw_font_load() (once, in main(), for z_font_5x8 -- see that
# call's own comment) -- hello_win/term no longer load a font
# themselves, and every app is expected to only ever draw with
# z_font_5x8 as a result. This sidesteps the cross-process glyph
# memory race entirely (see docs/window_manager.md, "Hardware glyph
# blitting") rather than just narrowing it: there's nothing left to
# race over once only one process ever writes to glyph memory, and
# only one font's data is ever loaded, board-wide.

wm: wm.elf wm.bin

zeitlos.o:
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o:
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zgfx.o:
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zfont_data.o:
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

zkbd.o:
	$(CC) $(CFLAGS) -c ../../common/zkbd.c -o zkbd.o

# dock_icons.c/.h are generated -- see sw/data/icons/gen_dock_icon_data.py.
# not regenerated automatically here (this Makefile doesn't depend on
# Python/Pillow being available) -- run that script by hand after
# adding or changing an icon PNG, same as font/logo data (see
# dock_icons.h's own header comment).
dock_icons.o: dock_icons.c dock_icons.h
	$(CC) $(CFLAGS) -c dock_icons.c -o dock_icons.o

# win_icons.c/.h are hand-edited (binary literals), not generated --
# see win_icons.h's own header comment. Titlebar close icon and any
# future window icon (minimize, open file, save file, ...) live here.
win_icons.o: win_icons.c win_icons.h
	$(CC) $(CFLAGS) -c win_icons.c -o win_icons.o

wm.o:
	$(CC) $(CFLAGS) -c wm.c -S -o wm.asm
	$(CC) $(CFLAGS) -c wm.c -o wm.o

wm.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,wm.map -o wm.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble wm.elf > wm.dasm

wm.bin: wm.elf
	@EDATA=$$($(PREFIX)nm wm.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm wm.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary wm.elf wm.data; \
	python3 ../../../tools/mkexec.py wm.data wm.bin $$(($$END - $$EDATA)); \
	rm -f wm.data

clean:
	rm -f wm.elf wm.bin wm.data wm.asm wm.dasm wm.map *.o *.d

.PHONY: wm clean

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
