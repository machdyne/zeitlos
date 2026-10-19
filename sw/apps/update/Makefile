# sw/apps/update -- bring the card up to date with a release
# (docs/update.md). Build rules are shared: sw/common/app.mk.
#
# upd.c is the work, portable and tested on the host
# (`make -C sw/apps/update/tests`); update.c binds it to fs, net and
# the terminal.

APP = update

# Integer-only printf (docs/build.md): update prints sizes and rates
# with no floating point and no 64-bit numbers, through vsnprintf
# only -- so no FILE layer either.
ZFMT = 1
ZFMT_LITE = 1
ZFMT_NO64 = 1

OBJS = zeitlos.o zobj.o zport.o zwin.o zgfx.o zfont_data.o zstream.o \
	zdns.o zfsapp.o zsha256.o upd.o upd_net.o update.o

# Stack and heap. The heap holds both manifests, their parsed entries,
# the plan and the card's new manifest: measured at ~140-210 KB on the
# host for the 408-file base card (64-bit pointers; less here), and the
# host tests hold it under a limit (tests/test_update.c, "memory").
# 256K ran out on a board once; 512K leaves room for the release to grow.
APP_STACK = 512K

# upd.c includes "zsha256.h" plainly, so the host tests can build it.
APP_CFLAGS = -I../../common

clean: clean-tests
clean-tests: ; $(MAKE) -C tests clean

include ../../common/app.mk
