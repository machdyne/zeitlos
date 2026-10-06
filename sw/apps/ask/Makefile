# sw/apps/ask -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = ask

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

APP_CFLAGS = $(SLICE_DEFS)

# No zwidget/zdialog/zflist here, deliberately: `ask` has one text
# field and one list, both of which it draws itself in about forty
# lines. Linking the toolkit for that would cost more than it saves,
# and section GC cannot drop what a widget set's function pointers
# reach.

# Objects, and why each is linked:
#
#   aidx.o: The index reader and query engine. Also builds on the HOST against
#   stdio -- see test/Makefile -- which is how the on-card format is
#   validated against the packer that writes it, before any of this
#   reaches a board.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zfsapp.o \
	aidx.o ask.o

# A .zexe rather than a raw --pad-to binary. The index lives on the
# HEAP, not in .bss, so this app's .bss is small -- but the format is
# what every other app uses and carries .bss as a number rather than as
# literal zeros read off the card. See sw/common/zexec.h.

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
.PHONY: hosttest render

clean: clean-test
clean-test:
	$(MAKE) -C test clean
.PHONY: clean-test

# 1MB. The pack this tree ships is dense=no and needs little; a
# dense=yes pack still works up to about a megabyte and then says
# it needs 4MB. It was 4MB while the dense half was on.
APP_STACK = 1M

include ../../common/app.mk
