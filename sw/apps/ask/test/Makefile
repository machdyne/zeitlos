# Host build of the index engine, for testing against a real pack.
#
# Compiles aidx.c with -DASK_HOST, which swaps the six-function file
# shim from fs_* to stdio. Everything else is the code that ships.
#
#   make && ./hosttest ../../../../tools/ask/out/arklite "how do I purify water"
#
# loadtest checks the step-at-a-time loader (see loadtest.c): built
# with AddressSanitizer, and with aidx.c's malloc/free counted, so a
# load stopped at any step is checked to hold nothing.
#
#   make loadtest && ./loadtest ../../../../tools/ask/out/zdocs

CFLAGS = -std=c99 -Wall -Wextra -Wno-unused-parameter -O2 -g -DASK_HOST
ASAN = -fsanitize=address,undefined -fno-omit-frame-pointer
TARGET = hosttest

all: $(TARGET) loadtest

$(TARGET): hosttest.c ../aidx.c ../aidx.h
	$(CC) $(CFLAGS) -o $@ hosttest.c ../aidx.c

loadtest: loadtest.c ../aidx.c ../aidx.h
	$(CC) $(CFLAGS) $(ASAN) -Dmalloc=t_malloc -Dfree=t_free -c -o loadtest_aidx.o ../aidx.c
	$(CC) $(CFLAGS) $(ASAN) -o $@ loadtest.c loadtest_aidx.o
	rm -f loadtest_aidx.o

clean:
	rm -f $(TARGET) loadtest loadtest_aidx.o

# `make test` (and the top-level `make test`): the pack format's
# lens.zln check always; the loader against real packs when ASK_PACKS
# names one or more built card roots (each holding data/ask/<pack>/),
# since building a pack needs tools/ask's own setup:
#
#   make test ASK_PACKS="/tmp/zdocs-card /tmp/two-pack-card"
ASK_PACKS ?=
test: loadtest
	python3 -I ../../../../tools/ask/tests/test_lens.py $(ASK_PACKS)
	@if [ -z "$(ASK_PACKS)" ]; then echo "loadtest: skipped (no ASK_PACKS)"; \
	else for p in $(ASK_PACKS); do ./loadtest $$p || exit 1; done; fi

.PHONY: all clean test
