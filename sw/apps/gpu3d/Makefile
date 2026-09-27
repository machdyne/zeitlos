# sw/apps/gpu3d -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = gpu3d

# zfsapp.o is what stl.c reads files through, and is also needed by
# zflist.o (the file browser inside the dialog). zfont_data.o arrived
# with the FPS readout -- this app drew no text at all before.

# Objects, and why each is linked:
#
#   zedit.o: sw/common/zedit.c -- the single-line text field.
#
#   zdialog.c owned it privately until sw/apps/web's URL bar became its
#   second caller. Every app that links the dialog needs this too: a
#   shared widget is only shared if the build says so.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zflist.o zdialog.o zedit.o stl.o gpu3d.o

include ../../common/app.mk
