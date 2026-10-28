# sw/apps/draw -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = draw

# zfsapp.o/zflist.o/zdialog.o are the file dialogs (new/open/save) --
# zflist.o needs zfsapp.o, and draw.c needs both plus zdialog.o.

# Objects, and why each is linked:
#
#   zedit.o: sw/common/zedit.c -- the single-line text field.
#
#   zdialog.c owned it privately until sw/apps/web's URL bar became its
#   second caller. Every app that links the dialog needs this too: a
#   shared widget is only shared if the build says so.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zflist.o zdialog.o zedit.o draw_icons.o drawio.o \
	drawstamp.o drawundo.o draw.o

# draw_icons.c/.h are hand-edited (binary literals with an ASCII
# picture of each row in the comment), not generated -- see
# draw_icons.h's own header comment.
draw_icons.o: draw_icons.c draw_icons.h
	$(CC) $(CFLAGS) -c draw_icons.c -o draw_icons.o

include ../../common/app.mk

# -- host tests --------------------------------------------------------------
#
#   make test     the load and save state machines (drawio.c): slicing,
#                 stopping at every step, the .part, damaged files;
#                 then the stamps (drawstamp.c: word-at-a-time against
#                 the old per-pixel ones, on random strokes) and undo
#                 (drawundo.c: the page back byte for byte, both forms,
#                 the limits)

HOSTCC ?= cc
HOST_CFLAGS = -std=gnu99 -g -Wall -Wextra -fsanitize=address,undefined \
	-fno-sanitize=shift

test:
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/test_draw_io tests/test_drawio.c tests/hostfs.c drawio.c
	/tmp/test_draw_io
	$(HOSTCC) $(HOST_CFLAGS) -o /tmp/test_draw_undo tests/test_draw.c drawstamp.c drawundo.c
	/tmp/test_draw_undo
