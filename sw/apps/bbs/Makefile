# sw/apps/bbs -- the BBS. docs/bbs.md, docs/bbs_linux.md.
#
#   make            the Zeitlos app (build rules: sw/common/app.mk, docs/build.md)
#   make linux      the Linux server, ./bbs-linux
#   make test       host tests: the core on the Linux platform (twice: 16KB
#                   and 4KB rings), then bbs.c against a scripted kernel

APP = bbs
APP_LISTING = 0

# The core, and the platform from sw/common (zplat_zeitlos.c).
SRC_DIRS = core
CORE = cfg.o out.o session.o text.o users.o msgbase.o reader.o fedlink.o
OBJS = zeitlos.o zobj.o zport.o zcfg.o zfsapp.o zrng.o zsha256.o zjson.o zetta.o $(CORE) zplat_zeitlos.o bbs.o

# 4 KB of output per caller, and 6 KB for a message being written, on
# the board (docs/bbs.md, "Memory").
override CFLAGS += -I../../common -Icore -DBBS_OUT_RING=4096 -DMSG_BODY_MAX=6144

HOSTCC ?= cc
HOST_SRC = core/*.c ../../common/zsha256.c ../../common/zjson.c ../../common/zetta.c
HOST_CFLAGS = -std=c99 -Wall -Wextra -I core -I ../../common

.PHONY: linux test live
# Two BBS nodes over zfed, callers on telnet (tests/live_fed.py)
live: linux
	$(MAKE) -C ../fed linux
	python3 tests/live_fed.py ./bbs-linux ../fed/fed-linux
linux:
	$(HOSTCC) -O2 $(HOST_CFLAGS) -DBBS_OUT_RING=16384 -o bbs-linux linux/main.c ../../common/zplat_posix.c $(HOST_SRC)

test:
	$(HOSTCC) -g $(HOST_CFLAGS) -fsanitize=address,undefined -o /tmp/test_bbs \
		tests/test_bbs.c ../../common/zplat_posix.c $(HOST_SRC) ../../common/zvt100.c
	/tmp/test_bbs
	$(HOSTCC) -g $(HOST_CFLAGS) -DBBS_OUT_RING=4096 -DMSG_BODY_MAX=6144 -o /tmp/test_bbs_small \
		tests/test_bbs.c ../../common/zplat_posix.c $(HOST_SRC) ../../common/zvt100.c
	/tmp/test_bbs_small
	$(HOSTCC) -std=gnu99 -g -Wall -no-pie -I core -I ../../common -o /tmp/test_bbs_zport \
		tests/test_zport.c ../../common/zplat_posix.c $(HOST_SRC) ../../common/zport.c \
		../../common/zobj.c ../../common/zeitlos.c ../../common/zcfg.c
	@/tmp/test_bbs_zport; r=$$?; if [ $$r -eq 77 ]; then \
		echo "(bbs.c's test needs vm.mmap_min_addr=0 -- skipped)"; elif [ $$r -ne 0 ]; then exit $$r; fi
	$(HOSTCC) -std=gnu99 -g -Wall -no-pie -I core -I ../../common -o /tmp/test_bbs_zfed \
		tests/test_zport_fed.c ../../common/zplat_posix.c $(HOST_SRC) ../../common/zport.c \
		../../common/zobj.c ../../common/zeitlos.c ../../common/zcfg.c
	@/tmp/test_bbs_zfed; r=$$?; if [ $$r -eq 77 ]; then \
		echo "(bbs.c's fed-link test needs vm.mmap_min_addr=0 -- skipped)"; elif [ $$r -ne 0 ]; then exit $$r; fi

CLEAN_EXTRA = bbs-linux

# 64KB. Each node is about 5KB, four by default, and a caller can
# have eight 512-byte sends in flight until they are acked.
APP_STACK = 64K

include ../../common/app.mk
