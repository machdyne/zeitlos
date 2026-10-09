/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zlink -- the link layer. The interface and the frame format are in
 * sw/common/zlink.h; docs/zlink.md has the whole picture.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zlink.h"

// -- CRC-32 ------------------------------------------------------------
//
// Table-driven: about ten instructions a byte, where the bitwise form
// is forty -- and at a megabyte a second the link checks every byte
// twice (once each end). The table is built on first use rather than
// stored, so it costs 1 KB of .bss and no .data.

static uint32_t crc_table[256];
static bool crc_ready;

static void crc_init(void) {
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
		crc_table[i] = c;
	}
	crc_ready = true;
}

uint32_t zl_crc32_update(uint32_t crc, const uint8_t *p, uint32_t n) {
	uint32_t c = crc ^ 0xFFFFFFFFu;
	if (!crc_ready) crc_init();
	while (n--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
	return c ^ 0xFFFFFFFFu;
}

uint32_t zl_crc32(const uint8_t *p, uint32_t n) {
	return zl_crc32_update(0, p, n);
}

// -- small helpers -----------------------------------------------------

static uint32_t rnd(zlink_t *l) {
	// xorshift32: timing jitter and ids, nothing that has to be secure
	uint32_t x = l->rng;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	l->rng = x ? x : 0x9E3779B9u;
	return l->rng;
}

static void put32(uint8_t *p, uint32_t v) {
	p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool after(uint32_t now, uint32_t t) {
	return (int32_t)(now - t) >= 0;
}

static uint8_t window(const zlink_t *l) {
	return l->t->half_duplex ? 1 : ZL_WINDOW;
}

// Build and hand a frame to the transport. false: transport busy.
static bool tx_frame(zlink_t *l, uint8_t type, uint8_t seq, uint8_t chan,
		const uint8_t *payload, uint16_t len) {
	uint8_t *f = l->frame;
	uint32_t crc;
	f[0] = type;
	f[1] = seq;
	f[2] = l->expect;
	f[3] = chan;
	f[4] = (uint8_t)len;
	f[5] = (uint8_t)(len >> 8);
	if (len) memcpy(f + ZL_HDR, payload, len);
	crc = zl_crc32(f, ZL_HDR + len);
	put32(f + ZL_HDR + len, crc);
	if (!l->t->send(l->t->ctx, f, (uint16_t)(ZL_HDR + len + 4))) return false;
	l->st.tx_frames++;
	l->last_tx = l->now;
	if (type != ZL_T_HELLO) l->ack_due = false;	// every frame carries the ack
	return true;
}

static bool tx_hello(zlink_t *l) {
	uint8_t p[13];
	p[0] = ZL_MAGIC0;
	p[1] = ZL_MAGIC1;
	p[2] = ZL_VERSION;
	p[3] = l->my_caps;
	put32(p + 4, l->my_rand);
	// the peer's number, if we have it: how the peer learns we heard it
	put32(p + 8, l->got_hello ? l->peer_rand : 0);
	// whether we are up: an up end's HELLO needs no answer
	p[12] = l->up ? 1 : 0;
	return tx_frame(l, ZL_T_HELLO, 0, 0, p, sizeof p);
}

static void reset_seq(zlink_t *l) {
	l->next_seq = 0;
	l->base = 0;
	l->inflight = 0;
	l->resend_from = 0;
	l->expect = 0;
	l->ack_due = false;
}

static void go_down(zlink_t *l, uint32_t now) {
	if (l->up) l->st.downs++;
	l->up = false;
	l->got_hello = false;
	l->turn = false;
	l->turn_fresh = false;
	reset_seq(l);
	l->rxq_count = 0;		// for the session that ended (zlink.h)
	l->hello_due = now + 50 + rnd(l) % 300;
}

static void go_up(zlink_t *l, uint32_t now) {
	l->up = true;
	l->st.ups++;
	l->primary = l->my_rand > l->peer_rand;
	reset_seq(l);
	l->last_rx = now;
	// half duplex: the primary has the first turn
	l->turn = l->primary;
	l->turn_at = now;
}

// -- public ------------------------------------------------------------

void zl_init(zlink_t *l, zl_transport_t *t, uint32_t seed, uint8_t caps) {
	memset(l, 0, sizeof *l);
	l->t = t;
	l->rng = seed ? seed : 0x12345678u;
	rnd(l); rnd(l);
	l->my_rand = rnd(l) | 1;	// never 0: 0 means "not heard yet" in HELLO
	l->my_caps = caps;
	if (!crc_ready) crc_init();
	go_down(l, 0);
	l->st.downs = 0;
	l->hello_due = 20 + rnd(l) % 200;
}

bool zl_up(const zlink_t *l) {
	return l->up;
}

uint32_t zl_recv_time(const zlink_t *l) {
	return l->recv_at;
}

uint8_t zl_peer_caps(const zlink_t *l) {
	return l->peer_caps;
}

bool zl_can_send(const zlink_t *l) {
	return l->up && l->inflight < window(l);
}

bool zl_tx_empty(const zlink_t *l) {
	return l->inflight == 0;
}

bool zl_send(zlink_t *l, uint8_t chan, const void *buf, uint16_t len) {
	uint8_t slot;
	if (!zl_can_send(l) || len > ZL_PAYLOAD) return false;
	slot = (uint8_t)(l->next_seq % ZL_WINDOW);
	l->win[slot].chan = chan;
	l->win[slot].len = len;
	if (len) memcpy(l->win[slot].data, buf, len);
	l->next_seq++;
	l->inflight++;
	l->st.tx_bytes += len;
	return true;
}

int zl_recv(zlink_t *l, uint8_t *chan, void *buf, uint16_t max) {
	uint16_t n;
	if (l->rxq_count == 0) return -1;
	n = l->rxq[l->rxq_head].len;
	if (n > max) n = max;
	if (n) memcpy(buf, l->rxq[l->rxq_head].data, n);
	if (chan) *chan = l->rxq[l->rxq_head].chan;
	l->recv_at = l->rxq[l->rxq_head].at;
	l->rxq_head = (uint8_t)((l->rxq_head + 1) % ZL_RXQ);
	l->rxq_count--;
	return n;
}

// -- receiving ---------------------------------------------------------

static void on_ack(zlink_t *l, uint8_t a, uint32_t now) {
	uint8_t n = (uint8_t)(a - l->base);		// frames this acks
	if (n == 0 || n > l->inflight) return;
	l->base = a;
	l->inflight -= n;
	// whatever was going to be (re)sent from the window now starts
	// that much later in it
	l->resend_from = (uint8_t)(l->resend_from > n ? l->resend_from - n : 0);
	l->rto_at = now + l->t->rto_ms;
}

static void on_frame(zlink_t *l, const uint8_t *f, uint16_t n, uint32_t now) {
	uint16_t len;
	uint8_t type;

	if (n < ZL_HDR + 4) { l->st.crc_errors++; return; }
	len = (uint16_t)(f[4] | (f[5] << 8));
	if (len > ZL_PAYLOAD || n != ZL_HDR + len + 4 ||
	    zl_crc32(f, ZL_HDR + len) != get32(f + ZL_HDR + len)) {
		l->st.crc_errors++;
		return;
	}
	l->st.rx_frames++;
	type = f[0];

	if (type == ZL_T_HELLO) {
		const uint8_t *p = f + ZL_HDR;
		uint32_t pr, echo;
		bool peer_up;
		if (len < 13 || p[0] != ZL_MAGIC0 || p[1] != ZL_MAGIC1) {
			l->st.crc_errors++;
			return;
		}
		pr = get32(p + 4);
		echo = get32(p + 8);
		peer_up = p[12] & 1;
		if (l->up && pr == l->peer_rand && echo == l->my_rand) {
			// the peer, up or coming up, knows us: nothing changes --
			// but a peer still down did not get our last HELLO
			l->last_rx = now;
			if (!peer_up) l->hello_owed = true;
			return;
		}
		if (l->up) {
			// a HELLO that does not know us: the peer restarted
			go_down(l, now);
		}
		l->peer_rand = pr;
		l->peer_caps = p[3];
		l->got_hello = true;
		l->last_rx = now;
		if (pr == l->my_rand) {
			// the same number both ends: pick again, both of us
			l->my_rand = rnd(l) | 1;
			l->got_hello = false;
			return;
		}
		if (echo == l->my_rand) go_up(l, now);
		// answer a peer that is not up yet, so it learns we heard it
		if (!peer_up) l->hello_owed = true;
		return;
	}

	if (!l->up) return;
	l->last_rx = now;

	if (type != ZL_T_DATA && type != ZL_T_ACK) { l->st.crc_errors++; return; }

	on_ack(l, f[2], now);

	if (type == ZL_T_DATA) {
		if (f[1] == l->expect) {
			if (l->rxq_count < ZL_RXQ) {
				uint8_t i = (uint8_t)((l->rxq_head + l->rxq_count) % ZL_RXQ);
				l->rxq[i].chan = f[3];
				l->rxq[i].len = len;
				l->rxq[i].at = now;
				if (len) memcpy(l->rxq[i].data, f + ZL_HDR, len);
				l->rxq_count++;
				l->expect++;
				l->st.rx_bytes += len;
			} else {
				// flow control: not taken, not acked; it will come again
				l->st.rx_full++;
			}
		} else {
			l->st.out_of_order++;
		}
		l->ack_due = true;
	}

	if (l->t->half_duplex) {
		// the peer has finished its frame: the turn is ours
		l->turn = true;
		l->turn_fresh = true;
		// primary: answer data at once, otherwise poll again later
		l->turn_at = (type == ZL_T_DATA || l->inflight) ? now
		                                               : now + ZL_POLL_MS;
	}
}

// -- sending -----------------------------------------------------------

// The next frame from the window, if any is due. true if one was sent.
static bool tx_window(zlink_t *l, uint32_t now) {
	uint8_t seq, slot;
	if (l->resend_from >= l->inflight) return false;
	seq = (uint8_t)(l->base + l->resend_from);
	slot = (uint8_t)(seq % ZL_WINDOW);
	if (!tx_frame(l, ZL_T_DATA, seq, l->win[slot].chan,
	              l->win[slot].data, l->win[slot].len))
		return false;
	if (l->resend_from == 0) l->rto_at = now + l->t->rto_ms;
	l->resend_from++;
	return true;
}

static void tx_full_duplex(zlink_t *l, uint32_t now) {
	if (!l->t->tx_idle(l->t->ctx)) return;
	if (tx_window(l, now)) return;
	if (l->ack_due) tx_frame(l, ZL_T_ACK, 0, 0, NULL, 0);
}

static void tx_half_duplex(zlink_t *l, uint32_t now) {
	if (!l->turn || !l->t->tx_idle(l->t->ctx)) return;

	if (l->primary) {
		// speak when there is something to say, or the poll is due
		if (!l->inflight && !l->ack_due && !after(now, l->turn_at)) return;
	} else if (!l->inflight && l->turn_fresh) {
		// The secondary answers every frame -- but not with a bare ACK
		// in the same call that brought the turn: the app has not had
		// a chance to zl_send() yet, and an ACK now would leave its
		// next message waiting for the primary's next poll, ZL_POLL_MS
		// away. One zl_poll() later the answer carries the data.
		return;
	}
	// window of one: whatever is unacked goes (again), else an ACK
	l->resend_from = 0;
	if (l->inflight) {
		if (!tx_window(l, now)) return;
	} else {
		if (!tx_frame(l, ZL_T_ACK, 0, 0, NULL, 0)) return;
	}
	l->turn = false;
	// primary: if no answer comes, the turn comes back after the timeout
	l->turn_at = now + l->t->rto_ms;
}

void zl_poll(zlink_t *l, uint32_t now) {
	uint16_t n;

	l->now = now;
	l->t->poll(l->t->ctx, now);

	while ((n = l->t->recv(l->t->ctx, l->frame, sizeof l->frame)) != 0)
		on_frame(l, l->frame, n, now);

	// a HELLO we owe goes first, up or down
	if (l->hello_owed && l->t->tx_idle(l->t->ctx)) {
		if (tx_hello(l)) l->hello_owed = false;
		return;
	}

	if (!l->up) {
		if (after(now, l->hello_due) && l->t->tx_idle(l->t->ctx)) {
			if (tx_hello(l))
				l->hello_due = now + 100 + rnd(l) % 300;
		}
		return;
	}

	if ((int32_t)(now - l->last_rx) > ZL_DEAD_MS) {
		go_down(l, now);
		return;
	}

	if (l->t->half_duplex) {
		// primary: an answer that never came gives the turn back
		if (l->primary && !l->turn && after(now, l->turn_at)) {
			l->turn = true;
			if (l->inflight) l->st.retransmits++;
		}
		tx_half_duplex(l, now);
		l->turn_fresh = false;
	} else {
		if (l->inflight && l->resend_from && after(now, l->rto_at)) {
			l->st.retransmits += l->resend_from;
			l->resend_from = 0;		// go back to base
		}
		// keepalive: an idle full-duplex link sends nothing, and the
		// other end would call it dead after ZL_DEAD_MS
		if (!l->ack_due && !l->inflight &&
		    (int32_t)(now - l->last_tx) > ZL_DEAD_MS / 3)
			l->ack_due = true;
		tx_full_duplex(l, now);
	}
}
