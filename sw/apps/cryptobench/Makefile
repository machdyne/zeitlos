# sw/apps/cryptobench -- what the primitives cost here. docs/cryptobench.md.
#
#   make          the Zeitlos app
#   make test     a host build, checking the known answers and the
#                 montmul arithmetic against a software model

APP = cryptobench
APP_LISTING = 0

MONO = ../../ext/monocypher
OBJS = zeitlos.o zobj.o zfsapp.o zsha256.o monocypher.o monocypher-ed25519.o \
	o2_mono.o o2_ed.o o2_sha.o hwsha.o ecdsa.o z25519.o $(MLKEM_OBJS) cryptobench.o

# ML-KEM-768 (FIPS 203): sw/common/zmlkem.c over the vendored reference
# in sw/ext/mlkem (docs/mlkem.md)
MLKEM = ../../ext/mlkem
MLKEM_SRCS = kem indcpa polyvec poly ntt cbd reduce verify symmetric-shake
MLKEM_OBJS = zmlkem.o zfips202.o zkeccak.o $(patsubst %,mlkem_%.o,$(MLKEM_SRCS))
zmlkem.o: ../../common/zmlkem.c
	$(CC) $(CFLAGS) -DKYBER_K=3 -c $< -o $@
# SHA-3/SHAKE on zkeccak.c: the Keccak block when there is one
zfips202.o: ../../common/zfips202.c
	$(CC) $(CFLAGS) -DKYBER_K=3 -c $< -o $@
zkeccak.o: ../../common/zkeccak.c
	$(CC) $(CFLAGS) -c $< -o $@
mlkem_%.o: $(MLKEM)/%.c
	$(CC) $(CFLAGS) -DKYBER_K=3 -c $< -o $@

z25519.o: ../../common/z25519.c
	$(CC) $(CFLAGS) -c $< -o $@

# sw/apps/web's ECDSA, as web builds it: the montmul block, and its
# register file when the bitstream has one.
ecdsa.o: ../web/ecdsa.c
	$(CC) $(CFLAGS) -DEC_HW -c $< -o $@

override CFLAGS += -I../../common -I$(MONO)

# The two measured software copies of SHA-256 stay software: since the
# SHA-256 block (rtl/sha256.v), zsha256.c uses it wherever it can, and
# these lines are the comparison with it. hwsha.o is the driver as
# every app now gets it, renamed hw_*.
zsha256.o: ../../common/zsha256.c
	$(CC) $(CFLAGS) -DZ_SHA256_NO_HW -c $< -o $@
hwsha.o: ../../common/zsha256.c
	$(CC) $(CFLAGS) -Dz_sha256_init=hw_z_sha256_init -Dz_sha256_update=hw_z_sha256_update \
		-Dz_sha256_final=hw_z_sha256_final -Dz_sha256=hw_z_sha256 -c $< -o $@

monocypher.o: $(MONO)/monocypher.c
	$(CC) $(CFLAGS) -c $< -o $@
monocypher-ed25519.o: $(MONO)/monocypher-ed25519.c
	$(CC) $(CFLAGS) -c $< -o $@

# The same sources again at -O2 (the later flag wins over app.mk's -Os),
# every global symbol then renamed o2_*: one binary measures both.
# The list comes from all three objects together, so the calls between
# them (monocypher-ed25519.c into monocypher.c) are renamed alike.
o2raw_mono.o: $(MONO)/monocypher.c
	$(CC) $(CFLAGS) -O2 -c $< -o $@
o2raw_ed.o: $(MONO)/monocypher-ed25519.c
	$(CC) $(CFLAGS) -O2 -c $< -o $@
o2raw_sha.o: ../../common/zsha256.c
	$(CC) $(CFLAGS) -O2 -DZ_SHA256_NO_HW -c $< -o $@
o2.syms: o2raw_mono.o o2raw_ed.o o2raw_sha.o
	$(PREFIX)nm -g --defined-only $^ | awk 'NF == 3 { print $$3 " o2_" $$3 }' | sort -u > $@
o2_mono.o: o2raw_mono.o o2.syms
	$(PREFIX)objcopy --redefine-syms=o2.syms $< $@
o2_ed.o: o2raw_ed.o o2.syms
	$(PREFIX)objcopy --redefine-syms=o2.syms $< $@
o2_sha.o: o2raw_sha.o o2.syms
	$(PREFIX)objcopy --redefine-syms=o2.syms $< $@

.PHONY: test
test:
	cc -std=gnu99 -O1 -Wall -Wextra -I../../common -I$(MONO) -o /tmp/cryptobench_host \
		cryptobench.c tests/host.c ../../common/zsha256.c $(MONO)/monocypher.c $(MONO)/monocypher-ed25519.c \
		../web/ecdsa.c ../../common/z25519.c ../../common/zmlkem.c \
		$(patsubst %,../../ext/mlkem/%.c,kem indcpa polyvec poly ntt cbd reduce verify symmetric-shake) \
		../../common/zfips202.c ../../common/zkeccak.c \
		-DKYBER_K=3
	cd /tmp && /tmp/cryptobench_host

CLEAN_EXTRA = o2.syms o2raw_*.o
include ../../common/app.mk
