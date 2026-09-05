
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

# -DZ_GFX_HW_BLIT, same as wm/text/term: every glyph goes through the
# hardware glyph blitter. It matters more here than almost anywhere --
# a full repaint of a 16-byte-wide grid is rows x 76 characters, and
# this app draws one whole row per call precisely so that cost is paid
# in as few blitter setups as possible.
#
# hex does NOT call z_gfx_hw_font_load() itself. wm is the only process
# on the board that ever loads glyph memory, and both fonts this app
# switches between are already resident. See docs/window_manager.md,
# "Fonts in glyph memory".
#
# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS -- a command-line assignment replaces the makefile's outright,
# silently dropping -Os, -march, section GC and the libc specs. The
# result still builds; it is just much larger and slower, and can
# overrun the space the loader has for it. See sw/apps/text/Makefile.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) -DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# zfsapp.o is needed by zflist.o (the file browser inside the dialogs)
# as well as by hex.c itself.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o \
	zfsapp.o zflist.o zdialog.o hex.o

hex: hex.elf hex.bin

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

hex.o:
	$(CC) $(CFLAGS) -c hex.c -S -o hex.asm
	$(CC) $(CFLAGS) -c hex.c -o hex.o

hex.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,hex.map -o hex.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble hex.elf > hex.dasm

# A .zexe rather than a raw --pad-to binary, same as text/draw: the
# header carries the bss size as a NUMBER instead of writing it out as
# literal zeros to be read off the card on every launch. This app's
# .bss is dominated by the 4KB file cache. See sw/common/zexec.h.
hex.bin: hex.elf
	@EDATA=$$($(PREFIX)nm hex.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm hex.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary hex.elf hex.data; \
	python3 ../../../tools/mkexec.py hex.data hex.bin $$(($$END - $$EDATA)); \
	rm -f hex.data

# -- host-side tests --
#
# Native cc, no RISC-V toolchain involved. hex.c, zwin.c and zwidget.c
# are the REAL sources in all three; only the pixel plotting and the
# filesystem are stubbed.
#
# -no-pie is REQUIRED, not a preference: tests/trampoline.h plants a
# function pointer in reg_kernel, which is a uint32_t, and a PIE build
# puts the stub above 4GB where the truncated pointer aims at nothing.
# The tests detect it and exit 77 rather than crashing, so a missing
# flag shows up as a skip. They also need page 0 mappable:
#
#     sudo sysctl -w vm.mmap_min_addr=0
#
# See tests/trampoline.h for why both are needed.
HOSTCC ?= cc
HOSTSRC = ../../common/zwin.c ../../common/zwidget.c \
	../../common/zfont_data.c ../../common/zobj.c ../../common/zeitlos.c
HOSTFLAGS = -std=gnu99 -Wall -no-pie -I ../../common

# The layout render (sw/common/tests/zrender.h) -- see
# docs/window_manager.md, "Rendering a panel on the build machine".
# LOOK at the output before changing the grid; that is the whole point
# of it, and it is what caught both layout problems this app has had.
render:
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/hex_render tests/render.c $(HOSTSRC)
	/tmp/hex_render /tmp/hex.pbm
	@echo "wrote /tmp/hex.pbm -- try: /tmp/hex_render /tmp/hex.pbm 500 300 1"

# Unattended: geometry across every window size and font, and the
# editing model (journal, save coalescing, undo) against a stub card
# that counts its seeks and writes.
test:
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/hex_test_layout tests/test_layout.c $(HOSTSRC)
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/hex_test_edit tests/test_edit.c $(HOSTSRC)
	/tmp/hex_test_layout
	/tmp/hex_test_edit

clean:
	rm -f hex.elf hex.bin hex.data hex.asm hex.dasm hex.map *.o *.d

.PHONY: hex clean render test

# Header dependency tracking -- see sw/apps/wm/Makefile for the full
# explanation. In short: without this, editing a .c or .h rebuilds
# nothing once the object exists, and the link silently produces a
# binary from stale objects.
-include $(wildcard *.d)
-include $(wildcard */*.d)
-include $(wildcard */*/*.d)
