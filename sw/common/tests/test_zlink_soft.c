/*
 * Host test for sw/common/zlink_soft.c, with sw/common/zlink.c on top.
 *
 *   make -C sw/common/tests -f Makefile.zlink
 *
 * Two open-drain wires with pull-ups: a wire is low if either end pulls
 * it. Each end runs a whole zlink over a soft transport. The two are
 * stepped in a random interleaving, and each is now and then STALLED --
 * not polled at all for up to several milliseconds -- the way a process
 * is when the scheduler runs something else. The protocol has to make
 * progress anyway, and lose nothing.
 *
 * Cases: a straight cable and a crossed one (A's X is B's Y), with and
 * without stalls, both ends sending; and a case where B starts 2 s
 * late. The crossed cases check that both ends noticed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zlink.h"
#include "zlink_soft.h"

static bool pulled[2][2];		// [end][physical wire]
static bool crossed;

static int phys(int end, int w) {
	return (end == 1 && crossed) ? 1 - w : w;
}

static void pin_pull(void *ctx, int w, bool low) {
	int end = (int)(intptr_t)ctx;
	pulled[end][phys(end, w)] = low;
}

static bool pin_level(void *ctx, int w) {
	int end = (int)(intptr_t)ctx;
	int p = phys(end, w);
	return !(pulled[0][p] || pulled[1][p]);
}

static uint32_t seed = 1;
static uint32_t prng(void) {
	seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
	return seed;
}

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static zl_soft_t sa, sb;
static zlink_t A, B;

static void make_msg(uint8_t *buf, uint16_t *len, uint8_t chan, uint32_t n) {
	*len = (uint16_t)((n * 53 + chan * 7) % 200);
	for (uint16_t i = 0; i < *len; i++) buf[i] = (uint8_t)(n * 13 + i * 3 + chan);
	// make sure the escaped bytes turn up
	if (*len > 3) { buf[0] = ZL_SOFT_END; buf[1] = ZL_SOFT_ESC; buf[2] = ZL_SOFT_INV; }
}

typedef struct { uint32_t sent, got, total; } flow_t;

static void pump(zlink_t *l, flow_t *tx, flow_t *rx, uint8_t ch, const char *who) {
	uint8_t buf[ZL_PAYLOAD], exp[ZL_PAYLOAD];
	uint16_t len, elen;
	uint8_t c;
	int n;
	if (tx->sent < tx->total && zl_can_send(l)) {
		make_msg(buf, &len, ch, tx->sent);
		if (zl_send(l, ch, buf, len)) tx->sent++;
	}
	while ((n = zl_recv(l, &c, buf, sizeof buf)) >= 0) {
		make_msg(exp, &elen, c, rx->got);
		CHECK(n == elen && !memcmp(buf, exp, elen), "%s: message %u wrong", who, rx->got);
		rx->got++;
	}
}

static void run_case(const char *name, bool cross, bool stalls, uint32_t total,
                     uint32_t b_late_ms) {
	zl_pins_t pa = { (void *)0, pin_pull, pin_level };
	zl_pins_t pb = { (void *)1, pin_pull, pin_level };
	flow_t ab = { 0, 0, total }, ba = { 0, 0, total };
	uint64_t it;
	uint32_t stall_a = 0, stall_b = 0;
	const uint64_t per_ms = 50;			// iterations per simulated ms
	const uint64_t limit = per_ms * 120000;

	memset(pulled, 0, sizeof pulled);
	crossed = cross;
	seed = 77 + total + (cross ? 1 : 0) + (stalls ? 2 : 0);
	zl_soft_init(&sa, &pa);
	zl_soft_init(&sb, &pb);
	zl_init(&A, &sa.t, prng(), ZL_CAP_SOFT);
	zl_init(&B, &sb.t, prng(), ZL_CAP_SOFT);

	for (it = 0; it < limit; it++) {
		uint32_t now = (uint32_t)(it / per_ms);
		if (stalls) {
			if (!stall_a && prng() % 4000 == 0) stall_a = prng() % (per_ms * 6);
			if (!stall_b && prng() % 4000 == 0) stall_b = prng() % (per_ms * 6);
		}
		if (stall_a) stall_a--;
		else {
			zl_poll(&A, now);
			if (zl_up(&A)) pump(&A, &ab, &ba, 1, "A");
		}
		if (now < b_late_ms) continue;
		if (stall_b) stall_b--;
		else {
			zl_poll(&B, now);
			if (zl_up(&B)) pump(&B, &ba, &ab, 2, "B");
		}
		if (ab.got == total && ba.got == total) break;
	}
	printf("  %-36s %6u ms  A->B %u/%u  B->A %u/%u  bits %u  retx %u+%u  aborts %u+%u\n",
	       name, (unsigned)(it / per_ms), ab.got, total, ba.got, total,
	       sa.bits_tx + sb.bits_tx, A.st.retransmits, B.st.retransmits,
	       sa.aborts, sb.aborts);
	CHECK(ab.got == total && ba.got == total, "%s: not everything arrived", name);
	CHECK(zl_soft_crossed(&sa) == cross && zl_soft_crossed(&sb) == cross,
	      "%s: crossed cable detection (A %d, B %d)", name,
	      zl_soft_crossed(&sa), zl_soft_crossed(&sb));
}

int main(void) {
	printf("test_zlink_soft:\n");
	run_case("straight cable", false, false, 40, 0);
	run_case("crossed cable", true, false, 40, 0);
	run_case("straight, ends stalled at random", false, true, 40, 0);
	run_case("crossed, ends stalled at random", true, true, 40, 0);
	run_case("B starts 2 s late", false, false, 20, 2000);
	if (fails) { printf("test_zlink_soft: %d FAILED\n", fails); return 1; }
	printf("test_zlink_soft: PASS\n");
	return 0;
}
