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
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zdialog.o zflist.o zedit.o ztype.o zcfg.o zrtc.o zkbd.o settings.o

include ../../common/app.mk
