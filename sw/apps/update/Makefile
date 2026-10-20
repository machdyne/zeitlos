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
	zdns.o zfsapp.o zsha256.o zexec_lz4.o upd.o upd_net.o update.o

# Stack and heap. The heap holds both manifests, their parsed entries,
# the plan and the card's new manifest: measured at 126-138 KB at the
# peak on the host for the 472-file base card of October 2026 (64-bit
# pointers; less here; `tests/test_update --update` prints it), and the
# host tests run it out of memory at every point (tests/test_update.c,
# "memory"). 256K ran out on a board once; 512K leaves room to grow.
# Static buffers, in .bss: two 32 KB for LZ4 blocks, 8 KB for writes.
APP_STACK = 512K

# upd.c includes "zsha256.h" plainly, so the host tests can build it.
APP_CFLAGS = -I../../common

clean: clean-tests
clean-tests: ; $(MAKE) -C tests clean

include ../../common/app.mk
