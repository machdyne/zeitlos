# sw/apps/checkup -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = checkup

# Tests this machine and saves what it found (docs/checkup.md). A card
# app, in the base set.

# Integer-only printf: it prints no floating point (docs/build.md).
ZFMT = 1

# Objects, and why each is linked:
#
#   zfsapp.o, zfsc.o: the card and RAM disk checks, and saving the report
#   zdialog.o (zflist, zedit, ztype): the save dialog and the F1 help box
#   zrtc.o: the clock check
#   zrng.o: the TRNG's registers (zrng.h)
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zdialog.o zflist.o zedit.o ztype.o zrtc.o zrng.o checkup.o

# Host test: the real checkup.c with a scripted kernel -- memory, an
# arena, the store, the card, the RAM disk, the log -- every check run
# to the end, failures made on purpose, and the report saved
# (tests/render.c). Needs vm.mmap_min_addr=0; skipped (exit 77) where it
# cannot map address 0.
HOSTCC ?= cc
C = ../../common
.PHONY: test
test:
	$(HOSTCC) -std=gnu99 -w -no-pie -I $(C) -o /tmp/checkup_render tests/render.c $(C)/zwin.c \
		$(C)/zwidget.c $(C)/zfont_data.c $(C)/zobj.c $(C)/zeitlos.c $(C)/zfsapp.c \
		$(C)/zrtc.c $(C)/zspeak.c
	mkdir -p /tmp/checkup-render
	/tmp/checkup_render /tmp/checkup-render/c || [ $$? -eq 77 ]

include ../../common/app.mk
