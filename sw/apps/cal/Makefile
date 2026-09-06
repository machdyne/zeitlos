
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
#
# It matters more than usual here: this app links zrtc.o for
# z_time_to_tm()/z_tm_to_time()/z_month_name_long(), and section GC is
# what stops the long month-name table being carried by every OTHER
# app that links zrtc.o for the register accessors alone.
GC_SECTIONS ?= 1
ifeq ($(GC_SECTIONS),1)
GC_CFLAGS = -ffunction-sections -fdata-sections
GC_LDFLAGS = -Wl,--gc-sections
endif

# -DZ_GFX_HW_BLIT, same as wm/clock/settings: the text goes through
# the hardware glyph blitter. cal does NOT call z_gfx_hw_font_load()
# itself -- wm is the only process on the board that ever loads glyph
# memory, and it loads BOTH z_font_5x8 and z_font_6x12 at startup,
# which is what lets this app use 6x12 for the grid and 5x8 for the
# status line without touching glyph memory. See
# docs/window_manager.md, "Hardware glyph blitting".
#
# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS. A command-line assignment replaces the makefile's CFLAGS
# outright -- `make CFLAGS+=-DFOO=1` appends to the command-line
# value, not to this one -- silently dropping -Os, -march, section GC,
# the libc specs and any -D this app needs to build correctly. The
# result still builds and links; it just produces a much larger,
# slower binary that can overrun the space the loader has for it and
# crash on start.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# cal_core.o is the month geometry and the year-entry state machine,
# kept free of any I/O so the test suite below can build it for the
# host -- see tests/cal_test.c. It has no date arithmetic of its own;
# that is zrtc.o, which is the same code the kernel and clock use.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zrtc.o \
	cal_core.o cal.o

cal: cal.elf cal.bin

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

zrtc.o:
	$(CC) $(CFLAGS) -c ../../common/zrtc.c -o zrtc.o

cal_core.o:
	$(CC) $(CFLAGS) -c cal_core.c -o cal_core.o

cal.o:
	$(CC) $(CFLAGS) -c cal.c -S -o cal.asm
	$(CC) $(CFLAGS) -c cal.c -o cal.o

cal.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,cal.map -o cal.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble cal.elf > cal.dasm

# Produces a .zexe rather than a raw --pad-to binary -- the header
# carries .bss as a NUMBER instead of writing it out as literal zeros
# to be read off the SD card on every launch. See sw/common/zexec.h.
cal.bin: cal.elf
	@EDATA=$$($(PREFIX)nm cal.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm cal.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary cal.elf cal.data; \
	python3 ../../../tools/mkexec.py cal.data cal.bin $$(($$END - $$EDATA)); \
	rm -f cal.data

# -- host-side tests --
#
# Native cc, no RISC-V toolchain involved. cal.c, cal_core.c, zrtc.c,
# zwin.c and zwidget.c are the REAL sources in all three targets; only
# the pixel plotting is software (sw/common/tests/zrender.h).
#
# zrtc.c compiles on a host unmodified -- its MMIO accessors are
# static inlines that nothing in the tests calls -- which is what lets
# the calendar arithmetic under test be the shipped calendar
# arithmetic rather than a copy of it.
HOSTCC ?= cc

# Two lists, because the two kinds of test need different things and
# using one for both does not link.
#
# CORESRC is the logic: the grid, the year entry and the calendar
# arithmetic under them. Nothing here draws, so nothing here needs the
# window layer -- and it must NOT have it, because zwin.c and
# zwidget.c reference the z_fb_* pixel primitives that only
# sw/common/tests/zrender.h supplies. Linking them into a test that
# does not include that header fails with a page of undefined
# references to functions the test never calls.
CORESRC = ../../common/zrtc.c cal_core.c

# PANELSRC adds the real window and widget layers, for the two tests
# that do include zrender.h and therefore do have the software pixel
# primitives. zgfx.c is deliberately absent -- see zrender.h's header
# for why the plotting is stubbed and the layout is not.
PANELSRC = $(CORESRC) ../../common/zwin.c ../../common/zwidget.c \
	../../common/zfont_data.c ../../common/zobj.c ../../common/zeitlos.c

HOSTFLAGS = -std=gnu99 -Wall -no-pie -I ../../common

# The layout render (sw/common/tests/zrender.h) -- see
# docs/window_manager.md, "Rendering a panel on the build machine".
# LOOK at the output before changing the grid.
#
# Four states, because three of them are awkward to reach on hardware:
#
#   make render                 today marked, ordinary five-row month
#   make render WHAT=sixrow     31 days from a Saturday -- all six rows
#   make render WHAT=epoch      no clock: 1970, no mark, dead `<`
#   make render WHAT=nortc      no RTC in the bitstream at all
#   make render WHAT=entry      a year being typed
WHAT ?= today

render:
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/cal_render tests/render.c $(PANELSRC)
	/tmp/cal_render /tmp/cal.pbm $(WHAT)
	@echo "wrote /tmp/cal.pbm ($(WHAT)) -- other states: make render WHAT=sixrow|epoch|nortc|entry"

# Unattended. cal_test covers the grid arithmetic and the year-entry
# state machine across every month from 1970 to 2105; test_layout
# covers the geometry that a render would show but only if somebody
# looks.
test:
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/cal_test tests/cal_test.c $(CORESRC)
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/cal_test_layout tests/test_layout.c $(PANELSRC)
	/tmp/cal_test
	/tmp/cal_test_layout

clean:
	rm -f cal.elf cal.bin cal.data cal.asm cal.dasm cal.map *.o *.d

.PHONY: cal clean render test

# Header dependency tracking -- see sw/apps/wm/Makefile for the full
# explanation. In short: without this, editing a .c or .h rebuilds
# nothing once the object exists, and the link silently produces a
# binary from stale objects.
-include $(wildcard *.d)
-include $(wildcard */*.d)
