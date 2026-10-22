# sw/apps/backup -- dated backups on a second drive (docs/backup.md).
# Build rules are shared: sw/common/app.mk, docs/build.md.
#
# bk.c is the work, portable and tested on the host
# (`make -C sw/apps/backup/tests`); backup.c binds it to fs, the clock
# and the terminal.

APP = backup

# Integer-only printf (docs/build.md): bk.c prints no floating point
# and no 64-bit numbers (u64str() for the one it writes), through
# vsnprintf only -- so no FILE layer either.
ZFMT = 1
ZFMT_LITE = 1
ZFMT_NO64 = 1

# zfsc.o: straight to the fs driver -- it serves every volume, so the
# backup drive's free space (STATVFS "/usb") and eject (UMOUNT) are
# there, and listings come a page at a time (OPENDIR/READDIR).
OBJS = zeitlos.o zobj.o zport.o zwin.o zgfx.o zfont_data.o zfsc.o zcfg.o \
	zwidget.o zspeak.o zfsapp.o zdialog.o zflist.o zedit.o ztype.o zco.o \
	zrtc.o bk.o bk_zfs.o bkjob.o bkwin.o bkwatch.o backup.o

# Stack and heap: the newest backup's index and the card's file list,
# about 100 bytes a file (a 2,000-file card: ~200 KB), the index file
# itself while it is read (~80 bytes a file), and the folder listings
# of one walk. 512K covers ~2,500 files; past that, "out of memory",
# and nothing is written. On a 1MB board the kernel refuses to start it
# (the RAM rule leaves no room for 512K beside the boot set).
APP_STACK = 512K

# No write-behind in zfsc.c: each 4 KB write is its own request to fs,
# ~22 ms to a stick, and the window hands back after each one
# (bkjob.c). With it, two writes go as one 8 KB request, ~45 ms, and a
# slice can make two requests (tests/test_job.c measures it).
APP_CFLAGS = -DZFSC_WBUF=0

clean: clean-tests
clean-tests: ; $(MAKE) -C tests clean

include ../../common/app.mk
