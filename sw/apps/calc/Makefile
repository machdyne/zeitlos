
PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)
# See sw/apps/repl/Makefile for the full reasoning -- in short,
# sw/common is shared by every app, so each links whole objects for the
# sake of a fraction of them, and section GC drops the rest. Set
# GC_SECTIONS=0 to disable.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# -DZ_GFX_HW_BLIT, same as wm/hello_win/term: the widget toolkit calcs
# its text labels through the hardware glyph blitter. calc does NOT
# call z_gfx_hw_font_load() itself -- wm is the only process on the
# board that ever loads glyph memory, and every app calcs with
# z_font_5x8 as a result. See docs/window_manager.md, "Hardware glyph
# blitting".
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

# calc_core.o is the arithmetic, kept free of any I/O so the test
# suite below can build it for the host -- see calc_test.c.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o \
	calc_core.o calc.o

calc: calc.elf calc.bin

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

zwidget.o:
	$(CC) $(CFLAGS) -c ../../common/zwidget.c -o zwidget.o

calc_core.o: calc_core.c calc_core.h
	$(CC) $(CFLAGS) -c calc_core.c -o calc_core.o

calc.o:
	$(CC) $(CFLAGS) -c calc.c -S -o calc.asm
	$(CC) $(CFLAGS) -c calc.c -o calc.o

calc.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,calc.map -o calc.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble calc.elf > calc.dasm

# Produces a .zexe rather than a raw --pad-to binary. calc has a large
# .bss -- the document buffer is the full 640x480 framebuffer size, so
# 37.5K on its own -- and the old format wrote .bss out as literal
# zeros, so that would be 37.5K of zeros read off the SD card on every
# launch. The header carries it as a NUMBER instead. See
# sw/common/zexec.h.
calc.bin: calc.elf
	@EDATA=$$($(PREFIX)nm calc.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm calc.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary calc.elf calc.data; \
	python3 ../../../tools/mkexec.py calc.data calc.bin $$(($$END - $$EDATA)); \
	rm -f calc.data

# Host build of the arithmetic, with the host's own compiler -- not
# $(CC), which is the cross compiler and produces something this
# machine cannot run.
#
#   make test
#
# Worth running after ANY change to calc_core.c. A calculator that is
# subtly wrong is worse than no calculator, because nobody checks its
# answers -- which is why the arithmetic is in a file with no I/O in
# it at all.
test:
	cc --std=gnu99 -Wall -Wextra -o calc_test calc_test.c calc_core.c
	./calc_test

clean:
	rm -f calc.elf calc.bin calc.data calc.asm calc.dasm calc.map calc_test *.o *.d

.PHONY: calc clean test

# Header dependency tracking -- see sw/apps/wm/Makefile for the full
# explanation. In short: without this, editing a .c or .h rebuilds
# nothing once the object exists, and the link silently produces a
# binary from stale objects.
-include $(wildcard *.d)
-include $(wildcard */*.d)
-include $(wildcard */*/*.d)
