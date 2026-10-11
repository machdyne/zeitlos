/*
 * Host test for sw/common/zlink_fast.c -- fast zlink's transport and its
 * negotiation of who transmits on which pair -- with the real zfast.c
 * under it and the real zlink.c on top.
 *
 *   make -C sw/common/tests -f Makefile.zlink
 *
 * Two fast blocks (rtl/zfast.v) are modelled at the register level and
 * joined by a modelled SATA cable: straight or crossed, either pair
 * optionally wired P to N. A pair carries symbols from the end driving
 * it to the end listening on it -- if exactly one end drives it, and the
 * listener is on the same rate. Data on an inverted pair arrives as code
 * errors unless the listener has RXINV. Both ends driving one pair is
 * CONTENTION: allowed for a moment (two ends trying DS1 at once on a
 * straight cable), measured, and failed if it lasts longer than one try.
 *
 * Each case runs with many random seeds -- the negotiation is a race --
 * and must link, then move messages both ways intact:
 *
 *   ML1-ML1 straight, crossed, and with a pair inverted
 *   ML1-ML2 and ML2-ML1 straight; ML2-ML2 crossed
 *   ML2-ML2 straight: cannot link; the hint must say crossover
 *   a restart of one end once linked
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "zlink.h"
#include "zlink_fast.h"
#include "zfast.h"

int z_zf_test_side;
int16_t z_zf_test_pending[2] = { -1, -1 };

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

// -- the blocks --------------------------------------------------------------

typedef struct {
	bool tx_ds2;
	uint32_t fctl;
	uint16_t txq[1024]; int txh, txn;
	uint16_t rxq[1024]; int rxh, rxn;
	bool kword, rxovr;
	uint32_t errors, lock;
} blk_t;
static blk_t blk[2];

static bool crossed;
static bool inv[2];			// wire (A's pair 1, 2) wired P to N
static uint32_t contention_ms, contention_max, contention_now;

static bool b_on(int s) { return blk[s].fctl & 1u; }
static int b_tx(int s) { return b_on(s) ? (int)((blk[s].fctl >> 1) & 3u) : 0; }
static int b_rx(int s) { return ((blk[s].fctl >> 3) & 1u) ? 2 : 1; }
static int b_rate(int s) { return (int)((blk[s].fctl >> 6) & 3u); }
static bool b_rxinv(int s) { return (blk[s].fctl >> 9) & 1u; }

// the wire (1 or 2, named by A's pairs) that side s's pair p is on
static int wire_of(int s, int p) {
	if (p == 0) return 0;
	return (s == 1 && crossed) ? 3 - p : p;
}

static void rx_push(blk_t *b, uint16_t v) {
	if (!(b->fctl & (1u << 4))) return;		// RXEN
	if (b->rxn == 1024) { b->rxovr = true; return; }
	b->rxq[(b->rxh + b->rxn++) % 1024] = v;
}

static bool aligned(int s) {
	int o = 1 - s, w;
	if (!b_on(s)) return false;
	w = wire_of(s, b_rx(s));
	if (wire_of(s, b_tx(s)) == w) return false;		// listening to itself
	return wire_of(o, b_tx(o)) == w && b_rate(o) == b_rate(s);
}

// move up to n symbols down every pair that carries any
static void cable(int n, bool tick_ms) {
	bool both = false;
	for (int w = 1; w <= 2; w++)
		if (wire_of(0, b_tx(0)) == w && wire_of(1, b_tx(1)) == w) both = true;
	if (tick_ms) {
		if (both) {
			contention_ms++;
			if (++contention_now > contention_max) contention_max = contention_now;
		} else contention_now = 0;
	}
	for (int s = 0; s < 2; s++) {
		blk_t *b = &blk[s];
		int o = 1 - s, w = wire_of(s, b_tx(s));
		if (!w) continue;
		for (int i = 0; i < n && b->txn; i++) {
			uint16_t v = b->txq[b->txh];
			b->txh = (b->txh + 1) % 1024;
			b->txn--;
			if (!aligned(o) || wire_of(o, b_rx(o)) != w) continue;	// into nothing
			if (inv[w - 1] != b_rxinv(o)) { blk[o].errors++; continue; }
			rx_push(&blk[o], v);
		}
	}
}

static uint32_t rd_blk(int s, uint32_t off) {
	blk_t *b = &blk[s];
	uint32_t v = 0;
	switch (off) {
	case Z_ZF_REG_FCTL: return b->fctl | (b->tx_ds2 ? 1u << 16 : 0);
	case Z_ZF_REG_FSTAT:
		return (uint32_t)b->txn | ((uint32_t)b->rxn << 16) | (b->rxovr ? Z_ZF_ST_RXOVR : 0) |
		       (aligned(s) ? Z_ZF_ST_ALIGNED : 0) | (b->kword ? 1u << 15 : 0);
	case Z_ZF_REG_FRX:
		if (!b->rxn) return 0;
		v = 0x200u | b->rxq[b->rxh];
		b->rxh = (b->rxh + 1) % 1024;
		b->rxn--;
		return v;
	case Z_ZF_REG_FRX4:
		if (b->rxn < 4) return 0;
		for (int i = 0; i < 4; i++) {
			uint16_t e = b->rxq[b->rxh];
			b->rxh = (b->rxh + 1) % 1024;
			b->rxn--;
			if (e & 0x100) b->kword = true;
			v |= (uint32_t)(e & 0xff) << (8 * i);
		}
		return v;
	case Z_ZF_REG_FERR: return b->errors & 0xffff;
	case Z_ZF_REG_FLOCK: v = b->lock; b->lock = 1; return v;
	}
	return 0;
}

static void wr_blk(int s, uint32_t off, uint32_t v) {
	blk_t *b = &blk[s];
	switch (off) {
	case Z_ZF_REG_FCTL:
		if (((v >> 1) & 3u) == 2 && !b->tx_ds2) v &= ~6u;
		b->fctl = v & 0x3ffu;
		break;
	case Z_ZF_REG_FTX:
		if (b->txn < 1024) b->txq[(b->txh + b->txn++) % 1024] = (uint16_t)(v & 0x1ff);
		break;
	case Z_ZF_REG_FTX4:
		for (int i = 0; i < 4 && b->txn < 1024; i++)
			b->txq[(b->txh + b->txn++) % 1024] = (uint16_t)((v >> (8 * i)) & 0xff);
		break;
	case Z_ZF_REG_FFLUSH:
		if (v & 1) b->txn = 0;
		if (v & 2) b->rxn = 0;
		if (v & 4) { b->rxovr = false; b->kword = false; b->errors = 0; }
		break;
	case Z_ZF_REG_FLOCK: b->lock = v & 1; break;
	}
}

uint32_t z_zf_test_rd(uint32_t a) {
	if (a == 0xe0000008u) return 0x5A475049u;
	if (a == 0xe000000cu) return 0x4750u << 16 | 1u << 15;
	return rd_blk(z_zf_test_side, a - Z_ZF_BASE);
}

void z_zf_test_wr(uint32_t a, uint32_t v) {
	wr_blk(z_zf_test_side, a - Z_ZF_BASE, v);
}

// -- the two machines ---------------------------------------------------------

static zl_fast_t F[2];
static zlink_t L[2];

static void side(int s) { z_zf_test_side = s; }
static uint32_t clock_ms;			// carries on from one run() to the next

static void start(int s, uint32_t seed) {
	side(s);
	z_zf_test_pending[s] = -1;
	CHECK(z_zf_claim(), "claim side %d", s);
	zl_fast_init(&F[s], Z_ZF_100M, seed);
	zl_init(&L[s], &F[s].t, seed * 7 + 1, ZL_CAP_STREAM);
}

static void make_msg(uint8_t *buf, uint16_t *len, int s, uint32_t n) {
	*len = (uint16_t)((n * 37 + (uint32_t)s * 11) % ZL_PAYLOAD);
	for (uint16_t i = 0; i < *len; i++) buf[i] = (uint8_t)(n * 3 + i + (uint32_t)s);
}

// Run up to `ms`; once both are up, send `total` messages each way.
// Returns the ms it took to link (or 0 if it never did) and checks the
// messages if any were asked for.
static uint32_t run(uint32_t ms, uint32_t total, uint32_t restart_at) {
	uint32_t sent[2] = { 0, 0 }, got[2] = { 0, 0 }, linked_at = 0;
	for (uint32_t i = 0; i < ms; i++) {
		uint32_t t = clock_ms++;
		for (int k = 0; k < 4; k++) {
			cable(2500, k == 0);			// 10 MB/s: 10000 symbols a ms
			for (int s = 0; s < 2; s++) {
				uint8_t buf[ZL_PAYLOAD], exp[ZL_PAYLOAD], c;
				uint16_t len, elen;
				int n;
				side(s);
				zl_poll(&L[s], t);
				while (zl_up(&L[s]) && sent[s] < total && zl_can_send(&L[s])) {
					make_msg(buf, &len, s, sent[s]);
					if (!zl_send(&L[s], (uint8_t)s, buf, len)) break;
					sent[s]++;
				}
				while ((n = zl_recv(&L[s], &c, buf, sizeof buf)) >= 0) {
					make_msg(exp, &elen, 1 - s, got[s]);
					CHECK(n == elen && !memcmp(buf, exp, elen), "side %d message %u wrong", s, got[s]);
					got[s]++;
				}
			}
		}
		if (!linked_at && zl_up(&L[0]) && zl_up(&L[1])) linked_at = i ? i : 1;
		if (restart_at && i == restart_at) start(1, 9999);
		if (total && got[0] >= total && got[1] >= total && (!restart_at || i > restart_at)) break;
	}
	if (total) CHECK(got[0] >= total && got[1] >= total, "messages: %u and %u of %u", got[0], got[1], total);
	if (getenv("ZFDBG") && total && (got[0] < total || got[1] < total))
		for (int s = 0; s < 2; s++)
			printf("  side %d: sent %u ns %d rs %d frames_rx %u dropped %u | zl up %d tx %u rx %u rexmit %u crc %u ooo %u full %u ups %u downs %u | txn %d rxn %d err %u\n",
			       s, sent[s], F[s].ns, F[s].rs, F[s].frames_rx, F[s].dropped, zl_up(&L[s]),
			       L[s].st.tx_frames, L[s].st.rx_frames, L[s].st.retransmits, L[s].st.crc_errors,
			       L[s].st.out_of_order, L[s].st.rx_full, L[s].st.ups, L[s].st.downs, blk[s].txn, blk[s].rxn, blk[s].errors);
	return linked_at;
}

static void reset_world(bool a_ds2, bool b_ds2, bool cross, bool inv1, bool inv2) {
	memset(blk, 0, sizeof blk);
	blk[0].tx_ds2 = a_ds2;
	blk[1].tx_ds2 = b_ds2;
	crossed = cross;
	inv[0] = inv1;
	inv[1] = inv2;
	contention_ms = contention_max = contention_now = 0;
}

static void kase(const char *name, bool a_ds2, bool b_ds2, bool cross, bool inv1, bool inv2,
                 int seeds, uint32_t total) {
	uint32_t worst = 0, sum = 0, cmax = 0;
	int linked = 0;
	for (int i = 0; i < seeds; i++) {
		uint32_t t;
		reset_world(a_ds2, b_ds2, cross, inv1, inv2);
		start(0, 1000u + (uint32_t)i * 17u);
		start(1, 5000u + (uint32_t)i * 31u);
		t = run(5000, total, 0);
		if (t) { linked++; sum += t; if (t > worst) worst = t; }
		if (contention_max > cmax) cmax = contention_max;
	}
	printf("  %-34s linked %2d/%-2d  ms to link: mean %4u worst %4u  longest contention %3u ms\n",
	       name, linked, seeds, linked ? sum / (uint32_t)linked : 0, worst, cmax);
	CHECK(linked == seeds, "%s: linked %d of %d", name, linked, seeds);
	CHECK(cmax <= 85, "%s: contention lasted %u ms", name, cmax);
}

int main(void) {
	printf("test_zlink_fast:\n");
	kase("ML1-ML1 straight", true, true, false, false, false, 40, 60);
	kase("ML1-ML1 crossed", true, true, true, false, false, 40, 60);
	kase("ML1-ML2 straight", true, false, false, false, false, 40, 60);
	kase("ML2-ML1 straight", false, true, false, false, false, 40, 60);
	kase("ML2-ML2 crossed", false, false, true, false, false, 40, 60);
	kase("ML1-ML1 straight, DS2 inverted", true, true, false, false, true, 20, 60);
	kase("ML1-ML1 crossed, both inverted", true, true, true, true, true, 20, 60);

	// two ML2s on a straight cable cannot link; the hint says why
	{
		reset_world(false, false, false, false, false);
		start(0, 4242);
		start(1, 2424);
		uint32_t t = run(3000, 0, 0);
		CHECK(t == 0, "ML2-ML2 straight linked?");
		CHECK(zl_fast_hint(&F[0]) && strstr(zl_fast_hint(&F[0]), "crossover"), "no crossover hint");
		printf("  %-34s does not link; hint: %s; longest contention %u ms\n", "ML2-ML2 straight",
		       zl_fast_hint(&F[0]) ? zl_fast_hint(&F[0]) : "(none)", contention_max);
		CHECK(contention_max <= 85, "ML2-ML2: contention lasted %u ms", contention_max);
	}

	// one end restarts once linked: the link comes back
	{
		reset_world(true, true, false, false, false);
		start(0, 77);
		start(1, 88);
		CHECK(run(2000, 40, 0), "not linked before the restart");
		side(1);
		z_zf_release();				// B stops and starts again
		start(1, 99);
		// as in test_zlink.c: the link is reliable within one session,
		// not across them -- so: back up, then fresh traffic both ways
		CHECK(run(3000, 0, 0), "not linked again after the restart");
		for (uint32_t i = 0; i < 3000 && !(zl_up(&L[0]) && zl_up(&L[1])); i++) run(1, 0, 0);
		CHECK(L[0].st.downs == 1, "A noticed the restart (downs %u)", L[0].st.downs);
		run(3000, 40, 0);
		printf("  %-34s back after a restart: A %s, B %s\n", "restart",
		       zl_fast_role(&F[0]), zl_fast_role(&F[1]));
		CHECK(zl_fast_linked(&F[0]) && zl_fast_linked(&F[1]), "not linked after restart");
	}

	if (fails) { printf("test_zlink_fast: %d FAILED\n", fails); return 1; }
	printf("test_zlink_fast: PASS\n");
	return 0;
}
