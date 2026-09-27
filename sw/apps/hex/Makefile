# sw/apps/hex -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = hex

# zfsapp.o is needed by zflist.o (the file browser inside the dialogs)
# as well as by hex.c itself.

# Objects, and why each is linked:
#
#   zedit.o: sw/common/zedit.c -- the single-line text field.
#
#   zdialog.c owned it privately until sw/apps/web's URL bar became its
#   second caller. Every app that links the dialog needs this too: a
#   shared widget is only shared if the build says so.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zflist.o zdialog.o zedit.o hex.o

# A .zexe rather than a raw --pad-to binary, same as text/draw: the
# header carries the bss size as a NUMBER instead of writing it out as
# literal zeros to be read off the card on every launch. This app's
# .bss is dominated by the 4KB file cache. See sw/common/zexec.h.

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

.PHONY: render test

include ../../common/app.mk
