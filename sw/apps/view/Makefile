
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

# Which decoders to build in. Each is a compile-time switch in
# zimg.h so an app can pay only for what it uses -- see that file's
# Z_IMG_HAVE_* block.
#
# PNG IS OFF BY DEFAULT AND THAT IS A DELIBERATE COST DECISION, not an
# oversight. Its inflate window alone is 32KB of .bss -- more than
# every other decoder here combined -- and DEFLATE is by a wide margin
# the slowest thing that would run on this CPU. Turn it on with:
#
#     make VIEW_PNG=1
#
# once the JPEG timings (VIEW_INSTRUMENT below) have shown what a
# comparable decode actually costs on real hardware. See
# docs/view_app.md, "Why PNG is optional".
VIEW_PNG ?= 0
IMG_DEFS = -DZ_IMG_HAVE_PNG=$(VIEW_PNG)

# Decode timing to UART0, off by default.
#
# Reports source size, output size and elapsed milliseconds for every
# image loaded. The point of it is the dithering-versus-decoding
# question: if dithering turns out to dominate, a hardware
# single-line ditherer becomes worth building, and if it doesn't, it
# very much isn't. Build with:
#
#     make VIEW_INSTRUMENT=1
#
# Costs nothing when off -- the whole block is #if'd out, and dbg()
# itself is dropped by --gc-sections if nothing else calls it.
VIEW_INSTRUMENT ?= 0
INSTR_DEFS = -DVIEW_INSTRUMENT=$(VIEW_INSTRUMENT)

# Cycle-level phase profiling (sw/common/zprof.h), off by default.
#
#     make VIEW_PROF=1
#
# Reports per-phase cycles, retired instructions and IPC to UART0 for
# every image loaded. This is the instrument that answers "where did
# the time go", which VIEW_INSTRUMENT above cannot: the KTIMER tick is
# 1.37ms and every phase worth optimising is shorter than that.
#
# ** This emits rdcycle/rdinstret. ** If a board is ever built with
# picorv32's ENABLE_COUNTERS(0) those become illegal instructions and
# this build will hang on the first image -- which is precisely why it
# is a separate flag and not always on. See zprof.h.
VIEW_PROF ?= 0
PROF_DEFS = -DZ_PROF=$(VIEW_PROF)

# Optimisation level for the decoders ONLY.
#
# Everything in this tree builds -Os, and for almost everything that
# is right: instruction fetch is the dominant term in CPI on this SOC
# (rtl/cache.v) so smaller code really is faster code.
#
# zimg.c's inverse DCT is the exception worth testing. It holds ~20
# live 32-bit values through the butterfly, and at -Os gcc spills
# rather than unrolls -- on a machine with no data cache every spill
# is a main-memory round trip at ~11 cycles (SDRAM) or ~63 (PSRAM).
# The profile showed ~7,400 instructions per 8x8 block against maybe
# 900 for a well-scheduled one, which is what spilling looks like.
#
#     make ZIMG_OPT=-O2 VIEW_PROF=1
#
# costs ~3KB of text. A/B it against the idct row of the profile
# before keeping it -- this is a hypothesis with a cheap experiment
# attached, not a known win, and -O2 could equally lose by pushing the
# hot loop out of the instruction cache.
ZIMG_OPT ?= -Os

# -DZ_GFX_HW_BLIT, same as wm/hello_win/term/draw: the widget toolkit
# draws its text labels through the hardware glyph blitter. view does
# NOT call z_gfx_hw_font_load() itself -- wm is the only process on
# the board that ever loads glyph memory, and every app draws with
# z_font_5x8 as a result. See docs/window_manager.md, "Hardware glyph
# blitting".
#
# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS. A command-line assignment replaces the makefile's CFLAGS
# outright -- `make CFLAGS+=-DFOO=1` appends to the command-line
# value, not to this one -- silently dropping -Os, -march, section GC,
# the libc specs and any -D this app needs to build correctly. The
# result still builds and links; it just produces a much larger,
# slower binary that can overrun the space the loader has for it and
# crash on start.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	-DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS) \
	$(IMG_DEFS) $(INSTR_DEFS) $(PROF_DEFS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# zfsapp.o/zflist.o/zdialog.o are the file dialogs -- zflist.o needs
# zfsapp.o, and view.c needs both plus zdialog.o for the open dialog.
# zimg.o is the decoders themselves (sw/common/zimg.c), shared with
# any other app that wants to read images.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o \
	zfsapp.o zflist.o zdialog.o zimg.o zprof.o view.o

view: view.elf view.bin

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

# $(ZIMG_OPT) last so it overrides the -Os in CFLAGS.
zimg.o:
	$(CC) $(CFLAGS) $(ZIMG_OPT) -c ../../common/zimg.c -o zimg.o

zprof.o:
	$(CC) $(CFLAGS) -c ../../common/zprof.c -o zprof.o

view.o:
	$(CC) $(CFLAGS) -c view.c -S -o view.asm
	$(CC) $(CFLAGS) -c view.c -o view.o

view.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,view.map -o view.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble view.elf > view.dasm

# Produces a .zexe rather than a raw --pad-to binary. view has a large
# .bss -- the document buffer alone is the full 640x480 framebuffer
# size, 37.5K, and the decoder scratch is another 20K -- and the old
# format wrote .bss out as literal zeros, so that would be ~58K of
# zeros read off the SD card on every launch. The header carries it as
# a NUMBER instead. See sw/common/zexec.h.
view.bin: view.elf
	@EDATA=$$($(PREFIX)nm view.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm view.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary view.elf view.data; \
	python3 ../../../tools/mkexec.py view.data view.bin $$(($$END - $$EDATA)); \
	rm -f view.data

clean:
	rm -f view.elf view.bin view.data view.asm view.dasm view.map *.o *.d

.PHONY: view clean

# Header dependency tracking -- see sw/apps/wm/Makefile for the full
# explanation. In short: without this, editing a .c or .h rebuilds
# nothing once the object exists, and the link silently produces a
# binary from stale objects.
-include $(wildcard *.d)
-include $(wildcard */*.d)
-include $(wildcard */*/*.d)
