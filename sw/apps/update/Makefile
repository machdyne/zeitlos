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
	zdns.o zfsapp.o zcfg.o zsha256.o zexec_lz4.o monocypher.o \
	monocypher-ed25519.o upd.o upd_net.o update.o

# Signatures (docs/distribution.md): Monocypher's Ed25519 check, as web
# uses it.
SRC_DIRS = ../../ext/monocypher

# Stack and heap. No manifest is held in memory any more: they are read
# from the card a line at a time (upd.c, "the manifests"), and the heap
# holds the plan, a byte a file -- 1,680 bytes for the 1,680 files of
# every set with the ask packs, whose manifest is 200 KB, and which ran
# the 512K this used to have out when all three manifests were held
# (October 2026). `tests/test_update --update` prints the peak. Static
# buffers, in .bss: 64 KB for LZ4 blocks, 36 KB for HTTP (upd_net.c),
# two manifest readers and headers, 8 KB for writes.
APP_STACK = 64K

# upd.c includes "zsha256.h" plainly, so the host tests can build it.
APP_CFLAGS = -I../../common -I../../ext/monocypher

# The distribution update follows by default: zdist.h, made from
# release/dist.cfg and committed (docs/distribution.md). The build stops
# while it is stale, as it does for sw/common/zabi.h.
update.o: zdist.h | check-dist
check-dist:
	@python3 ../../../release/lib/dist.py --check zdist.h
dist:
	python3 ../../../release/lib/dist.py --header zdist.h
.PHONY: check-dist dist

clean: clean-tests
clean-tests: ; $(MAKE) -C tests clean

include ../../common/app.mk
