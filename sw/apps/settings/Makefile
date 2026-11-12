# sw/apps/settings -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = settings

# A window, a radio group and three buttons -- plus what editing
# /sys/zeitlos.cfg takes: the whole-file API (zfsapp), the text-prompt
# dialog and what it links (zdialog, zflist, zedit, ztype), the config
# parser/editor (zcfg) and time-zone parsing, to check a zone before
# saving it (zrtc). See docs/config.md.

# Objects, and why each is linked:
#
#   zkbd.o: the keyboard layout names, for checking system.keyboard.layouts
#   zaccent.o: the desktop colour themes, for the Desktop colours row
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zdialog.o zflist.o zedit.o ztype.o zcfg.o zrtc.o zkbd.o zaccent.o zaccent_names.o settings.o

# Host test: the real settings.c with a scripted kernel and file, every
# page rendered (tests/render.c). Needs vm.mmap_min_addr=0; skipped
# (exit 77) where it cannot map address 0.
HOSTCC ?= cc
C = ../../common
.PHONY: test
test:
	$(HOSTCC) -std=gnu99 -w -no-pie -I $(C) -o /tmp/settings_render tests/render.c $(C)/zwin.c \
		$(C)/zwidget.c $(C)/zfont_data.c $(C)/zobj.c $(C)/zeitlos.c $(C)/zfsapp.c $(C)/zcfg.c \
		$(C)/zrtc.c $(C)/zkbd.c $(C)/zspeak.c $(C)/zaccent.c $(C)/zaccent_names.c $(C)/zi18n.c
	mkdir -p /tmp/settings-render
	/tmp/settings_render /tmp/settings-render/s || [ $$? -eq 77 ]

include ../../common/app.mk
