# sw/apps/text -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = text

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
	zfsapp.o zflist.o zdialog.o zedit.o zspeak.o zsayall.o text.o

include ../../common/app.mk
