# sw/apps/backup/tests -- backup's core on the host (docs/backup.md).
#
#   make -C sw/apps/backup/tests          build and run
#   make -C sw/apps/backup/tests clean
#
# test_bk drives bk.c over memfs, a card and a backup drive in memory
# that lose their power wherever a test says. test_fat drives bk.c and
# bk_zfs.c (the board's file layer) through zfsc.c to the real fs
# driver and FatFs, on a FAT32 image made by mkfs.fat (dosfstools),
# with the power cut at every sector write. test_read.py checks
# tools/backup_read.py (the PC side) against a drive test_bk made.

CC ?= cc
CFLAGS = -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter \
	-fsanitize=address,undefined -fno-omit-frame-pointer -I..
DUMP = $(CURDIR)/drive

all: run

COMMON = ../../../common
FS = ../../fs
FAT_CFLAGS = -std=gnu99 -O1 -g -Wall -Wno-unused-function \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I.. -I$(COMMON) -I../../net -DFS_HOST_TEST -DZFSAPP_DIRECT -DZPOSIX_HOST_TEST \
	'-DFS_IN_WINDOW(d,n,b,s)=1' -DZFSC_WBUF=0
FAT_SRC = $(FS)/fs.c $(COMMON)/zfsc.c $(COMMON)/zfsapp.c $(COMMON)/zposix.c \
	$(FS)/fatfs/ff.c $(FS)/fatfs/ffunicode.c ../bk.c ../bk_zfs.c

test_fat: test_fat.c $(FS)/tests/test_fs.c $(FAT_SRC) ../bk.h ../bk_zfs.h
	$(CC) $(FAT_CFLAGS) -o $@ test_fat.c $(FAT_SRC)

test_bk: test_bk.c memfs.c memfs.h ../bk.c ../bk.h
	$(CC) $(CFLAGS) -o $@ test_bk.c memfs.c ../bk.c

# backup.c itself, command by command (test_cli.c includes it)
test_cli: test_cli.c ../backup.c ../bkwatch.c $(FS)/tests/test_fs.c $(FAT_SRC) ../bk.h ../bk_zfs.h
	$(CC) $(FAT_CFLAGS) -o $@ test_cli.c ../bkwatch.c $(FAT_SRC) $(COMMON)/zrtc.c

# bkjob.c: the engine a slice at a time, as the window runs it
test_job: test_job.c ../bkjob.c ../bkjob.h $(COMMON)/zco.c $(FS)/tests/test_fs.c $(FAT_SRC) ../bk.h ../bk_zfs.h
	$(CC) $(FAT_CFLAGS) -o $@ test_job.c ../bkjob.c $(COMMON)/zco.c $(FAT_SRC)

# bkwatch.c: watching through fs's watches, and the watch backups
test_watch: test_watch.c ../bkwatch.c ../bkwatch.h $(FS)/tests/test_fs.c $(FAT_SRC) ../bk.h ../bk_zfs.h
	$(CC) $(FAT_CFLAGS) -o $@ test_watch.c ../bkwatch.c $(FAT_SRC)

# The window, view by view, as pictures (tests/render/*.pbm) and its
# keys and buttons. Needs vm.mmap_min_addr=0 (sw/common/tests/ztramp.h);
# exits 77 (skipped) without it.
render_win: render.c ../bkwin.c ../bkwin.h ../bkjob.h ../bkwatch.c ../bkwatch.h
	$(CC) -std=gnu99 -Wall -Wno-unused-function -O0 -g -no-pie -I.. -I$(COMMON) -o $@ render.c ../bkwatch.c \
		$(COMMON)/zwin.c $(COMMON)/zwidget.c $(COMMON)/zfont_data.c $(COMMON)/zobj.c \
		$(COMMON)/zeitlos.c $(COMMON)/zrtc.c $(COMMON)/zspeak.c ../bk.c
render: render_win
	./render_win render

run: test_bk test_fat test_cli test_job test_watch
	./test_bk
	./test_fat | grep -v '^fs: '
	./test_cli | grep -v '^fs: '
	./test_job | grep -v '^fs: '
	./test_watch | grep -v '^fs: '
	rm -rf $(DUMP)
	./test_bk --dump $(DUMP)
	python3 -I test_read.py ../../../../tools/backup_read.py $(DUMP)

clean:
	rm -rf test_bk test_fat test_cli test_job test_watch render_win render $(DUMP)

.PHONY: all run clean render
