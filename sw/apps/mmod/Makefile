# sw/apps/mmod -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = mmod
APP_MAIN_RULE = 0

# Objects, and why each is linked:
#
#   zgfx.o: panel.c is INCLUDED by mmod.c rather than compiled separately, so
#   that sw/apps/mmod/tests/render.c can compile the panel without the
#   event loop and draw it on the build machine. See that file.
#
#   zsoc.o: The bit-bang SPI stack. No zport: this app talks to a chip, not to
#   another process.
#
#   zmmod.o: The device layer -- tested by sw/common/tests/test_mmod.c.
#
#   zfsapp.o: File I/O for READ and VERIFY.
#
#   zflist.o: The file dialogs. zflist.o needs zfsapp.o; zdialog.o needs both.
#
#   zedit.o: sw/common/zedit.c -- the single-line text field.
#
#   zdialog.c owned it privately until sw/apps/web's URL bar became its
#   second caller. Every app that links the dialog needs this too: a
#   shared widget is only shared if the build says so.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zsoc.o zgpio.o zspi.o zmmod.o zfsapp.o zflist.o zdialog.o zedit.o mmod.o

mmod.o: mmod.c panel.c
	$(CC) $(CFLAGS) -c mmod.c -o mmod.o
	$(CC) $(CFLAGS) -c mmod.c -o mmod.o

include ../../common/app.mk
