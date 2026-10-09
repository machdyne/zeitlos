/*
 * Host test for sw/common/zlink_stream.c, with sw/common/zlink.c on top
 * and the real sw/common/zgpio_stream.c underneath.
 *
 *   make -C sw/common/tests -f Makefile.zlink
 *
 * Two GPIO stream engines, modelled at the register level behind
 * zgpio_stream.c's RD()/WR() hooks: FIFOs of the real depth with the K
 * flag, SRX/SRX4 and SSTAT as the RTL has them. Engine 0's TX FIFO
 * drains into engine 1's RX FIFO and the other way round, a few dozen
 * symbols per step -- and the wire can lose a symbol or flip a bit in
 * one, which is what a burst on a real cable does to 8b/10b symbols
 * after decoding: a byte wrong, a byte missing, a control symbol gone.
 *
 * Cases: clean; damaged (1 symbol in 2000 dropped, 1 in 2000 changed);
 * a slow reader whose RX FIFO overflows. Every message must arrive
 * once, in order, intact.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zlink.h"
#include "zlink_stream.h"
#include "zgpio_stream.h"

// -- two engines ----------------------------------------------------------

typedef struct {
	uint32_t sctl, spins, div, lock;
	uint16_t tx[1024]; int txh, txn;
	uint16_t rx[1024]; int rxh, rxn;
	bool rxovr;
} eng_t;
static eng_t eng[2];

static uint32_t seed = 3;
static uint32_t prng(void) {
	seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
	return seed;
}

static int drop_in, flip_in;		// 1 in N symbols, 0 = never

static void rx_push(eng_t *g, uint16_t v) {
	if (g->rxn == 1024) { g->rxovr = true; return; }
	g->rx[(g->rxh + g->rxn) % 1024] = v;
	g->rxn++;
}

// move up to n symbols each way
static void wire(int n) {
	for (int e = 0; e < 2; e++) {
		eng_t *g = &eng[e], *p = &eng[1 - e];
		for (int i = 0; i < n && g->txn; i++) {
			uint16_t v = g->tx[g->txh];
			g->txh = (g->txh + 1) % 1024;
			g->txn--;
			if (drop_in && prng() % (uint32_t)drop_in == 0) continue;
			if (flip_in && prng() % (uint32_t)flip_in == 0) v ^= (uint16_t)(1u << (prng() % 9));
			rx_push(p, v);
		}
	}
}

static uint16_t rx_pop(eng_t *g) {
	uint16_t v = g->rx[g->rxh];
	g->rxh = (g->rxh + 1) % 1024;
	g->rxn--;
	return v;
}

uint32_t z_gs_test_rd(uint32_t a) {
	if (a == 0xe0000008u) return 0x5A475049u;
	if (a == 0xe000000cu) return 0x47500000u | (0xFu << 8) | (2u << 4) | 2u;
	int e = (int)((a - Z_GS_BASE) / Z_GS_SIZE);
	uint32_t r = (a - Z_GS_BASE) % Z_GS_SIZE;
	eng_t *g = &eng[e];
	switch (r) {
	case Z_GS_REG_SCTL: return g->sctl;
	case Z_GS_REG_SPINS: return g->spins;
	case Z_GS_REG_SRATE: return g->div;
	case Z_GS_REG_SSTAT:
		return (uint32_t)g->txn | ((uint32_t)g->rxn << 16) | (g->rxovr ? Z_GS_ST_RXOVR : 0);
	case Z_GS_REG_SRX:
		if (!g->rxn) return 0;
		return 0x200u | rx_pop(g);
	case Z_GS_REG_SRX4: {
		uint32_t w = 0;
		if (g->rxn < 4) return 0;
		for (int i = 0; i < 4; i++) w |= (uint32_t)(rx_pop(g) & 0xff) << (8 * i);
		return w;
	}
	case Z_GS_REG_SLOCK: { uint32_t l = g->lock; g->lock = 1; return l; }
	}
	return 0;
}

void z_gs_test_wr(uint32_t a, uint32_t v) {
	int e = (int)((a - Z_GS_BASE) / Z_GS_SIZE);
	uint32_t r = (a - Z_GS_BASE) % Z_GS_SIZE;
	eng_t *g = &eng[e];
	switch (r) {
	case Z_GS_REG_SCTL: g->sctl = v; break;
	case Z_GS_REG_SPINS: g->spins = v; break;
	case Z_GS_REG_SRATE: g->div = v; break;
	case Z_GS_REG_STX:
		if (g->txn < 1024) { g->tx[(g->txh + g->txn) % 1024] = (uint16_t)(v & 0x1ff); g->txn++; }
		break;
	case Z_GS_REG_STX4:
		for (int i = 0; i < 4 && g->txn < 1024; i++) {
			g->tx[(g->txh + g->txn) % 1024] = (uint16_t)((v >> (8 * i)) & 0xff);
			g->txn++;
		}
		break;
	case Z_GS_REG_SFLUSH:
		if (v & 1) g->txn = 0;
		if (v & 2) g->rxn = 0;
		if (v & 4) g->rxovr = false;
		break;
	case Z_GS_REG_SLOCK: g->lock = v & 1; break;
	}
}

// -- the test ---------------------------------------------------------------

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static zl_stream_t sa, sb;
static zlink_t A, B;

static void make_msg(uint8_t *buf, uint16_t *len, uint8_t chan, uint32_t n) {
	*len = (uint16_t)((n * 41 + chan) % ZL_PAYLOAD);
	for (uint16_t i = 0; i < *len; i++) buf[i] = (uint8_t)(n + i * 5 + chan);
}

typedef struct { uint32_t sent, got, total; } flow_t;

static void pump(zlink_t *l, flow_t *tx, flow_t *rx, uint8_t ch, const char *who, bool read) {
	uint8_t buf[ZL_PAYLOAD], exp[ZL_PAYLOAD];
	uint16_t len, elen;
	uint8_t c;
	int n;
	while (tx->sent < tx->total && zl_can_send(l)) {
		make_msg(buf, &len, ch, tx->sent);
		if (!zl_send(l, ch, buf, len)) break;
		tx->sent++;
	}
	while (read && (n = zl_recv(l, &c, buf, sizeof buf)) >= 0) {
		make_msg(exp, &elen, c, rx->got);
		CHECK(n == elen && !memcmp(buf, exp, elen), "%s: message %u wrong", who, rx->got);
		rx->got++;
	}
}

static void run_case(const char *name, int drop, int flip, uint32_t total, int slow) {
	z_gs_cfg_t c = Z_GS_CFG_INIT;
	flow_t ab = { 0, 0, total }, ba = { 0, 0, total };
	uint32_t now = 0, it;

	memset(eng, 0, sizeof eng);
	drop_in = drop;
	flip_in = flip;
	CHECK(z_gs_claim(0) == 0 && z_gs_claim(1) == 1, "claim both engines");
	c.mode = Z_GS_ZLINK;
	c.flags = Z_GS_RXEN;
	c.tx = Z_GS_PIN(0, 0); c.rx = Z_GS_PIN(0, 1);
	CHECK(z_gs_config(0, &c) == Z_GS_OK, "configure engine 0");
	c.tx = Z_GS_PIN(1, 0); c.rx = Z_GS_PIN(1, 1);
	CHECK(z_gs_config(1, &c) == Z_GS_OK, "configure engine 1");
	zl_stream_init(&sa, 0);
	zl_stream_init(&sb, 1);
	zl_init(&A, &sa.t, 1234 + total, ZL_CAP_STREAM);
	zl_init(&B, &sb.t, 999 + total, ZL_CAP_STREAM);

	// 40 symbols a step each way is ~12 Mbit/s if a step is 33 us;
	// time advances 1 ms every 30 steps
	for (it = 0; it < 30u * 60000u; it++) {
		now = it / 30;
		wire(40);
		zl_poll(&A, now);
		zl_poll(&B, now);
		if (zl_up(&A) && zl_up(&B)) {
			pump(&A, &ab, &ba, 1, "A", true);
			// a slow B reads only every `slow` steps
			pump(&B, &ba, &ab, 2, "B", !slow || it % (uint32_t)slow == 0);
		}
		if (ab.got == total && ba.got == total) break;
	}
	printf("  %-38s %5u ms  A->B %u/%u  B->A %u/%u  retx %u+%u  crc %u+%u  dropped %u+%u\n",
	       name, now, ab.got, total, ba.got, total, A.st.retransmits, B.st.retransmits,
	       A.st.crc_errors, B.st.crc_errors, sa.dropped, sb.dropped);
	CHECK(ab.got == total && ba.got == total, "%s: not everything arrived", name);
	z_gs_release(0);
	z_gs_release(1);
}

int main(void) {
	printf("test_zlink_stream:\n");
	run_case("clean", 0, 0, 500, 0);
	run_case("1 in 2000 symbols lost or changed", 2000, 2000, 500, 0);
	run_case("slow reader, RX FIFO overflowing", 0, 0, 300, 400);
	if (fails) { printf("test_zlink_stream: %d FAILED\n", fails); return 1; }
	printf("test_zlink_stream: PASS\n");
	return 0;
}
