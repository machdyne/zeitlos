PREFIX ?= $(RISCV_PREFIX)
# ARCH/ABI come from one place -- see that file for why
include ../../common/arch.mk
CC = $(PREFIX)gcc
AS = $(PREFIX)as
ASFLAGS = -march=$(ARCH) -mabi=$(ABI)

# See sw/apps/repl/Makefile for the full reasoning -- in short,
# sw/common is shared by every app, so each links whole objects for the
# sake of a fraction of them, and section GC drops the rest. That
# matters more here than it used to: this app now links zdialog.o and
# zflist.o for one file-open dialog, and those pull in the whole widget
# toolkit. Set GC_SECTIONS=0 to disable.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# -DZ_GFX_HW_BLIT, same as wm/hello_win/term/draw/text: the FPS readout
# is redrawn every single frame, so its glyphs go through the hardware
# glyph blitter rather than a software per-pixel loop. At several
# hundred frames per second a software path for even seven characters
# is measurable against the thing the counter is trying to measure.
#
# gpu3d does NOT call z_gfx_hw_font_load() itself -- wm is the only
# process that ever loads glyph memory, and every app draws with
# z_font_5x8 as a result. See docs/window_manager.md, "Hardware glyph
# blitting".
# -DGPU3D_PERF=0 compiles out the per-frame cycle counters and the
# once-a-second serial-console report (see docs/gpu3d_app.md). They
# are on by default: the whole reason this app exists is to be a
# measurable load on the GPU, and a measurement you have to rebuild to
# get is one nobody takes.
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

# zfsapp.o is what stl.c reads files through, and is also needed by
# zflist.o (the file browser inside the dialog). zfont_data.o arrived
# with the FPS readout -- this app drew no text at all before.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o \
	zfsapp.o zflist.o zdialog.o stl.o gpu3d.o

gpu3d: gpu3d.elf gpu3d.bin

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

stl.o:
	$(CC) $(CFLAGS) -c stl.c -o stl.o

gpu3d.o:
	$(CC) $(CFLAGS) -c gpu3d.c -S -o gpu3d.asm
	$(CC) $(CFLAGS) -c gpu3d.c -o gpu3d.o

gpu3d.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,gpu3d.map -o gpu3d.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble gpu3d.elf > gpu3d.dasm

# Produces a .zexe rather than a raw --pad-to binary, for the same
# reason sw/apps/text and sw/apps/draw do: this app's .bss is now
# dominated by the model arrays and the loader's hash tables (~30KB),
# and the old format wrote all of that out as literal zeros to be read
# off the SD card on every launch. The header carries the size as a
# NUMBER instead. See sw/common/zexec.h.
gpu3d.bin: gpu3d.elf
	@EDATA=$$($(PREFIX)nm gpu3d.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm gpu3d.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary gpu3d.elf gpu3d.data; \
	python3 ../../../tools/mkexec.py gpu3d.data gpu3d.bin $$(($$END - $$EDATA)); \
	rm -f gpu3d.data

clean:
	rm -f gpu3d.elf gpu3d.bin gpu3d.data gpu3d.asm gpu3d.dasm gpu3d.map *.o *.d

.PHONY: gpu3d clean

# Header dependency tracking -- see sw/apps/wm/Makefile for the full
# explanation. In short: without this, editing a .c or .h rebuilds
# nothing once the object exists, and the link silently produces a
# binary from stale objects.
-include $(wildcard *.d)
-include $(wildcard */*.d)
-include $(wildcard */*/*.d)
