/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zlink's stream transport -- sw/common/zlink_stream.h has the format.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zlink_stream.h"
#include "zgpio_stream.h"

#define K_SOF Z_GS_K27_7
#define K_EOF Z_GS_K29_7

static bool t_send(void *ctx, const uint8_t *f, uint16_t len) {
	zl_stream_t *s = ctx;
	// all or nothing: a frame half in the FIFO would go out with a gap
	// the receiver cannot tell from the end of it
	if (z_gs_tx_free(s->engine) < (uint32_t)len + 2) return false;
	z_gs_write_k(s->engine, K_SOF);
	z_gs_write(s->engine, f, len);
	z_gs_write_k(s->engine, K_EOF);
	return true;
}

// Room for another whole frame: the link layer can keep two in flight
// without waiting for the first to leave.
static bool t_idle(void *ctx) {
	zl_stream_t *s = ctx;
	return z_gs_tx_free(s->engine) >= ZL_FRAME_MAX + 2;
}

static uint16_t t_recv(void *ctx, uint8_t *buf, uint16_t max) {
	zl_stream_t *s = ctx;
	for (;;) {
		int k;
		if (s->in_frame) {
			// bytes up to the next control symbol
			uint32_t room = sizeof s->rx - s->rx_len;
			uint32_t n = z_gs_read(s->engine, s->rx + s->rx_len, room);
			s->rx_len = (uint16_t)(s->rx_len + n);
			if (n && s->rx_len == sizeof s->rx) {
				// full: anything but an end symbol next means too long
				k = z_gs_read_sym(s->engine);
				if (k < 0) return 0;	// decide when it arrives
				if (k != (0x100 | K_EOF)) {
					s->in_frame = false;
					s->dropped++;
					if (k == (0x100 | K_SOF)) { s->in_frame = true; s->rx_len = 0; }
					continue;
				}
			} else {
				if (n) continue;
				k = z_gs_read_sym(s->engine);
				if (k < 0) return 0;
				if (!(k & 0x100)) {
					// a data byte: z_gs_read() left it because we
					// were full, or it raced in -- keep it
					if (s->rx_len < sizeof s->rx) s->rx[s->rx_len++] = (uint8_t)k;
					continue;
				}
			}
			if (k == (0x100 | K_EOF)) {
				uint16_t n2 = s->rx_len;
				s->in_frame = false;
				s->rx_len = 0;
				if (n2 > max) { s->dropped++; continue; }
				memcpy(buf, s->rx, n2);
				s->frames_rx++;
				return n2;
			}
			if (k == (0x100 | K_SOF)) {
				// a new frame before this one ended: this one is lost
				s->dropped++;
				s->rx_len = 0;
				continue;
			}
			// any other control symbol: not ours, abandon the frame
			s->dropped++;
			s->in_frame = false;
			s->rx_len = 0;
		} else {
			uint8_t junk[32];
			// between frames: skip to a start symbol
			if (z_gs_read(s->engine, junk, sizeof junk)) continue;
			k = z_gs_read_sym(s->engine);
			if (k < 0) return 0;
			if (k == (0x100 | K_SOF)) {
				s->in_frame = true;
				s->rx_len = 0;
			}
		}
	}
}

static void t_poll(void *ctx, uint32_t now) {
	(void)ctx;
	(void)now;		// the engine does the work
}

void zl_stream_init(zl_stream_t *s, int engine) {
	memset(s, 0, sizeof *s);
	s->engine = engine;
	s->t.name = "stream";
	s->t.ctx = s;
	s->t.half_duplex = false;
	s->t.rto_ms = 30;
	s->t.send = t_send;
	s->t.tx_idle = t_idle;
	s->t.recv = t_recv;
	s->t.poll = t_poll;
}
