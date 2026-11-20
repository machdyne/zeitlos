# sw/apps/cinema -- build rules are shared: sw/common/app.mk, docs/build.md.
#
# The film player (docs/cinema_app.md): ZV files (docs/zv.md), made on
# another computer with tools/zv/zvenc. Not yet in sw/apps/Makefile's
# APPS: `make -C sw/apps/cinema` and copy cinema.bin to the card's /apps.

APP = cinema

# zv.c is the program's inner loop: -O2 for it alone (the rest -Os)
zv.o:
	$(CC) $(CFLAGS) -O2 -c ../../common/zv.c -o zv.o

# zfsapp/zflist/zdialog/zedit: the open dialog and the help box
OBJS = zeitlos.o zobj.o zgfx.o zfont_data.o zwin.o zwidget.o zspeak.o \
	zfsapp.o zflist.o zdialog.o zedit.o zaccent.o zv.o cinema.o

# the records waiting (256K), two pictures (80K), the sound ring (16K)
# are .bss; the open dialog's file list wants a little heap
APP_STACK = 128K

include ../../common/app.mk
