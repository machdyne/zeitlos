/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zlink's soft transport -- see sw/common/zlink_soft.h for the
 * protocol and docs/zlink.md for where it fits.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zlink_soft.h"

#define WX 0
#define WY 1

// a step waiting longer than this means a collision or a peer gone
#define BIT_TIMEOUT_MS 250

// Between two bits of a frame both wires are high for a moment, which
// looks exactly like an idle line. So an end only STARTS a frame when
// none is arriving and nothing has arrived for this long -- otherwise
// it would start talking in the gap between someone else's bits.
#define QUIET_MS 3

enum {
	S_IDLE,			// both wires ours to look at
	S_TX_ACK,		// we hold a wire for a bit: waiting for the ack
	S_TX_REL,		// we let go: waiting for the receiver to let go
	S_RX_ACK,		// we acked a bit: waiting for the sender to let go
	S_SETTLE,		// after an abort or a confusing state: both high
};

static void pull(zl_soft_t *s, int w, bool low) {
	s->pins.pull(s->pins.ctx, w, low);
}

static bool high(zl_soft_t *s, int w) {
	return s->pins.level(s->pins.ctx, w);
}

static void release_all(zl_soft_t *s) {
	pull(s, WX, false);
	pull(s, WY, false);
	s->mine = -1;
}

static void enter(zl_soft_t *s, uint8_t st) {
	s->state = st;
	s->since = s->now;
}

static void abort_all(zl_soft_t *s) {
	release_all(s);
	s->tx_active = false;
	s->aligned = false;
	s->rx_len = 0;
	s->aborts++;
	enter(s, S_SETTLE);
}

// -- receiving: bits into bytes into frames ------------------------------

static void rx_byte(zl_soft_t *s, uint8_t b) {
	if (b == ZL_SOFT_END) {
		if (s->esc) {			// ESC END: not a thing
			s->rx_len = 0;
			s->esc = false;
			return;
		}
		if (s->rx_len) {
			if (s->done_len == 0) {
				memcpy(s->done, s->rx, s->rx_len);
				s->done_len = s->rx_len;
				s->frames_rx++;
				s->crossed = s->inverted;
			} else {
				s->overflows++;	// nobody took the last one
			}
		}
		s->rx_len = 0;
		return;
	}
	if (b == ZL_SOFT_ESC) {
		s->esc = true;
		return;
	}
	if (b == ZL_SOFT_INV && !s->esc) {
		// never sent unescaped: we are off the byte boundary
		s->aligned = false;
		s->rx_len = 0;
		return;
	}
	if (s->esc) {
		b ^= 0x20;
		s->esc = false;
	}
	if (s->rx_len >= sizeof s->rx) {
		s->aligned = false;		// too long: lost a delimiter somewhere
		s->rx_len = 0;
		return;
	}
	s->rx[s->rx_len++] = b;
}

static void rx_bit(zl_soft_t *s, uint8_t bit) {
	s->bits_rx++;
	s->last_rx_bit = s->now;
	if (!s->aligned) {
		s->hunt = (uint16_t)((s->hunt >> 1) | (bit << 15));
		if (s->hunt == 0xC0C0 || s->hunt == 0x3F3F) {
			s->aligned = true;
			s->inverted = (s->hunt == 0x3F3F);
			s->rx_bits = 0;
			s->rx_byte = 0;
			s->rx_len = 0;
			s->esc = false;
		}
		return;
	}
	s->rx_byte |= (uint8_t)(bit << s->rx_bits);
	if (++s->rx_bits == 8) {
		uint8_t b = s->inverted ? (uint8_t)~s->rx_byte : s->rx_byte;
		s->rx_bits = 0;
		s->rx_byte = 0;
		rx_byte(s, b);
	}
}

// -- the bit engine -----------------------------------------------------

// Part of a frame has arrived and the rest has not.
static bool rx_in_frame(const zl_soft_t *s) {
	return s->aligned && (s->rx_len || s->rx_bits || s->esc);
}

