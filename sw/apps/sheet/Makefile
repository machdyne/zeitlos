
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

# -DZ_GFX_HW_BLIT, same as wm/text/hex/draw: every glyph this app draws
# -- every cell in the grid, the edit bar, the file dialogs -- goes
# through the hardware glyph blitter. A full repaint here is a
# screenful of characters, so it matters as much as it does in text.
#
# sheet does NOT call z_gfx_hw_font_load() itself -- wm is the only
# process on the board that ever loads glyph memory, and both fonts
# this app switches between are already resident. See
# docs/window_manager.md, "Hardware glyph blitting".
#
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

# sheet_core.o is the spreadsheet itself -- cells, formulas, files --
# kept free of any I/O so the test suite below can build it for the
# host. zfix.o is the fixed-point arithmetic, shared with sw/apps/calc.
# ztype.o is only for z_ftype_ext(), which is how a .CSV is told from a
# .ZSS on the way in and out.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o \
	zfsapp.o zflist.o zdialog.o zedit.o zfix.o ztype.o sheet_core.o sheet.o

sheet: sheet.elf sheet.bin

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

zfix.o:
	$(CC) $(CFLAGS) -c ../../common/zfix.c -o zfix.o

ztype.o:
	$(CC) $(CFLAGS) -c ../../common/ztype.c -o ztype.o

sheet_core.o: sheet_core.c sheet_core.h
	$(CC) $(CFLAGS) -c sheet_core.c -o sheet_core.o

sheet.o:
	$(CC) $(CFLAGS) -c sheet.c -S -o sheet.asm
	$(CC) $(CFLAGS) -c sheet.c -o sheet.o

sheet.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,sheet.map -o sheet.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble sheet.elf > sheet.dasm

# Produces a .zexe rather than a raw --pad-to binary, for the same
# reason sw/apps/text does: this app's .bss is dominated by the cell
# array and the text arena (32KB between them), and the old format
# wrote all of that out as literal zeros to be read off the SD card on
# every launch. The header carries the size as a NUMBER instead. See
# sw/common/zexec.h.
sheet.bin: sheet.elf
	@EDATA=$$($(PREFIX)nm sheet.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm sheet.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary sheet.elf sheet.data; \
	python3 ../../../tools/mkexec.py sheet.data sheet.bin $$(($$END - $$EDATA)); \
	rm -f sheet.data

# Host builds, with the host's own compiler -- not $(CC), which is the
# cross compiler and produces something this machine cannot run.
#
#   make test        the spreadsheet model: arithmetic, formulas,
#                    evaluation, the file format
#   make test-layout the window geometry: the hit-test round trip
#                    across window sizes, fonts and column widths
#   make test-files  this app's own file paths: the chunked reader,
#                    the streamed writer, and which format runs
#   make render      draws the panel to /tmp/sheet.pbm so it can be
#                    LOOKED AT before it reaches a screen
#
# `test` is worth running after ANY change to sheet_core.c: a
# spreadsheet that is subtly wrong is worse than no spreadsheet,
# because nobody re-checks its arithmetic.
#
# The other two need -no-pie and vm.mmap_min_addr=0 -- see
# sw/common/tests/ztramp.h -- and exit 77 (the skip convention) where
# that isn't available.
HOSTSRC = ../../common/zwin.c ../../common/zwidget.c \
	../../common/zfont_data.c ../../common/zobj.c ../../common/zeitlos.c \
	../../common/zfix.c sheet_core.c

test:
	cc --std=gnu99 -Wall -Wextra -o sheet_test \
		tests/test_core.c sheet_core.c ../../common/zfix.c
	./sheet_test

test-layout:
	cc --std=gnu99 -Wall -no-pie -I ../../common -o sheet_layout \
		tests/test_layout.c $(HOSTSRC)
	./sheet_layout

# ztype.c is linked for real here rather than stubbed, because which
# writer runs is decided by z_ftype_ext() and that decision is part of
# what this tests.
test-files:
	cc --std=gnu99 -Wall -no-pie -I ../../common -o sheet_files \
		tests/test_files.c ../../common/ztype.c $(HOSTSRC)
	./sheet_files

render:
	cc --std=gnu99 -Wall -no-pie -I ../../common -o sheet_render \
		tests/render.c $(HOSTSRC)
	./sheet_render /tmp/sheet.pbm

clean:
	rm -f sheet.elf sheet.bin sheet.data sheet.asm sheet.dasm sheet.map \
		sheet_test sheet_layout sheet_files sheet_render *.o *.d

.PHONY: sheet clean test test-layout test-files render

# Header dependency tracking -- see sw/apps/wm/Makefile for the full
# explanation. In short: without this, editing a .c or .h rebuilds
# nothing once the object exists, and the link silently produces a
# binary from stale objects.
-include $(wildcard *.d)
-include $(wildcard */*.d)
-include $(wildcard */*/*.d)

# sw/common/zedit.c -- the single-line text field.
#
# zdialog.c owned it privately until sw/apps/web's URL bar became its
# second caller. Every app that links the dialog needs this too: a
# shared widget is only shared if the build says so.
zedit.o:
	$(CC) $(CFLAGS) -c ../../common/zedit.c -o zedit.o
