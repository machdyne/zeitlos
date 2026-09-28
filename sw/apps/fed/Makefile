# sw/apps/fed -- zfed. docs/fed.md.
#
#   make            the Zeitlos app (build rules: sw/common/app.mk)
#   make linux      the Linux daemon, ./fed-linux
#   make test       host tests: objects (against an independent Python
#                   implementation, which needs python3 with `cryptography`
#                   for the check in the other direction; skipped without
#                   it), the store, sessions, the handshake, the node, node
#                   lists -- and zeitlos/fed.c against a scripted kernel
#                   and a real Linux fed
#   make live       two Linux daemons over TCP, and a third joining

APP = fed
APP_LISTING = 0

# The core, the Zeitlos platform, and the vendored crypto -- ML-KEM's
# SHA-3 from sw/common/zfips202.c on zkeccak.c (the Keccak block when
# there is one), not ML-KEM's own fips202.c.
SRC_DIRS = core zeitlos ../../ext/monocypher ../../ext/mlkem
MLKEM_O = kem.o indcpa.o polyvec.o poly.o ntt.o cbd.o reduce.o verify.o symmetric-shake.o
# zwin for z_launch_arg_take() alone -- how posix hands `run fed nodes`
# its command (core/fadmin.c). It pulls in zgfx/zfont_data, which
# --gc-sections then drops almost all of (as zcc's Makefile notes).
OBJS = zeitlos.o zobj.o zport.o zwin.o zgfx.o zfont_data.o zcfg.o zfsapp.o zrng.o zsha256.o zjson.o z25519.o \
	zmlkem.o zfips202.o zkeccak.o monocypher.o monocypher-ed25519.o $(MLKEM_O) \
	fobj.o fstore.o fsess.o fnet.o fmail.o fradio.o fnode.o fadmin.o zplat_zeitlos.o fed.o
override CFLAGS += -I../../common -Icore -DKYBER_K=3

MONO = ../../ext/monocypher
COMMON = ../../common
MLKEM = ../../ext/mlkem
MLKEM_SRC = $(COMMON)/zmlkem.c $(COMMON)/zfips202.c $(COMMON)/zkeccak.c \
	$(patsubst %,$(MLKEM)/%.c,kem indcpa polyvec poly ntt cbd reduce verify symmetric-shake)
HOST_SRC = core/fobj.c core/fstore.c core/fsess.c core/fnet.c core/fmail.c core/fradio.c $(COMMON)/zplat_posix.c $(COMMON)/zjson.c $(MLKEM_SRC) -DKYBER_K=3 $(COMMON)/zsha256.c $(COMMON)/z25519.c \
	$(MONO)/monocypher.c $(MONO)/monocypher-ed25519.c

.PHONY: test linux live

# fed on Linux: a daemon (linux/main.c). docs/fed.md.
linux:
	cc -std=gnu99 -O2 -Wall -Wextra -o fed-linux linux/main.c core/fnode.c core/fadmin.c $(HOST_SRC)

# Two daemons on this machine, over TCP, with local clients: tests/live_fed.py
live: linux
	python3 tests/live_fed.py ./fed-linux
	python3 tests/live_mail.py ./fed-linux
	python3 tests/live_cancel.py ./fed-linux
	python3 tests/live_profile.py ./fed-linux
	python3 tests/live_admin.py ./fed-linux
test:
	cc -std=c99 -g -Wall -Wextra -fsanitize=address,undefined -o /tmp/test_fobj tests/test_fobj.c $(HOST_SRC)
	cd tests && /tmp/test_fobj
	cc -std=c99 -g -Wall -Wextra -fsanitize=address,undefined -o /tmp/test_fstore tests/test_fstore.c $(HOST_SRC)
	/tmp/test_fstore
	cc -std=c99 -g -Wall -Wextra -fsanitize=address,undefined -o /tmp/test_fsess tests/test_fsess.c $(HOST_SRC)
	/tmp/test_fsess
	cc -std=c99 -g -Wall -Wextra -fsanitize=address,undefined -o /tmp/test_handshake tests/test_handshake.c $(HOST_SRC)
	/tmp/test_handshake
	cc -std=c99 -g -Wall -Wextra -fsanitize=address,undefined -o /tmp/test_fnode tests/test_fnode.c core/fnode.c $(HOST_SRC)
	/tmp/test_fnode
	cc -std=c99 -g -Wall -Wextra -fsanitize=address,undefined -o /tmp/test_fnet tests/test_fnet.c core/fnet.c $(COMMON)/zjson.c
	/tmp/test_fnet
	cc -std=c99 -O1 -Wall -I core -I $(COMMON) -o /tmp/test_fmail tests/test_fmail.c $(HOST_SRC)
	/tmp/test_fmail
	cc -std=gnu99 -O1 -Wall -Wextra -I core -I $(COMMON) -o /tmp/test_fradio tests/test_fradio.c $(HOST_SRC)
	/tmp/test_fradio
	$(MAKE) --no-print-directory linux
	cc -std=gnu99 -g -Wall -no-pie -I core -I $(COMMON) -o /tmp/test_zfed tests/test_zfed.c \
		core/fnode.c core/fadmin.c $(HOST_SRC) $(COMMON)/zport.c $(COMMON)/zobj.c $(COMMON)/zeitlos.c $(COMMON)/zcfg.c
	@/tmp/test_zfed ./fed-linux; r=$$?; if [ $$r -eq 77 ]; then \
		echo "(zeitlos/fed.c's test needs vm.mmap_min_addr=0 -- skipped)"; elif [ $$r -ne 0 ]; then exit $$r; fi
	cc -std=gnu99 -g -Wall -no-pie -I core -I $(COMMON) -o /tmp/test_zadmin tests/test_zadmin.c \
		core/fnode.c core/fadmin.c $(HOST_SRC) $(COMMON)/zport.c $(COMMON)/zobj.c $(COMMON)/zeitlos.c $(COMMON)/zcfg.c
	/tmp/test_zadmin ./fed-linux
	@if python3 -c "import cryptography" 2>/dev/null; then cd tests && python3 check_fobj.py; \
	else echo "(check_fobj.py needs python3 with cryptography -- skipped)"; fi

CLEAN_EXTRA = fed-linux
include ../../common/app.mk
