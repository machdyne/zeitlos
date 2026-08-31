
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

# -DZ_GFX_HW_BLIT, same as wm/hello_win/term: the widget toolkit filess
# its text labels through the hardware glyph blitter. files does NOT
# call z_gfx_hw_font_load() itself -- wm is the only process on the
# board that ever loads glyph memory, and every app filess with
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

# zflist.o is the browsing widget this app is mostly made of, zdialog.o
# the confirm/prompt dialogs, ztype.o the extension-to-app table.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o \
	zfsapp.o zflist.o zdialog.o ztype.o files.o

files: files.elf files.bin

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

zfsapp.o:
	$(CC) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o

zflist.o:
	$(CC) $(CFLAGS) -c ../../common/zflist.c -o zflist.o

zdialog.o:
	$(CC) $(CFLAGS) -c ../../common/zdialog.c -o zdialog.o

ztype.o:
	$(CC) $(CFLAGS) -c ../../common/ztype.c -o ztype.o

files.o:
	$(CC) $(CFLAGS) -c files.c -S -o files.asm
	$(CC) $(CFLAGS) -c files.c -o files.o

files.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,files.map -o files.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble files.elf > files.dasm

# Produces a .zexe rather than a raw --pad-to binary. files has a large
# .bss -- the document buffer is the full 640x480 framebuffer size, so
# 37.5K on its own -- and the old format wrote .bss out as literal
# zeros, so that would be 37.5K of zeros read off the SD card on every
# launch. The header carries it as a NUMBER instead. See
# sw/common/zexec.h.
files.bin: files.elf
	@EDATA=$$($(PREFIX)nm files.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm files.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary files.elf files.data; \
	python3 ../../../tools/mkexec.py files.data files.bin $$(($$END - $$EDATA)); \
	rm -f files.data

clean:
	rm -f files.elf files.bin files.data files.asm files.dasm files.map *.o *.d

.PHONY: files clean

# Header dependency tracking -- see sw/apps/wm/Makefile for the full
# explanation. In short: without this, editing a .c or .h rebuilds
# nothing once the object exists, and the link silently produces a
# binary from stale objects.
-include $(wildcard *.d)
-include $(wildcard */*.d)
-include $(wildcard */*/*.d)
