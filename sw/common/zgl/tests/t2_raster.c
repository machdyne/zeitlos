/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A model of T2's raster (rtl/gpu/t2_raster.v): the geometry unit's
 * records (zgl_t2.h) drawn strip by strip, as the RTL draws them, into
 * a 320x240 picture of indices. Bit for bit what the RTL does, and what
 * zgl_frames.c's software strips do: the same edges (zgl's top-left
 * rule, here as edge functions), the same planes mod 2^32, the same
 * dither and depth tests.
 */
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "../zgl_t2.h"
#include "t2_raster.h"

#define W 320
#define H 240
#define STRIP_H 16

static const uint8_t bayer4[4][4] = {
	{   8, 136,  40, 168 },
	{ 200,  72, 232, 104 },
	{  56, 184,  24, 152 },
	{ 248, 120, 216,  88 }
};

static uint32_t rec[T2_RECS * T2_REC_WORDS];
static uint32_t nrec_w, nrec;
static int32_t fx0, fx1, fy0, fy1;
static uint8_t clear;

void t2r_frame(int32_t x0, int32_t x1, int32_t y0, int32_t y1, uint8_t c) {
	fx0 = x0; fx1 = x1; fy0 = y0; fy1 = y1; clear = c;
	nrec_w = 0;
	nrec = 0;
}

volatile uint32_t *t2r_rec(int n) {
	volatile uint32_t *p;
	if (nrec >= T2_RECS || nrec_w + (uint32_t)n > sizeof(rec) / 4) return 0;
	p = &rec[nrec_w];
	nrec_w += (uint32_t)n;
	nrec++;
	return p;
}

static int32_t sx16(uint32_t w) { return (int32_t)(int16_t)(w & 0xffff); }
static int32_t sy16(uint32_t w) { return (int32_t)(int16_t)(w >> 16); }

/* an edge a->b (a above): the sign of (X - a.x) dy - (Y - a.y) dx */
static int64_t edge(uint32_t a, uint32_t b, int32_t X, int32_t Y) {
	return (int64_t)(X - sx16(a)) * (sy16(b) - sy16(a)) - (int64_t)(Y - sy16(a)) * (sx16(b) - sx16(a));
}

static void tri(const uint32_t *w, int32_t sy0, int32_t sy1, uint8_t (*col)[W], uint16_t (*zb)[W], t2_stats_t *st) {
	uint32_t h = w[0];
	bool dep = (h >> 2) & 1, sr = (h >> 3) & 1;
	int32_t ylo = (int32_t)((h >> 8) & 0xff), yhi = (int32_t)((h >> 16) & 0xff), ymid = (int32_t)(h >> 24);
	int32_t bx0 = (int32_t)(w[1] & 0xffff), bx1 = (int32_t)(w[1] >> 16);
	int32_t r0 = ylo > sy0 ? ylo : sy0, r1 = yhi < sy1 ? yhi : sy1, y, x;
	if (r0 > r1) return;
	st->cyc_raster += 12;                                   /* the strip's setup */
	for (y = r0; y <= r1; y++) {
		int32_t Y = y * 16;
		uint32_t sa = y < ymid ? w[2] : w[3], sb = y < ymid ? w[3] : w[4];
		uint32_t zr = w[5] + w[7] * (uint32_t)(y - ylo), cr = w[8] + w[10] * (uint32_t)(y - ylo);
		st->cyc_raster += (uint32_t)((bx1 >> 2) - (bx0 >> 2) + 1) + 2;
		for (x = bx0; x <= bx1; x++) {
			int32_t X = x * 16;
			int64_t fl = edge(w[2], w[4], X, Y), fs = edge(sa, sb, X, Y);
			bool in = sr ? (fl >= 0 && fs < 0) : (fs >= 0 && fl < 0);
			uint32_t z, c;
			uint8_t ci;
			if (!in) continue;
			z = zr + w[6] * (uint32_t)(x - bx0);
			c = cr + w[9] * (uint32_t)(x - bx0);
			ci = (uint8_t)((c + bayer4[y & 3][x & 3]) >> 8);
			st->pixels++;
			if (dep) {
				uint32_t zv = (int32_t)z < 0 ? 0 : z >> 15;
				if (zv < zb[y - sy0][x]) { zb[y - sy0][x] = (uint16_t)zv; col[y - sy0][x] = ci; }
			} else col[y - sy0][x] = ci;
		}
	}
}

static void line(const uint32_t *w, int32_t sy0, int32_t sy1, uint8_t (*col)[W], uint16_t (*zb)[W], t2_stats_t *st) {
	bool dep = (w[0] >> 2) & 1;
	uint32_t n = w[1], i;
	uint32_t x = w[2], y = w[3], z = w[6], c = w[8];
	for (i = 0; i <= n; i++) {
		int32_t px = (int32_t)(x + (8u << 16)) >> 20, py = (int32_t)(y + (8u << 16)) >> 20;
		st->cyc_raster++;
		if (py >= sy0 && py <= sy1 && px >= fx0 && px <= fx1) {
			uint32_t zv = (int32_t)z < 0 ? 0 : z >> 15;
			st->cyc_raster += 3;
			if (!dep || zv <= zb[py - sy0][px]) {
				if (dep) zb[py - sy0][px] = (uint16_t)zv;
				col[py - sy0][px] = (uint8_t)((c + 128) >> 8);
			}
		}
		x += w[4]; y += w[5]; z += w[7]; c += w[9];
	}
}

const uint32_t *t2r_list(uint32_t *nwords) {
	*nwords = nrec_w;
	return rec;
}

void t2r_draw(uint8_t pic[240][320], t2_stats_t *st) {
	static uint8_t col[STRIP_H][W];
	static uint16_t zb[STRIP_H][W];
	int32_t sy0, y;
	uint32_t i, k;
	for (sy0 = fy0; sy0 <= fy1; sy0 += STRIP_H) {
		int32_t sy1 = sy0 + STRIP_H - 1 < fy1 ? sy0 + STRIP_H - 1 : fy1;
		memset(col, clear, sizeof(col));
		memset(zb, 0xff, sizeof(zb));
		for (i = 0, k = 0; i < nrec; i++) {
			const uint32_t *w = &rec[k];
			int32_t ylo = (int32_t)((w[0] >> 8) & 0xff), yhi = (int32_t)((w[0] >> 16) & 0xff);
			st->cyc_raster++;                               /* the scan */
			if (!(yhi < sy0 || ylo > sy1)) {
				if ((w[0] & 3) == T2_REC_TRI) tri(w, sy0, sy1, col, zb, st);
				else line(w, sy0, sy1, col, zb, st);
			}
			k += (w[0] & 3) == T2_REC_TRI ? T2_REC_WORDS : 10;
		}
		for (y = sy0; y <= sy1; y++)
			memcpy(&pic[y][fx0], &col[y - sy0][fx0], (size_t)(fx1 - fx0 + 1));
		st->cyc_out += (uint32_t)(((fx1 >> 2) - (fx0 >> 2) + 1) * (sy1 - sy0 + 1)) * 6;
	}
	st->rx0 = fx0; st->rx1 = fx1; st->ry0 = fy0; st->ry1 = fy1;
}
