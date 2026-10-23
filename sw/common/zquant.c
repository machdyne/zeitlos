/*
 * Zeitlos -- median cut and nearest colours. See zquant.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <string.h>

#include "zquant.h"

static int dist(uint16_t a, uint16_t b) {
	int dr = (int)((a >> 8) & 15) - (int)((b >> 8) & 15);
	int dg = (int)((a >> 4) & 15) - (int)((b >> 4) & 15);
	int db = (int)(a & 15) - (int)(b & 15);
	/* green counts most, blue least, as the eye has it */
	return dr * dr * 3 + dg * dg * 4 + db * db * 2;
}

int zq_lum(uint16_t c) {
	return (int)(((c >> 8) & 15) * 3 + ((c >> 4) & 15) * 6 + (c & 15)) / 10;
}

void zq_nearest_lut(uint8_t *lut, const uint16_t *pal, int n,
	const uint32_t *only) {
	int c, i;
	for (c = 0; c < 4096; c++) {
		int best = 0, bd = 1 << 30;
		if (only && !only[c]) continue;
		for (i = 0; i < n; i++) {
			int d = dist((uint16_t)c, pal[i]);
			if (d < bd) { bd = d; best = i; if (!d) break; }
		}
		lut[c] = (uint8_t)best;
	}
}

typedef struct { uint8_t lo[3], hi[3]; uint32_t n; } box_t;

static uint32_t box_count(const uint32_t *hist, box_t *b) {
	int r, g, bl;
	uint8_t lo[3] = { 15, 15, 15 }, hi[3] = { 0, 0, 0 };
	uint32_t n = 0;
	for (r = b->lo[0]; r <= b->hi[0]; r++)
		for (g = b->lo[1]; g <= b->hi[1]; g++)
			for (bl = b->lo[2]; bl <= b->hi[2]; bl++) {
				uint32_t k = hist[(r << 8) | (g << 4) | bl];
				if (!k) continue;
				n += k;
				if (r < lo[0]) lo[0] = (uint8_t)r;
				if (r > hi[0]) hi[0] = (uint8_t)r;
				if (g < lo[1]) lo[1] = (uint8_t)g;
				if (g > hi[1]) hi[1] = (uint8_t)g;
				if (bl < lo[2]) lo[2] = (uint8_t)bl;
				if (bl > hi[2]) hi[2] = (uint8_t)bl;
			}
	if (n) { memcpy(b->lo, lo, 3); memcpy(b->hi, hi, 3); }
	b->n = n;
	return n;
}

int zq_median_cut(const uint32_t *hist, uint16_t *out, int want) {
	static box_t box[256];
	int nb = 1, i, c;

	if (want < 1) want = 1;
	if (want > 256) want = 256;

	/* few enough colours: those */
	for (c = 0, i = 0; c < 4096; c++)
		if (hist[c]) {
			if (i == want) break;
			out[i++] = (uint16_t)c;
		}
	if (c == 4096) return i;

	box[0].lo[0] = box[0].lo[1] = box[0].lo[2] = 0;
	box[0].hi[0] = box[0].hi[1] = box[0].hi[2] = 15;
	box_count(hist, &box[0]);
	while (nb < want) {
		int best = -1, ax = 0, k;
		uint64_t score = 0;
		for (i = 0; i < nb; i++)
			for (k = 0; k < 3; k++) {
				int side = box[i].hi[k] - box[i].lo[k];
				uint64_t s = (uint64_t)box[i].n * (uint64_t)side;
				if (side > 0 && s > score) { score = s; best = i; ax = k; }
			}
		if (best < 0) break;                /* every box one colour */
		{
			/* the median along ax */
			box_t a = box[best], b = box[best];
			uint32_t half = box[best].n / 2, run = 0;
			int cut = a.lo[ax], v, r, g, bl;
			for (v = a.lo[ax]; v < a.hi[ax]; v++) {
				for (r = a.lo[0]; r <= a.hi[0]; r++)
					for (g = a.lo[1]; g <= a.hi[1]; g++)
						for (bl = a.lo[2]; bl <= a.hi[2]; bl++) {
							int c3[3];
							c3[0] = r; c3[1] = g; c3[2] = bl;
							if (c3[ax] == v) run += hist[(r << 8) | (g << 4) | bl];
						}
				cut = v;
				if (run >= half) break;
			}
			a.hi[ax] = (uint8_t)cut;
			b.lo[ax] = (uint8_t)(cut + 1);
			box_count(hist, &a);
			box_count(hist, &b);
			box[best] = a;
			box[nb++] = b;
		}
	}
	for (i = 0; i < nb; i++) {
		uint64_t sr = 0, sg = 0, sb = 0, n = 0;
		int r, g, bl;
		for (r = box[i].lo[0]; r <= box[i].hi[0]; r++)
			for (g = box[i].lo[1]; g <= box[i].hi[1]; g++)
				for (bl = box[i].lo[2]; bl <= box[i].hi[2]; bl++) {
					uint32_t k = hist[(r << 8) | (g << 4) | bl];
					sr += (uint64_t)k * (uint64_t)r; sg += (uint64_t)k * (uint64_t)g;
					sb += (uint64_t)k * (uint64_t)bl; n += k;
				}
		if (!n) n = 1;
		out[i] = (uint16_t)((((sr + n / 2) / n) << 8) | (((sg + n / 2) / n) << 4) | ((sb + n / 2) / n));
	}
	return nb;
}
