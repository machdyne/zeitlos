# Host build of the index engine, for testing against a real pack.
#
# Compiles aidx.c with -DASK_HOST, which swaps the six-function file
# shim from fs_* to stdio. Everything else is the code that ships.
#
#   make && ./hosttest ../../../../tools/ask/out/arklite "how do I purify water"

CFLAGS = -std=c99 -Wall -Wextra -Wno-unused-parameter -O2 -g -DASK_HOST
TARGET = hosttest

$(TARGET): hosttest.c ../aidx.c ../aidx.h
	$(CC) $(CFLAGS) -o $@ hosttest.c ../aidx.c

clean:
	rm -f $(TARGET)

.PHONY: clean
