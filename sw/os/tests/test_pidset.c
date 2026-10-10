/*
 * Host test for sw/os/kpidset.h at several Z_PROCS_MAX:
 *
 *   for n in 32 64 100 128 256; do
 *     cc -std=gnu99 -Wall -DZ_PROCS_MAX=$n -o /tmp/test_pidset \
 *        sw/os/tests/test_pidset.c && /tmp/test_pidset || break; done
 *
 * k_pidset_next2() is checked against the slot walk it replaced, for
 * random sets and every starting pid.
 */

#include <stdio.h>
#include <stdlib.h>

#include "../kpidset.h"

static int failed, run;

// the old scheduler loop: z_pid++ with wrap, until a candidate
static uint32_t walk(uint32_t cur, const bool *in) {
	for (uint32_t n = 0; n < Z_PROCS_MAX; n++) {
		cur = (cur + 1 < Z_PROCS_MAX) ? cur + 1 : 0;
		if (in[cur]) return cur;
	}
	return Z_PROCS_MAX;
}

int main(void) {
	srand(1);
	for (int t = 0; t < 4000; t++) {
		k_pidset_t a, b;
		bool in[Z_PROCS_MAX];
		uint32_t cnt = 0;
		k_pidset_clear(&a); k_pidset_clear(&b);
		int density = rand() % 4;	// empty-ish to full-ish
		for (uint32_t p = 0; p < Z_PROCS_MAX; p++) {
			bool x = density && (rand() % 8) < density;
			bool y = density && (rand() % 16) == 0;
			k_pidset_put(&a, p, x);
			k_pidset_put(&b, p, y);
			in[p] = x || y;
			cnt += x;
		}
		run++;
		if (k_pidset_count(&a) != cnt) { failed++; printf("count\n"); }
		for (uint32_t cur = 0; cur < Z_PROCS_MAX; cur++) {
			run++;
			uint32_t want = walk(cur, in), got = k_pidset_next2(cur, &a, &b);
			if (want != got) {
				failed++;
				if (failed < 10) printf("FAIL cur %u: want %u got %u\n", cur, want, got);
			}
		}
	}
	printf("test_pidset Z_PROCS_MAX=%d: %d checks, %d failed\n", Z_PROCS_MAX, run, failed);
	return failed != 0;
}
