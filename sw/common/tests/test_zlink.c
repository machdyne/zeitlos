/*
 * Host test for sw/common/zlink.c -- the zlink link layer.
 *
 *   make -C sw/common/tests -f Makefile.zlink
 *
 * Two links, A and B, joined by a simulated wire that carries whole
 * frames: each takes time to send (a byte time per byte), arrives after
 * a latency, and may be DROPPED or CORRUPTED at random. In half-duplex
 * mode the wire is shared, and two frames that overlap in time destroy
 * each other -- a collision, as on the soft transport.
 *
 * Time is simulated in 1 ms steps. Each case pushes numbered messages
 * through on two channels in both directions at once and checks that
 * every one arrives, once, in order, intact, on its own channel.
 *
 * Cases:
 *   1. full duplex, clean
 *   2. full duplex, 15% of frames dropped and 5% corrupted
 *   3. half duplex, clean
 *   4. half duplex, 10% dropped, 5% corrupted, plus collisions
 *   5. a receiver that stops reading for 2 s: flow control, no loss
 *   6. B restarts mid-transfer: both go down, come back up, carry on
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zlink.h"

// -- the wire -----------------------------------------------------------

#define MAXQ 16

typedef struct end end_t;

typedef struct {
	uint8_t data[ZL_FRAME_MAX];
	uint16_t len;
	uint32_t start, done;		// on the wire from start to done
	bool dead;					// collided
} txf_t;

typedef struct {
	uint8_t data[ZL_FRAME_MAX];
	uint16_t len;
	uint32_t at;
} rxf_t;

struct end {
	zl_transport_t t;
	end_t *peer;
	txf_t cur;					// the frame being sent
	bool sending;
	rxf_t q[MAXQ];				// arriving at this end
	int qn;
};

static struct {
	bool half;
	int drop_pct, corrupt_pct;
	uint32_t latency;
	uint32_t byte_us;			// microseconds per byte
	uint32_t now;
	uint32_t collisions;
} wire;

static uint32_t seed = 1;
static uint32_t prng(void) {
	seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
	return seed;
}

static void deliver(end_t *to, const txf_t *f) {
	if (to->qn == MAXQ) return;		// the far end's buffer is full: lost
	if ((int)(prng() % 100) < wire.drop_pct) return;
	rxf_t *r = &to->q[to->qn++];
	memcpy(r->data, f->data, f->len);
	r->len = f->len;
	r->at = f->done + wire.latency;
	if ((int)(prng() % 100) < wire.corrupt_pct)
		r->data[prng() % f->len] ^= (uint8_t)(1u << (prng() % 8));
}

static bool t_send(void *ctx, const uint8_t *frame, uint16_t len) {
	end_t *e = ctx;
	if (e->sending) return false;
	memcpy(e->cur.data, frame, len);
	e->cur.len = len;
	e->cur.start = wire.now;
	e->cur.done = wire.now + 1 + (len * wire.byte_us) / 1000;
	e->cur.dead = false;
	e->sending = true;
	// half duplex: overlapping the other end's frame kills both
	if (wire.half && e->peer->sending) {
		e->cur.dead = true;
		e->peer->cur.dead = true;
		wire.collisions++;
	}
	return true;
}

static bool t_idle(void *ctx) {
	return !((end_t *)ctx)->sending;
}

static uint16_t t_recv(void *ctx, uint8_t *buf, uint16_t max) {
	end_t *e = ctx;
	for (int i = 0; i < e->qn; i++) {
		if (e->q[i].at <= wire.now) {
			uint16_t n = e->q[i].len;
			if (n <= max) memcpy(buf, e->q[i].data, n);
			memmove(&e->q[i], &e->q[i + 1], (size_t)(e->qn - i - 1) * sizeof(rxf_t));
			e->qn--;
			return n <= max ? n : 0;
		}
	}
	return 0;
}

static void t_poll(void *ctx, uint32_t now) {
	end_t *e = ctx;
	(void)now;
	if (e->sending && wire.now >= e->cur.done) {
		e->sending = false;
		if (!e->cur.dead) deliver(e->peer, &e->cur);
	}
}

static end_t ea, eb;

static void setup_ends(bool half) {
	memset(&ea, 0, sizeof ea);
	memset(&eb, 0, sizeof eb);
	ea.peer = &eb; eb.peer = &ea;
	for (int i = 0; i < 2; i++) {
		end_t *e = i ? &eb : &ea;
		e->t.name = i ? "B" : "A";
		e->t.ctx = e;
		e->t.half_duplex = half;
		e->t.rto_ms = half ? 60 : 30;
		e->t.send = t_send;
		e->t.tx_idle = t_idle;
		e->t.recv = t_recv;
		e->t.poll = t_poll;
	}
	wire.half = half;
}

// -- traffic ------------------------------------------------------------

static zlink_t A, B;
static int fails;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef struct {
	uint32_t sent[4], got[4];	// per channel: next to send, next expected
	uint32_t total;				// messages per channel to send
	bool reading;
} flow_t;

static void make_msg(uint8_t *buf, uint16_t *len, uint8_t chan, uint32_t n) {
	// a length that varies, contents that depend on channel and number
	*len = (uint16_t)((n * 37 + chan * 11) % ZL_PAYLOAD);
	for (uint16_t i = 0; i < *len; i++) buf[i] = (uint8_t)(n * 7 + i + chan);
	if (*len >= 4) {
		buf[0] = (uint8_t)n; buf[1] = (uint8_t)(n >> 8);
		buf[2] = chan; buf[3] = 0xA5;
	}
}

static void pump(zlink_t *l, flow_t *f, const char *who, uint8_t ch_tx, uint8_t ch_rx) {
	uint8_t buf[ZL_PAYLOAD], exp[ZL_PAYLOAD];
	uint16_t len, elen;
	// send while there is room
	while (f->sent[ch_tx] < f->total && zl_can_send(l)) {
		make_msg(buf, &len, ch_tx, f->sent[ch_tx]);
		if (!zl_send(l, ch_tx, buf, len)) break;
		f->sent[ch_tx]++;
	}
	// receive, and check
	while (f->reading) {
		uint8_t ch;
		int n = zl_recv(l, &ch, buf, sizeof buf);
		if (n < 0) break;
		if (ch != ch_rx) { CHECK(0, "%s: message on channel %d, expected %d", who, ch, ch_rx); continue; }
		make_msg(exp, &elen, ch, f->got[ch]);
		CHECK(n == elen && memcmp(buf, exp, elen) == 0,
		      "%s: channel %d message %u wrong (len %d want %d)", who, ch, f->got[ch], n, elen);
		f->got[ch]++;
	}
}

// Run until both sides have everything or time runs out. Returns ms.
static uint32_t run(flow_t *fa, flow_t *fb, uint32_t limit_ms,
                    void (*hook)(uint32_t t)) {
	uint32_t t0 = wire.now;
	while (wire.now - t0 < limit_ms) {
		if (hook) hook(wire.now - t0);
		zl_poll(&A, wire.now);
		zl_poll(&B, wire.now);
		if (zl_up(&A) && zl_up(&B)) {
			pump(&A, fa, "A", 1, 2);
			pump(&B, fb, "B", 2, 1);
		}
		if (fa->got[2] == fb->total && fb->got[1] == fa->total) break;
		wire.now++;
	}
	return wire.now - t0;
}

static void start(bool half, int drop, int corrupt, uint32_t s) {
	setup_ends(half);
	wire.drop_pct = drop;
	wire.corrupt_pct = corrupt;
	wire.latency = 1;
	wire.byte_us = half ? 40 : 1;		// soft ~200 kbit/s; stream ~8 Mbit/s
	wire.now = 1000;
	wire.collisions = 0;
	seed = s;
	zl_init(&A, &ea.t, s * 7 + 1, ZL_CAP_SOFT);
	zl_init(&B, &eb.t, s * 13 + 5, ZL_CAP_SOFT | ZL_CAP_STREAM);
}

static void report(const char *name, uint32_t ms, flow_t *fa, flow_t *fb) {
	printf("  %-40s %5u ms  A->B %u/%u  B->A %u/%u  retx %u+%u  crc %u+%u  coll %u\n",
	       name, ms, fb->got[1], fa->total, fa->got[2], fb->total,
	       A.st.retransmits, B.st.retransmits, A.st.crc_errors, B.st.crc_errors,
	       wire.collisions);
	CHECK(fb->got[1] == fa->total && fa->got[2] == fb->total,
	      "%s: not everything arrived", name);
}

static void case_basic(const char *name, bool half, int drop, int corrupt,
                       uint32_t n, uint32_t limit) {
	flow_t fa = { .total = n, .reading = true }, fb = { .total = n, .reading = true };
	start(half, drop, corrupt, 42 + n + (uint32_t)drop);
	uint32_t ms = run(&fa, &fb, limit, NULL);
	report(name, ms, &fa, &fb);
	CHECK(A.primary != B.primary, "%s: exactly one primary", name);
	CHECK(zl_peer_caps(&A) == (ZL_CAP_SOFT | ZL_CAP_STREAM) &&
	      zl_peer_caps(&B) == ZL_CAP_SOFT, "%s: caps exchanged", name);
}

// 5: B stops reading for 2 s
static flow_t *g_fb;
static void stall_hook(uint32_t t) {
	g_fb->reading = !(t > 200 && t < 2200);
}

// 6: B restarts at 300 ms
static void restart_hook(uint32_t t) {
	if (t == 300) zl_init(&B, &eb.t, 999, ZL_CAP_SOFT);
}

int main(void) {
	printf("test_zlink:\n");

	// CRC-32 (IEEE): the standard check value, and in pieces
	{
		const uint8_t *t = (const uint8_t *)"123456789";
		CHECK(zl_crc32(t, 9) == 0xCBF43926u, "CRC-32 of 123456789");
		CHECK(zl_crc32_update(zl_crc32_update(0, t, 4), t + 4, 5) == 0xCBF43926u,
		      "CRC-32 in two pieces");
		CHECK(zl_crc32_update(0, t, 0) == 0, "CRC-32 of nothing");
	}

	case_basic("full duplex, clean", false, 0, 0, 300, 20000);
	case_basic("full duplex, 15% dropped, 5% corrupted", false, 15, 5, 300, 60000);
	case_basic("half duplex, clean", true, 0, 0, 60, 60000);
	case_basic("half duplex, 10% dropped, 5% corrupted", true, 10, 5, 60, 120000);

	{
		flow_t fa = { .total = 200, .reading = true }, fb = { .total = 0, .reading = true };
		start(false, 0, 0, 7);
		g_fb = &fb;
		uint32_t ms = run(&fa, &fb, 30000, stall_hook);
		report("receiver stalls for 2 s", ms, &fa, &fb);
		CHECK(B.st.rx_full > 0, "the stall was felt (rx_full %u)", B.st.rx_full);
		CHECK(ms > 2000, "and lasted");
	}

	{
		// After a restart the messages in flight at the time are lost --
		// the link is reliable within one session, not across them -- so
		// this checks the link comes back and carries traffic again.
		flow_t fa = { .total = 1000000, .reading = true }, fb = { .total = 0, .reading = true };
		start(false, 0, 0, 11);
		run(&fa, &fb, 301, restart_hook);	// still sending when B restarts
		CHECK(!zl_up(&B), "B restarted");
		uint32_t t0 = wire.now;
		while (!(zl_up(&A) && zl_up(&B)) && wire.now - t0 < 10000) {
			zl_poll(&A, wire.now); zl_poll(&B, wire.now); wire.now++;
		}
		CHECK(zl_up(&A) && zl_up(&B), "both back up after the restart");
		CHECK(A.st.downs == 1, "A noticed the restart (downs %u)", A.st.downs);
		flow_t ga = { .total = 50, .reading = true }, gb = { .total = 50, .reading = true };
		uint32_t ms = run(&ga, &gb, 20000, NULL);
		report("traffic after a restart", ms, &ga, &gb);
	}

	if (fails) { printf("test_zlink: %d FAILED\n", fails); return 1; }
	printf("test_zlink: PASS\n");
	return 0;
}
