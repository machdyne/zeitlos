# sw/apps/launcher -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# The console profile's front end (docs/profiles.md): what wm starts in
# place of the desktop on a console bitstream -- a menu of /apps in game
# mode. docs/launcher_app.md.

APP = launcher
APP_LISTING = 0

SRC_DIRS = $(COMMON_DIR)/games $(COMMON_DIR)/games/ui

OBJS = zeitlos.o zobj.o zgfx.o zcolor.o zfont_data.o zwin.o zwidget.o \
	zspeak.o zdialog.o zfsapp.o \
	zarcade.o zarcade_sys.o zsfx.o zcfg.o sui.o launcher.o

include ../../common/app.mk
