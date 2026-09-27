# sw/apps/files -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = files

# zflist.o is the browsing widget this app is mostly made of, zdialog.o
# the confirm/prompt dialogs, ztype.o the extension-to-app table.

# Objects, and why each is linked:
#
#   zedit.o: sw/common/zedit.c -- the single-line text field.
#
#   zdialog.c owned it privately until sw/apps/web's URL bar became its
#   second caller. Every app that links the dialog needs this too: a
#   shared widget is only shared if the build says so.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zfsutil.o zflist.o zdialog.o zedit.o ztype.o files.o

include ../../common/app.mk
