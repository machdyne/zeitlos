# sw/common/tests/sim_chacha -- ChaCha20 with and without the CPU's
# rotate instructions (`CPU_ROT, docs/chacha.md), in the simulator:
#
#   make -C sw/common/tests/sim_chacha
#   sim/zsim-headless -n 0 sw/common/tests/sim_chacha/chachatest.bin             (shifts)
#   ZSIM_ROT=1 sim/zsim-headless -n 0 sw/common/tests/sim_chacha/chachatest.bin  (rori)
#
# Both must print the RFC 8439 vectors' results as "ok"; the instruction
# counts say what the rotates save. `make test` runs both and checks.
APP = chachatest
SRC_DIRS = $(COMMON_DIR)/../ext/monocypher
OBJS = zeitlos.o zobj.o zrng.o monocypher.o chachatest.o
APP_STACK = 16K
APP_CFLAGS = -I$(COMMON_DIR)
include ../../app.mk

monocypher.o: $(COMMON_DIR)/../ext/monocypher/monocypher.c
	$(CC) $(CFLAGS) -DZ_CHACHA_ROUNDS=z_chacha20_rounds -c $< -o $@

test: $(APP).bin
	$(MAKE) -C ../../../../sim zsim-headless
	../../../../sim/zsim-headless -n 0 $(APP).bin | tee /tmp/chacha_shift.txt
	ZSIM_ROT=1 ../../../../sim/zsim-headless -n 0 $(APP).bin | tee /tmp/chacha_rot.txt
	grep -q "chacha: all ok, with shifts" /tmp/chacha_shift.txt
	grep -q "chacha: all ok, with rori" /tmp/chacha_rot.txt
.PHONY: test
