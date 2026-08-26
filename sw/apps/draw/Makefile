
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

# -DZ_GFX_HW_BLIT, same as wm/hello_win/term: the widget toolkit draws
# its text labels through the hardware glyph blitter. draw does NOT
# call z_gfx_hw_font_load() itself -- wm is the only process on the
# board that ever loads glyph memory, and every app draws with
# z_font_5x8 as a result. See docs/window_manager.md, "Hardware glyph
# blitting".
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o draw_icons.o draw.o

draw: draw.elf draw.bin

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

# draw_icons.c/.h are hand-edited (binary literals with an ASCII
# picture of each row in the comment), not generated -- see
# draw_icons.h's own header comment.
draw_icons.o: draw_icons.c draw_icons.h
	$(CC) $(CFLAGS) -c draw_icons.c -o draw_icons.o

draw.o:
	$(CC) $(CFLAGS) -c draw.c -S -o draw.asm
	$(CC) $(CFLAGS) -c draw.c -o draw.o

draw.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,draw.map -o draw.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble draw.elf > draw.dasm

# Produces a .zexe rather than a raw --pad-to binary. draw has a large
# .bss -- the document buffer is the full 640x480 framebuffer size, so
# 37.5K on its own -- and the old format wrote .bss out as literal
# zeros, so that would be 37.5K of zeros read off the SD card on every
# launch. The header carries it as a NUMBER instead. See
# sw/common/zexec.h.
draw.bin: draw.elf
	@EDATA=$$($(PREFIX)nm draw.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm draw.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary draw.elf draw.data; \
	python3 ../../../tools/mkexec.py draw.data draw.bin $$(($$END - $$EDATA)); \
	rm -f draw.data

clean:
	rm -f draw.elf draw.bin draw.data draw.asm draw.dasm draw.map *.o *.d

.PHONY: draw clean

# Header dependency tracking -- see sw/apps/wm/Makefile for the full
# explanation. In short: without this, editing a .c or .h rebuilds
# nothing once the object exists, and the link silently produces a
# binary from stale objects.
-include $(wildcard *.d)
-include $(wildcard */*.d)
-include $(wildcard */*/*.d)
