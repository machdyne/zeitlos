# sw/apps/welcome -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = welcome
APP_MAIN_RULE = 0

# What a new machine shows first, and with a card the first questions
# it offers: keyboard, time zone, display (docs/welcome_app.md). A core app,
# in flash, so it is kept small: no dialogs, no colour themes, and the
# layouts' names generated rather than zkbd's key tables linked.

# The integer-only printf, and stdout without newlib's stdio: it prints
# no floating point and no 64-bit number (docs/build.md, "ZFMT").
ZFMT = 1
ZFMT_LITE = 1
ZFMT_NO64 = 1

# Objects, and why each is linked:
#
#   zrtc.o: the city table, and the time a zone makes
#   zcfg.o: reading the configuration and editing its text
#   zfsapp.o: the whole-file read and write, the card check (fs_df) and
#             removing /sys/firstrun
#   zspeak.o: each page said aloud when speech is on
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zcfg.o zrtc.o welcome.o

# Every ordinary layout's name and description, from the layout table
# (gen_kbd_names.awk). Not committed.
kbd_names.h: ../../common/zkbd_layouts.c gen_kbd_names.awk
	awk -f gen_kbd_names.awk ../../common/zkbd_layouts.c > kbd_names.h

welcome.o: welcome.c kbd_names.h
	$(CC) $(CFLAGS) -c welcome.c -S -MF /dev/null -o welcome.asm
	$(CC) $(CFLAGS) -c welcome.c -o welcome.o

CLEAN_EXTRA = kbd_names.h

# Host test: the real welcome.c with a scripted kernel and file, every
# page walked by keyboard and by pointer and rendered (tests/render.c).
# zkbd.c is linked here only, to check kbd_names.h against the table.
# Needs vm.mmap_min_addr=0; skipped (exit 77) where it cannot map
# address 0.
HOSTCC ?= cc
C = ../../common
.PHONY: test
test: kbd_names.h
	$(HOSTCC) -std=gnu99 -w -no-pie -I $(C) -I . -o /tmp/welcome_render tests/render.c $(C)/zwin.c \
		$(C)/zwidget.c $(C)/zfont_data.c $(C)/zobj.c $(C)/zeitlos.c $(C)/zfsapp.c $(C)/zcfg.c \
		$(C)/zrtc.c $(C)/zspeak.c $(C)/zkbd.c $(C)/zi18n.c
	mkdir -p /tmp/welcome-render
	/tmp/welcome_render /tmp/welcome-render/w || [ $$? -eq 77 ]

include ../../common/app.mk
