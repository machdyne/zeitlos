
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

# -DZ_GFX_HW_BLIT, same as wm/hello_win/term: the widget toolkit reads
# its text labels through the hardware glyph blitter. read does NOT
# call z_gfx_hw_font_load() itself -- wm is the only process on the
# board that ever loads glyph memory, and every app reads with
# z_font_5x8 as a result. See docs/window_manager.md, "Hardware glyph
# blitting".
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# md.o is the Markdown parser, deliberately free of I/O so the test
# suite below can build it for the host -- see md_test.c.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o \
	zfsapp.o zflist.o zdialog.o ztype.o md.o read.o

read: read.elf read.bin

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

md.o: md.c md.h
	$(CC) $(CFLAGS) -c md.c -o md.o

read.o:
	$(CC) $(CFLAGS) -c read.c -S -o read.asm
	$(CC) $(CFLAGS) -c read.c -o read.o

read.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,read.map -o read.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble read.elf > read.dasm

# Produces a .zexe rather than a raw --pad-to binary. read has a large
# .bss -- the document buffer is the full 640x480 framebuffer size, so
# 37.5K on its own -- and the old format wrote .bss out as literal
# zeros, so that would be 37.5K of zeros read off the SD card on every
# launch. The header carries it as a NUMBER instead. See
# sw/common/zexec.h.
read.bin: read.elf
	@EDATA=$$($(PREFIX)nm read.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm read.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary read.elf read.data; \
	python3 ../../../tools/mkexec.py read.data read.bin $$(($$END - $$EDATA)); \
	rm -f read.data

# Host builds of the parser and the layout logic, with the host's own
# compiler -- not $(CC), which is the cross compiler.
#
#   make test                 unit tests plus invariants
#   make test DOCS="a.md b.md"   also run them over real documents
#
# The second form is the one that finds things. Real Markdown contains
# constructs nobody thinks to write a case for -- hard-wrapped link
# syntax, nested lists indented like code blocks -- and both of those
# were found exactly this way.
DOCS ?= ../../../docs/*.md

test:
	cc --std=gnu99 -Wall -Wextra -o md_test md_test.c md.c
	./md_test $(DOCS)

# Renders a document to ASCII using the same parser, wrapping and
# block logic the app uses, and checks that scrolling is REVERSIBLE --
# down N display lines and back up N must land exactly where it
# started, from every position. That property is why positions are
# block-aligned; see read.c.
#
#   make render DOC=file.md            layout, as text
#   make render DOC=file.md MODE=scroll  reversibility check
DOC ?= ../../../docs/welcome.md
MODE ?= render

render:
	cc --std=gnu99 -Wall -o render_test render_test.c md.c
	./render_test $(DOC) 300 $(MODE)

clean:
	rm -f read.elf read.bin read.data read.asm read.dasm read.map md_test render_test *.o *.d

.PHONY: read clean test render

# Header dependency tracking -- see sw/apps/wm/Makefile for the full
# explanation. In short: without this, editing a .c or .h rebuilds
# nothing once the object exists, and the link silently produces a
# binary from stale objects.
-include $(wildcard *.d)
-include $(wildcard */*.d)
-include $(wildcard */*/*.d)
