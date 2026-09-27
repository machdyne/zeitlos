# sw/apps/logic -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = logic

# About as small as an app in this tree gets: a window, a radio group,
# and no filesystem, dialogs or document of any kind.

# Objects, and why each is linked:
#
#   zsoc.o: z_soc_has_feature2() and the GPIO registers -- see logic.c.
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zsoc.o zgpio.o logic.o

include ../../common/app.mk
