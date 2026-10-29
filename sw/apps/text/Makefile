# sw/apps/text -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = text

# A core app in the flash archive: integer-only printf, about 57KB
# smaller in flash and in RAM. It prints no floating point. docs/build.md,
# "Integer-only printf (ZFMT)".
ZFMT = 1

# ...and stdout without newlib's stdio (docs/build.md, "ZFMT_LITE"):
# text's only output is printf to the console log, and it never calls
# fflush(). That takes newlib's FILE layer, its buffering and the
# realloc behind it out of the link -- about 7.5KB of flash and of RAM,
# which paid for Replace, Go to line, Save As and Windows-1252 in 0.0.7
# with room to spare (docs/text_editor.md, "Size").
ZFMT_LITE = 1

# ...and no 64-bit division: text prints no 64-bit number (app.mk,
# ZFMT_NO64), which keeps 2.3KB of libgcc out of a core app.
ZFMT_NO64 = 1

# zfsapp.o is needed by zflist.o (the file browser inside the dialogs)
# as well as by text.c itself, and zdialog.o/zflist.o are what make
# this the first app to link either.

# Objects, and why each is linked:
#
#   zspeak.o: Super+A reads the document aloud -- docs/tts.md
#
#   zedit.o: sw/common/zedit.c -- the single-line text field.
#
#   zdialog.c owned it privately until sw/apps/web's URL bar became its
#   second caller. Every app that links the dialog needs this too: a
#   shared widget is only shared if the build says so.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o \
	zfsapp.o zflist.o zdialog.o zedit.o zspeak.o zsayall.o \
	zbigline.o zbigfont_10x20.o text.o

# -- host tests: `make test` here, or `make test` at the top --
#
# Both build the REAL text.c over real zwin/zwidget/zdialog and a
# scripted kernel (needs -no-pie and vm.mmap_min_addr=0; each exits 77
# to skip without them):
#   tests/utf8.c       encodings, loading and saving (via NAME.part),
#                      wrapping, autosave and its untitled copies
#   tests/test_edit.c  the arena, undo and redo, and the clipboard, with
#                      an arena that moves on every resize
HOSTCC ?= cc
HOSTSRC = ../../common/zwin.c ../../common/zwidget.c ../../common/zflist.c \
	../../common/zedit.c ../../common/zfsapp.c ../../common/zfont_data.c \
	../../common/zobj.c ../../common/zeitlos.c ../../common/zspeak.c \
	../../common/zsayall.c ../../common/zdialog.c ../../common/zbigline.c \
	../../common/zbigfont_10x20.c
HOSTFLAGS = -std=gnu99 -Wall -Wno-comment -no-pie -g -fsanitize=address,undefined -I ../../common

test:
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/text_test_utf8 tests/utf8.c $(HOSTSRC)
	$(HOSTCC) $(HOSTFLAGS) -o /tmp/text_test_edit tests/test_edit.c $(HOSTSRC)
	/tmp/text_test_utf8
	/tmp/text_test_edit

.PHONY: test

include ../../common/app.mk
