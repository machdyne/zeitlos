/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zlink's fast transport -- sw/common/zlink_fast.h has the protocol.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zlink_fast.h"
#include "zfast.h"

#define K_SOF 0xFB		// K27.7
#define K_EOF 0xFD		// K29.7

#define LISTEN_MS   5		// on each pair before deciding nothing is there
#define BACKOFF_MS  40		// at most, random, before trying DS1
#define TRY_MS      80		// transmitting on DS1, waiting to hear back
#define QUIET_MS    10		// an ML2 waiting for DS1 to go quiet
#define LOST_MS     200		// linked, but nothing heard: start again
#define INV_ERRORS  16		// errors in a check period, no frame through: wrong polarity
				// (an idle link carries only HELLOs, ~27 symbols each)
#define INV_MS      50

enum { N_LISTEN1, N_LISTEN2, N_BACKOFF, N_TRY, N_WAIT, N_PAUSE, N_UP };
enum { R_HUNT, R_LEN, R_DATA, R_END };

static uint32_t rnd(zl_fast_t *f) {
	f->rng ^= f->rng << 13;
	f->rng ^= f->rng >> 17;
	f->rng ^= f->rng << 5;
	return f->rng;
}

static bool after(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

static void cfg(zl_fast_t *f, int tx, int rx) {
	f->tx = tx;
	f->rx = rx;
	z_zf_config(tx, rx, f->rate, Z_ZF_RXEN | (f->inv ? Z_ZF_RXINV : 0));
}

static void listen(zl_fast_t *f, int pair, uint8_t ns, uint32_t ms) {
	cfg(f, Z_ZF_NONE, pair);
	f->ns = ns;
	f->until = f->now + ms;
}

static void up(zl_fast_t *f, int tx, int rx) {
	cfg(f, tx, rx);
	z_zf_flush(true, true);
	f->ns = N_UP;
	f->rs = R_HUNT;
	f->last_aligned = f->now;
	f->err_last = 0;
	f->err_at = f->now;
}

static void restart(zl_fast_t *f) {
	f->rounds++;
	listen(f, Z_ZF_DS1, N_LISTEN1, LISTEN_MS);
}

static void negotiate(zl_fast_t *f) {
	bool al = z_zf_aligned();
	switch (f->ns) {
	case N_LISTEN1:
		if (al) {
			if (f->tx_ds2) up(f, Z_ZF_DS2, Z_ZF_DS1);
			else {
				f->ns = N_WAIT;		// an ML2: let DS1 go quiet first
				f->quiet_since = f->now;
				f->until = f->now + 500;
			}
		} else if (after(f->now, f->until)) {
			listen(f, Z_ZF_DS2, N_LISTEN2, LISTEN_MS);
		}
		break;
	case N_LISTEN2:
		if (al) up(f, Z_ZF_DS1, Z_ZF_DS2);
		else if (after(f->now, f->until)) {
			f->ns = N_BACKOFF;
			f->until = f->now + rnd(f) % BACKOFF_MS;
		}
		break;
	case N_BACKOFF:			// still listening on DS2
		if (al) up(f, Z_ZF_DS1, Z_ZF_DS2);
		else if (after(f->now, f->until)) {
			cfg(f, Z_ZF_DS1, Z_ZF_DS2);
			f->ns = N_TRY;
			f->until = f->now + TRY_MS;
		}
		break;
	case N_TRY:
		if (al) up(f, Z_ZF_DS1, Z_ZF_DS2);
		else if (after(f->now, f->until)) {
			// nobody answered: let go, and start again a random while
			// later than the other end will
			cfg(f, Z_ZF_NONE, Z_ZF_DS1);
			f->ns = N_PAUSE;
			f->until = f->now + rnd(f) % BACKOFF_MS;
		}
		break;
	case N_WAIT:			// an ML2, listening on DS1
		if (al) f->quiet_since = f->now;
		else if (f->now - f->quiet_since >= QUIET_MS) {
			cfg(f, Z_ZF_DS1, Z_ZF_DS2);
			f->ns = N_TRY;
			f->until = f->now + TRY_MS;
		}
		if (after(f->now, f->until)) restart(f);
		break;
	case N_PAUSE:
		if (after(f->now, f->until)) restart(f);
		break;
	case N_UP:
		if (al) f->last_aligned = f->now;
		else if (f->now - f->last_aligned > LOST_MS) {
			restart(f);
			break;
		}
		// Polarity. An inverted pair still gives commas (each K28.5 is
		// the other's complement), but every data symbol is an error --
		// and each error drops alignment until the next comma, so this
		// does not ask for alignment: errors, and no frame through.
		if (f->now - f->err_at >= INV_MS) {
			uint32_t e = z_zf_errors();
			uint32_t d = e >= f->err_last ? e - f->err_last : e;
			bool none = f->frames_rx == f->frames_at;
			f->err_last = e;
			f->err_at = f->now;
			f->frames_at = f->frames_rx;
			if (d >= INV_ERRORS && none) {
				f->inv = !f->inv;
				cfg(f, f->tx, f->rx);
				z_zf_flush(false, true);
				f->err_last = 0;
				f->rs = R_HUNT;
			}
		}
		break;
	}
}

// -- the transport --------------------------------------------------------

static bool t_send(void *ctx, const uint8_t *fr, uint16_t len) {
	zl_fast_t *f = ctx;
	uint8_t l[2];
	if (f->ns != N_UP || z_zf_tx_free() < (uint32_t)len + 4u) return false;
	l[0] = (uint8_t)len;
	l[1] = (uint8_t)(len >> 8);
	z_zf_write_k(K_SOF);
	z_zf_write(l, 2);
	z_zf_write(fr, len);
	z_zf_write_k(K_EOF);
	return true;
}

static bool t_idle(void *ctx) {
	zl_fast_t *f = ctx;
	return f->ns == N_UP && z_zf_tx_free() >= ZL_FRAME_MAX + 4u;
}

static void broken(zl_fast_t *f) {
	f->dropped++;
	f->rs = R_HUNT;
}

static uint16_t t_recv(void *ctx, uint8_t *buf, uint16_t max) {
	zl_fast_t *f = ctx;
	if (f->ns != N_UP) {
		if (z_zf_rx_level()) z_zf_flush(false, true);
		return 0;
	}
	for (;;) {
		int k;
		switch (f->rs) {
		case R_HUNT: {
			uint8_t junk[32];
			if (z_zf_read(junk, sizeof junk)) continue;
			k = z_zf_read_sym();
			if (k < 0) return 0;
			if (k == (0x100 | K_SOF)) {
				f->rs = R_LEN;
				f->got = 0;
			}
			continue;
		}
		case R_LEN:
			k = z_zf_read_sym();
			if (k < 0) return 0;
			if (k & 0x100) {
				if (k != (0x100 | K_SOF)) broken(f);
				else f->got = 0;
				continue;
			}
			if (f->got == 0) {
				f->want = (uint16_t)k;
				f->got = 1;
			} else {
				f->want |= (uint16_t)(k << 8);
				f->got = 0;
				if (f->want > ZL_FRAME_MAX || f->want == 0) broken(f);
				else f->rs = R_DATA;
			}
			continue;
		case R_DATA: {
			uint32_t left = (uint32_t)(f->want - f->got);
			if (left >= 4) {
				uint32_t w[32], n;
				n = left / 4u;
				if (n > 32) n = 32;
				n = z_zf_read_words(w, n);
				if (n) {
					for (uint32_t i = 0; i < n; i++) {
						f->rx_buf[f->got++] = (uint8_t)w[i];
						f->rx_buf[f->got++] = (uint8_t)(w[i] >> 8);
						f->rx_buf[f->got++] = (uint8_t)(w[i] >> 16);
						f->rx_buf[f->got++] = (uint8_t)(w[i] >> 24);
					}
					if (z_zf_kword()) {
						// a control symbol came out as data: damage
						z_zf_flush(false, false);
						broken(f);
					} else if (f->got == f->want) {
						f->rs = R_END;
					}
					continue;
				}
				if (z_zf_rx_level() >= 4) continue;
			}
			// the last one to three, or fewer than four waiting
			k = z_zf_read_sym();
			if (k < 0) return 0;
			if (k & 0x100) {
				broken(f);
				if (k == (0x100 | K_SOF)) { f->rs = R_LEN; f->got = 0; }
				continue;
			}
			f->rx_buf[f->got++] = (uint8_t)k;
			if (f->got == f->want) f->rs = R_END;
			continue;
		}
		case R_END:
			k = z_zf_read_sym();
			if (k < 0) return 0;
			if (k != (0x100 | K_EOF)) {
				broken(f);
				if (k == (0x100 | K_SOF)) { f->rs = R_LEN; f->got = 0; }
				continue;
			}
			f->rs = R_HUNT;
			if (f->want > max) { f->dropped++; continue; }
			memcpy(buf, f->rx_buf, f->want);
			f->frames_rx++;
			f->last_aligned = f->now;		// a frame is a sign of life too
			return f->want;
		}
	}
}

static void t_poll(void *ctx, uint32_t now) {
	zl_fast_t *f = ctx;
	f->now = now;
	negotiate(f);
}

void zl_fast_init(zl_fast_t *f, int rate, uint32_t seed) {
	memset(f, 0, sizeof *f);
	f->rate = rate;
	f->tx_ds2 = z_zf_tx_ds2();
	f->rng = seed ? seed : 0x2545f491u;
	f->t.name = "fast";
	f->t.ctx = f;
	f->t.half_duplex = false;
	f->t.rto_ms = 20;
	f->t.send = t_send;
	f->t.tx_idle = t_idle;
	f->t.recv = t_recv;
	f->t.poll = t_poll;
	z_zf_flush(true, true);
	listen(f, Z_ZF_DS1, N_LISTEN1, LISTEN_MS);
}

void zl_fast_stop(zl_fast_t *f) {
	(void)f;
	z_zf_off();
}

bool zl_fast_linked(const zl_fast_t *f) {
	return f->ns == N_UP;
}

const char *zl_fast_role(const zl_fast_t *f) {
	if (f->ns != N_UP) return f->ns == N_TRY ? "trying DS1" : "listening";
	if (f->tx == Z_ZF_DS1) return f->inv ? "transmits on DS1, receives on DS2 (inverted)"
	                                     : "transmits on DS1, receives on DS2";
	return f->inv ? "transmits on DS2, receives on DS1 (inverted)"
	              : "transmits on DS2, receives on DS1";
}

const char *zl_fast_hint(const zl_fast_t *f) {
	if (f->ns != N_UP && !f->tx_ds2 && f->rounds >= 8)
		return "no answer: if the other end is also a Sechzig ML2, use a crossover SATA cable";
	return NULL;
}