// One step. true if anything changed.
static bool step(zl_soft_t *s) {
	bool x = high(s, WX), y = high(s, WY);

	switch (s->state) {

	case S_IDLE:
		if (x != y) {
			// the peer holds one wire: a bit for us. Ack on the other.
			uint8_t bit = x ? 1 : 0;		// Y low means 1
			rx_bit(s, bit);
			s->mine = bit ? WX : WY;
			pull(s, s->mine, true);
			enter(s, S_RX_ACK);
			return true;
		}
		if (!x && !y) {
			// both low with nothing of ours on them: someone else's
			// confusion. Wait it out.
			enter(s, S_SETTLE);
			return true;
		}
		if (s->tx_active && (s->tx_pos || s->tx_bit ||
		    (!rx_in_frame(s) && s->now - s->last_rx_bit >= QUIET_MS))) {
			uint8_t bit = (s->tx[s->tx_pos] >> s->tx_bit) & 1;
			s->mine = bit ? WY : WX;
			pull(s, s->mine, true);
			enter(s, S_TX_ACK);
			return true;
		}
		return false;

	case S_TX_ACK:
		// the ack is the other wire going low too
		if (!(s->mine == WX ? y : x)) {
			pull(s, s->mine, false);
			s->mine = -1;
			enter(s, S_TX_REL);
			return true;
		}
		break;

	case S_TX_REL:
		if (x && y) {
			s->bits_tx++;
			if (++s->tx_bit == 8) {
				s->tx_bit = 0;
				if (++s->tx_pos == s->tx_len) s->tx_active = false;
			}
			enter(s, S_IDLE);
			return true;
		}
		break;

	case S_RX_ACK:
		// the sender's wire -- the one that is not ours -- goes high
		if (s->mine == WX ? y : x) {
			pull(s, s->mine, false);
			s->mine = -1;
			enter(s, S_IDLE);
			return true;
		}
		break;

	case S_SETTLE:
		if (x && y) {
			enter(s, S_IDLE);
			return true;
		}
		break;
	}

	if (s->now - s->since > BIT_TIMEOUT_MS) {
		if (s->state == S_SETTLE) {
			// still low: whatever holds it is not us. Keep waiting, but
			// restart the clock so this does not fire every step.
			s->since = s->now;
		} else {
			abort_all(s);
		}
		return true;
	}
	return false;
}

// -- the transport interface ----------------------------------------------

static void t_poll(void *ctx, uint32_t now) {
	zl_soft_t *s = ctx;
	s->now = now;
	// a few steps: as far as we can go without waiting for the peer
	for (int i = 0; i < 4 && step(s); i++)
		;
}

static bool t_send(void *ctx, const uint8_t *f, uint16_t len) {
	zl_soft_t *s = ctx;
	uint16_t n = 0;
	if (s->tx_active) return false;
	s->tx[n++] = ZL_SOFT_END;
	s->tx[n++] = ZL_SOFT_END;
	for (uint16_t i = 0; i < len; i++) {
		uint8_t b = f[i];
		if (b == ZL_SOFT_END || b == ZL_SOFT_ESC || b == ZL_SOFT_INV) {
			s->tx[n++] = ZL_SOFT_ESC;
			s->tx[n++] = b ^ 0x20;
		} else {
			s->tx[n++] = b;
		}
	}
	s->tx[n++] = ZL_SOFT_END;
	s->tx_len = n;
	s->tx_pos = 0;
	s->tx_bit = 0;
	s->tx_active = true;
	return true;
}

static bool t_idle(void *ctx) {
	return !((zl_soft_t *)ctx)->tx_active;
}

static uint16_t t_recv(void *ctx, uint8_t *buf, uint16_t max) {
	zl_soft_t *s = ctx;
	uint16_t n = s->done_len;
	if (!n) return 0;
	s->done_len = 0;
	if (n > max) return 0;
	memcpy(buf, s->done, n);
	return n;
}

void zl_soft_init(zl_soft_t *s, const zl_pins_t *pins) {
	memset(s, 0, sizeof *s);
	s->pins = *pins;
	s->mine = -1;
	s->t.name = "soft";
	s->t.ctx = s;
	s->t.half_duplex = true;
	s->t.rto_ms = 400;
	s->t.send = t_send;
	s->t.tx_idle = t_idle;
	s->t.recv = t_recv;
	s->t.poll = t_poll;
	release_all(s);
	s->state = S_SETTLE;
}

void zl_soft_stop(zl_soft_t *s) {
	release_all(s);
	s->tx_active = false;
	s->aligned = false;
	s->rx_len = 0;
	s->done_len = 0;
	s->state = S_SETTLE;
}

bool zl_soft_busy(const zl_soft_t *s) {
	return s->tx_active || s->state != S_IDLE || (s->aligned && s->rx_len) ||
	       s->rx_bits || s->esc;
}

bool zl_soft_crossed(const zl_soft_t *s) {
	return s->crossed;
}
