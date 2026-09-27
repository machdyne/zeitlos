# sw/apps/clock -- build rules are shared: sw/common/app.mk, docs/build.md.

APP = clock
APP_MAIN_RULE = 0

# A small app: a window, three widgets, and no filesystem, dialogs or
# document of any kind. zrtc.o is the one thing here that isn't in
# every app's list -- it's the calendar arithmetic behind the date
# line and the hour hand.
# zcfg.o: system.rtc.timezone -- docs/config.md
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o zrtc.o zcfg.o clock.o

clock.o: clock.c ../../common/zrtc.h ../../common/zntp.h
	$(CC) $(CFLAGS) -c clock.c -S -MF /dev/null -o clock.asm
	$(CC) $(CFLAGS) -c clock.c -o clock.o

# Produces a ZEXE rather than a raw --pad-to binary, same as every
# other app here -- the header carries .bss as a NUMBER for the loader
# to memset instead of shipping it as literal zeros in the file. See
# sw/common/zexec.h and tools/mkexec.py.

include ../../common/app.mk
