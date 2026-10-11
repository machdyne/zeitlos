# sw/apps/cron -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = cron

# cron writes a line now and then: no write-behind buffer, and no
# malloc pulled in for one (~5KB).
APP_CFLAGS = -DZFSAPP_WBUF=0

OBJS = zeitlos.o zobj.o zfsapp.o zcfg.o zrtc.o zwin.o zgfx.o zfont_data.o cron.o

include ../../common/app.mk
