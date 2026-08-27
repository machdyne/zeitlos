
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

# -DZ_GFX_HW_BLIT, same as wm/settings/term: the digital view's text
# and the widget toolkit's button labels both go through the hardware
# glyph blitter. clock does NOT call z_gfx_hw_font_load() itself -- wm
# is the only process on the board that ever loads glyph memory, and it
# loads BOTH z_font_5x8 and z_font_6x12 there at startup, which is what
# makes the 6x12 digital display possible without breaking that
# single-owner rule. See docs/window_manager.md, "Hardware glyph
# blitting".
CFLAGS = --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# A small app: a window, three widgets, and no filesystem, dialogs or
# document of any kind. zrtc.o is the one thing here that isn't in
# every app's list -- it's the calendar arithmetic behind the date
# line and the hour hand.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zrtc.o clock.o

clock: clock.elf clock.bin

zeitlos.o: ../../common/zeitlos.c ../../common/zeitlos.h
	$(CC) $(CFLAGS) -c ../../common/zeitlos.c -o zeitlos.o

zobj.o: ../../common/zobj.c ../../common/zobj.h
	$(CC) $(CFLAGS) -c ../../common/zobj.c -o zobj.o

zgfx.o: ../../common/zgfx.c ../../common/zgfx.h
	$(CC) $(CFLAGS) -c ../../common/zgfx.c -o zgfx.o

zfont_data.o: ../../common/zfont_data.c ../../common/zfont.h
	$(CC) $(CFLAGS) -c ../../common/zfont_data.c -o zfont_data.o

zwin.o: ../../common/zwin.c ../../common/zwin.h
	$(CC) $(CFLAGS) -c ../../common/zwin.c -o zwin.o

zwidget.o: ../../common/zwidget.c ../../common/zwidget.h
	$(CC) $(CFLAGS) -c ../../common/zwidget.c -o zwidget.o

zrtc.o: ../../common/zrtc.c ../../common/zrtc.h
	$(CC) $(CFLAGS) -c ../../common/zrtc.c -o zrtc.o

clock.o: clock.c ../../common/zrtc.h ../../common/zntp.h
	$(CC) $(CFLAGS) -c clock.c -S -o clock.asm
	$(CC) $(CFLAGS) -c clock.c -o clock.o

clock.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,clock.map -o clock.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble clock.elf > clock.dasm

# Produces a ZEXE rather than a raw --pad-to binary, same as every
# other app here -- the header carries .bss as a NUMBER for the loader
# to memset instead of shipping it as literal zeros in the file. See
# sw/common/zexec.h and tools/mkexec.py.
clock.bin: clock.elf
	@EDATA=$$($(PREFIX)nm clock.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm clock.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary clock.elf clock.data; \
	python3 ../../../tools/mkexec.py clock.data clock.bin $$(($$END - $$EDATA)); \
	rm -f clock.data

clean:
	rm -f clock.elf clock.bin clock.data clock.asm clock.dasm clock.map *.o *.d

.PHONY: clock clean

# Header dependency tracking -- see sw/apps/wm/Makefile for the full
# explanation. In short: without this, editing a .c or .h rebuilds
# nothing once the object exists, and the link silently produces a
# binary from stale objects.
-include $(wildcard *.d)
-include $(wildcard */*.d)
-include $(wildcard */*/*.d)
