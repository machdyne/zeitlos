
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

# How many vectors the coarse scan does between returns to the message
# loop. The responsiveness dial -- see Z_ASK_SCAN_SLICE in
# sw/common/zask.h, which is where the number is justified. Overridable
# for measurement on a board rather than by editing the header:
#
#     make ASK_SLICE=512
#
# Lower makes cancellation crisper and per-slice overhead a larger
# fraction of the query; higher does the opposite. 2048 at 32
# dimensions is ~65K MACs, a few milliseconds on a 48MHz core, which is
# inside one frame.
ASK_SLICE ?= 0
ifneq ($(ASK_SLICE),0)
SLICE_DEFS = -DAI_SCAN_SLICE=$(ASK_SLICE)
endif

# -DZ_GFX_HW_BLIT, same as wm/view/term/draw: text is drawn through the
# hardware glyph blitter. `ask` does NOT call z_gfx_hw_font_load() --
# wm is the only process that ever loads glyph memory, and every app
# draws with z_font_5x8 as a result. See docs/window_manager.md,
# "Hardware glyph blitting".
#
# `override` so the flags this app REQUIRES survive a command-line
# CFLAGS. A command-line assignment replaces the makefile's CFLAGS
# outright, silently dropping -Os, -march, section GC and the libc
# specs -- the result still links, it is just much larger and can
# overrun the space the loader has for it.
override CFLAGS += --std=gnu99 -Os -MD -Wall -march=$(ARCH) -mabi=$(ABI) \
	-DZ_GFX_HW_BLIT $(GC_CFLAGS) $(ARCH_DEFS) $(LIBC_FLAGS) $(SLICE_DEFS)
LDFLAGS = -march=$(ARCH) -mabi=$(ABI) $(GC_LDFLAGS) $(LIBC_FLAGS)
LDSCRIPT = ../../common/riscv-app.ld

# No zwidget/zdialog/zflist here, deliberately: `ask` has one text
# field and one list, both of which it draws itself in about forty
# lines. Linking the toolkit for that would cost more than it saves,
# and section GC cannot drop what a widget set's function pointers
# reach.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zfsapp.o \
	aidx.o ask.o

ask: ask.elf ask.bin

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

zfsapp.o:
	$(CC) $(CFLAGS) -c ../../common/zfsapp.c -o zfsapp.o

# The index reader and query engine. Also builds on the HOST against
# stdio -- see test/Makefile -- which is how the on-card format is
# validated against the packer that writes it, before any of this
# reaches a board.
aidx.o:
	$(CC) $(CFLAGS) -c aidx.c -o aidx.o

ask.o:
	$(CC) $(CFLAGS) -c ask.c -S -MF /dev/null -o ask.asm
	$(CC) $(CFLAGS) -c ask.c -o ask.o

ask.elf: $(OBJS)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -Wl,-Map,ask.map -o ask.elf $(OBJS)
	$(PREFIX)objdump -S --disassemble ask.elf > ask.dasm

# A .zexe rather than a raw --pad-to binary. The index lives on the
# HEAP, not in .bss, so this app's .bss is small -- but the format is
# what every other app uses and carries .bss as a number rather than as
# literal zeros read off the card. See sw/common/zexec.h.
ask.bin: ask.elf
	@EDATA=$$($(PREFIX)nm ask.elf | awk '$$3=="_edata"{print "0x"$$1}'); \
	END=$$($(PREFIX)nm ask.elf | awk '$$3=="_end"{print "0x"$$1}'); \
	$(PREFIX)objcopy -O binary ask.elf ask.data; \
	python3 ../../../tools/mkexec.py ask.data ask.bin $$(($$END - $$EDATA)); \
	rm -f ask.data

# The host test. Not built by the default target -- it needs the HOST
# compiler, not the RISC-V one.
hosttest:
	$(MAKE) -C test

# Draw the panel on the build machine and write it out as an image.
#
# LOOK AT IT before shipping a layout change. The pack buttons went out
# with their labels one content-inset below their boxes, because
# z_win_hw_box() takes absolute screen coordinates while everything
# else is window-relative -- which no assertion anybody thought to
# write would have caught and which one look catches immediately. See
# sw/common/tests/zrender.h.
#
#     make render && feh /tmp/ask.pbm
render:
	cc -std=gnu99 -w -no-pie -I ../../common -o /tmp/askrender \
		tests/render.c ../../common/zwin.c ../../common/zwidget.c \
		../../common/zfont_data.c ../../common/zobj.c \
		../../common/zeitlos.c
	/tmp/askrender /tmp/ask.pbm

clean:
	rm -f ask.elf ask.bin ask.data ask.asm ask.dasm ask.map *.o *.d
	$(MAKE) -C test clean

# `ask` MUST be here.
#
# It is a convenience target with no file behind it. Without the phony
# declaration, make treats it as a file that does not exist, finds no
# rule that builds it, and falls back to its BUILT-IN link rule
# (`%: %.o`) -- which then links ask.o, ask.elf and ask.bin together:
#
#     cc ask.o ask.elf ask.bin -o ask
#     multiple definition of `_start'
#     multiple definition of `main'
#     ask.bin: file format not recognized
#
# The confusing part is that this happens AFTER a completely successful
# build, so the real output is already correct and sitting on disk.
# Every other app's Makefile in this tree declares its own name here
# for the same reason (`.PHONY: view clean`, and so on).
.PHONY: ask clean hosttest render

# Header dependency tracking -- see sw/apps/wm/Makefile for the full
# explanation. In short: without this, editing a .c or .h rebuilds
# nothing once the object exists, and the link silently produces a
# binary from stale objects.
#
# $(wildcard) rather than $(OBJS:.o=.d) so a .d that does not exist yet
# is simply absent rather than a missing-file rule make has to consider.
-include $(wildcard *.d)
-include $(wildcard */*.d)
