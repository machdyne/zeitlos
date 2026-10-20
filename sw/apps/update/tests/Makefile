# sw/apps/update/tests -- update's core on the host (docs/update.md).
#
#   make -C sw/apps/update/tests          build, make the fixture, run
#   make -C sw/apps/update/tests clean
#
# The fixture (two releases and their store) is made by mkfixture.py
# with release/lib's own manifest.py and store.py, so this checks the
# board's reader against what `zrelease tftp` writes.

CC ?= cc
COMMON = ../../../common
CFLAGS = -std=gnu99 -O1 -g -Wall -Wextra -Wno-unused-parameter \
	-fsanitize=address,undefined -fno-omit-frame-pointer \
	-I.. -I$(COMMON)
FIXTURE = $(CURDIR)/fixture

all: run

# upd.c's allocations are counted (alloc_hook.h): the peak a run needs,
# and a heap that runs out as the board's does.
test_update: test_update.c ../upd.c ../upd.h $(COMMON)/zsha256.c $(COMMON)/zexec_lz4.c alloc_hook.h
	$(CC) $(CFLAGS) -c -include alloc_hook.h -o upd_host.o ../upd.c
	$(CC) $(CFLAGS) -o $@ test_update.c upd_host.o $(COMMON)/zsha256.c \
		$(COMMON)/zexec_lz4.c

# The mailbox and TFTP source, with the real stream and object code.
test_net: test_net.c ../upd_net.c ../upd_net.h $(COMMON)/zstream.c $(COMMON)/zobj.c
	$(CC) $(CFLAGS) -o $@ test_net.c ../upd_net.c $(COMMON)/zstream.c \
		$(COMMON)/zobj.c -lm

# Stopping the run at every step longjmps out of it, so its allocations
# are not freed on those runs: leak checking would report each one. In
# test_net the leaks are net's borrowed error strings (zstream_reject()
# and zstream_send_error() allocate, as they do on the board) and the
# one request update keeps when net never answers it.
run: test_update test_net
	ASAN_OPTIONS=detect_leaks=0 ./test_net
	python3 -I mkfixture.py $(FIXTURE)
	ASAN_OPTIONS=detect_leaks=0 ./test_update $(FIXTURE)

clean:
	rm -rf test_update test_net upd_host.o fixture

.PHONY: all run clean
