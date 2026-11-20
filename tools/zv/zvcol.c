// zvcol.c - the encoder's colour layer (see sw/common/zv.h)
//
// Each cell's (8x8, or 4x4) mean chroma, frame by frame. A group of frames (one
// seek point's) gets 16 chroma classes by k-means, class 0 held neutral;
// cells then take the nearest class, keeping the one they had unless
// another is clearly nearer (so colours don't flicker), and only cells
// that change are sent.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "zvcol.h"
#include "zv.h"

// A cell keeps its class unless another is nearer by more than
// HYST_MUL/16 of the distance plus HYST_ADD (squared chroma units).
// Measured on Big Buck Bunny at 320x176, 4x4 cells (ZVCOL_STATS=1):
//   4/24: 6.3 KB/s, 135 cells changed a frame, error 5.60
//   8/48: 5.7 KB/s, 116 cells,                 error 5.93  <- this
//   16/96: 5.0 KB/s, 91 cells,                 error 7.03
#ifndef HYST_MUL
#define HYST_MUL 8
#endif
#ifndef HYST_ADD
#define HYST_ADD 48
#endif

struct zvc {
	int cw, ch, cells, w, h, cell;
	int8_t *uv;         // buffered frames: cells x 2 each
	uint8_t *wt;        // and each cell's weight (from its brightness)
	int nf, cap;
	uint32_t first;     // the buffered group's first frame
	uint32_t frame;     // frames seen
	uint8_t *out;       // the groups' data
	size_t len, ocap;
	uint32_t *gidx;     // first frame, offset (from the section's start, filled at the end)
	uint32_t ng, gcap;
	uint64_t coloured, total, sent_cells, changed;
	double tw, tu, tv, tuu, tvv, tuv;	// the whole film's chroma, weighted: one tint?
	double err;             // the weighted chroma error of the classes chosen
	// the last group's classes and each cell's class at its end: the
	// next group starts from them, so a class keeps its index (and
	// roughly its colour) across a seek point instead of jumping
	int pcen[ZV_COL_CLASSES][2], pnk;
	uint8_t *last;
};

static void put(zvc_t *c, const void *p, size_t n) {
	if (c->len + n > c->ocap) {
		c->ocap = (c->len + n) * 2 + 65536;
		c->out = realloc(c->out, c->ocap);
	}
	memcpy(c->out + c->len, p, n);
	c->len += n;
}

zvc_t *zvc_new(int w, int h, int cell) {
	zvc_t *c = calloc(1, sizeof(*c));
	c->w = w; c->h = h;
	c->cell = cell == 4 ? 4 : 8;
	c->cw = w / c->cell; c->ch = h / c->cell;
	c->cells = c->cw * c->ch;
	c->last = calloc((size_t)c->cells, 1);
	return c;
}

static int d2(int u0, int v0, int u1, int v1) { return (u0 - u1) * (u0 - u1) + (v0 - v1) * (v0 - v1); }

// a 64th of the cells a frame: 75 of a 320x240 film's 4x4 cells, some
// 20 to 40 blocks -- what the colour window's planes or game mode's
// blitter redraw in about a tenth of a frame on a 48 MHz board
#define CHANGE_DIV 64
static int cmp_fdesc(const void *a, const void *b) {
	float x = *(const float *)a, y = *(const float *)b;
	return x < y ? 1 : x > y ? -1 : 0;
}

static void flush_group(zvc_t *c) {
	int cen[ZV_COL_CLASSES][2], k, it, f, i, nk = 1;
	int8_t tab[ZV_COL_TABLE];
	uint8_t *cls, *prev;
	double su[ZV_COL_CLASSES], sv[ZV_COL_CLASSES], sw[ZV_COL_CLASSES];
	int step = c->nf > 12 ? 2 : 1;
	if (!c->nf) return;
	memset(cen, 0, sizeof(cen));
	// start from the last group's classes (in their places), then fill
	// the rest farthest-point: each next centre where weight x distance
	// is largest
	for (k = 1; k < c->pnk; k++) { cen[k][0] = c->pcen[k][0]; cen[k][1] = c->pcen[k][1]; }
	if (c->pnk > 1) nk = c->pnk;
	for (k = 1; k < ZV_COL_CLASSES; k++) {
		double best = 0;
		int bu = 0, bv = 0, j2;
		// a slot to fill: past the last group's, or one of its that went grey
		if (k < nk && (cen[k][0] || cen[k][1])) continue;
		for (f = 0; f < c->nf; f += step)
			for (i = 0; i < c->cells; i++) {
				const int8_t *p = c->uv + ((size_t)f * c->cells + i) * 2;
				int j, dm = 1 << 30;
				double s;
				for (j = 0; j < (k < nk ? nk : k); j++) {
					int d;
					if (j && j != k && !cen[j][0] && !cen[j][1]) continue;
					if (j == k) continue;
					d = d2(p[0], p[1], cen[j][0], cen[j][1]);
					if (d < dm) dm = d;
				}
				s = (double)dm * c->wt[(size_t)f * c->cells + i];
				if (s > best) { best = s; bu = p[0]; bv = p[1]; }
			}
		if (best < 9.0 * 64) { if (k >= nk) break; continue; }   // nothing left that's coloured
		cen[k][0] = bu; cen[k][1] = bv;
		if (k >= nk) nk = k + 1;
		(void)j2;
	}
	for (it = 0; it < 8 && nk > 1; it++) {
		memset(su, 0, sizeof(su)); memset(sv, 0, sizeof(sv)); memset(sw, 0, sizeof(sw));
		for (f = 0; f < c->nf; f += step)
			for (i = 0; i < c->cells; i++) {
				const int8_t *p = c->uv + ((size_t)f * c->cells + i) * 2;
				double w = c->wt[(size_t)f * c->cells + i];
				int j, bj = 0, dm = 1 << 30;
				for (j = 0; j < nk; j++) { int d = d2(p[0], p[1], cen[j][0], cen[j][1]); if (d < dm) { dm = d; bj = j; } }
				su[bj] += w * p[0]; sv[bj] += w * p[1]; sw[bj] += w;
			}
		for (k = 1; k < nk; k++)
			if (sw[k] > 0) { cen[k][0] = (int)lrint(su[k] / sw[k]); cen[k][1] = (int)lrint(sv[k] / sw[k]); }
	}
	// near-neutral classes: the palette can't show them apart from grey
	for (k = 1; k < nk; k++)
		if (d2(cen[k][0], cen[k][1], 0, 0) < 16) { cen[k][0] = 0; cen[k][1] = 0; }
	memcpy(c->pcen, cen, sizeof(cen));
	c->pnk = nk;
	memset(tab, 0, sizeof(tab));
	for (k = 1; k < ZV_COL_CLASSES; k++) {
		int u = k < nk ? cen[k][0] : 0, v = k < nk ? cen[k][1] : 0;
		tab[k * 2] = (int8_t)(u < -128 ? -128 : u > 127 ? 127 : u);
		tab[k * 2 + 1] = (int8_t)(v < -128 ? -128 : v > 127 ? 127 : v);
	}

	// the group
	if (c->ng == c->gcap) { c->gcap = c->gcap ? c->gcap * 2 : 256; c->gidx = realloc(c->gidx, c->gcap * 2 * sizeof(uint32_t)); }
	c->gidx[c->ng * 2] = c->first;
	c->gidx[c->ng * 2 + 1] = (uint32_t)c->len;
	c->ng++;
	put(c, tab, sizeof(tab));
	cls = calloc((size_t)c->cells, 1);
	prev = calloc((size_t)c->cells, 1);
	memcpy(prev, c->last, (size_t)c->cells);
	{
		uint8_t *buf = malloc((size_t)c->cells * 2 + 16), *chg = malloc((size_t)c->cells);
		float *gain = malloc((size_t)c->cells * sizeof(float)), *srt = malloc((size_t)c->cells * sizeof(float));
		for (f = 0; f < c->nf; f++) {
			size_t n = 2;
			int at = 0;
			for (i = 0; i < c->cells; i++) {
				const int8_t *p = c->uv + ((size_t)f * c->cells + i) * 2;
				int j, bj = 0, dm = 1 << 30, dp;
				for (j = 0; j < ZV_COL_CLASSES; j++) {
					int d = d2(p[0], p[1], tab[j * 2], tab[j * 2 + 1]);
					if (d < dm) { dm = d; bj = j; }
				}
				dp = d2(p[0], p[1], tab[prev[i] * 2], tab[prev[i] * 2 + 1]);
				// keep the class it had unless another is clearly nearer
				// (the group's first frame too: it sends every cell, but
				// the classes carry on from the last group's end)
				cls[i] = (uint8_t)(dp <= dm + dm * HYST_MUL / 16 + HYST_ADD ? prev[i] : bj);
				gain[i] = cls[i] != prev[i] ? (float)((double)(dp - dm) * c->wt[(size_t)f * c->cells + i]) : 0;
			}
			// No more cells change class in a frame than the board has
			// time to redraw (CHANGE_MAX of them, the ones that gain
			// most): the rest keep their class and stand again next
			// frame. A group's first frame sends them all regardless.
			if (f) {
				int nch = 0, cap = c->cells / CHANGE_DIV;
				for (i = 0; i < c->cells; i++) if (gain[i] > 0) nch++;
				if (nch > cap) {
					float thr;
					memcpy(srt, gain, (size_t)c->cells * sizeof(float));
					qsort(srt, (size_t)c->cells, sizeof(float), cmp_fdesc);
					thr = srt[cap];
					for (i = 0; i < c->cells; i++) if (gain[i] > 0 && gain[i] <= thr) cls[i] = prev[i];
				}
			}
			for (i = 0; i < c->cells; i++) {
				const int8_t *p = c->uv + ((size_t)f * c->cells + i) * 2;
				c->err += (double)d2(p[0], p[1], tab[cls[i] * 2], tab[cls[i] * 2 + 1]) * c->wt[(size_t)f * c->cells + i];
				if (f && cls[i] != prev[i]) c->changed++;
				chg[i] = !f || cls[i] != prev[i];
				if (cls[i]) c->coloured++;
				c->total++;
			}
			// runs: a gap of one or two unchanged cells costs less inside a
			// literal run (half a byte each) than as a run of its own
			while (at < c->cells) {
				int k2;
				if (!chg[at]) {
					for (k2 = 0; at + k2 < c->cells && !chg[at + k2] && k2 < 128; k2++) { }
					buf[n++] = (uint8_t)(k2 - 1);
					at += k2;
					continue;
				}
				for (k2 = 0; at + k2 < c->cells && k2 < 128; k2++) {
					if (chg[at + k2]) continue;
					if (at + k2 + 2 < c->cells && (chg[at + k2 + 1] || chg[at + k2 + 2]) && k2 + 2 < 128) continue;
					break;
				}
				buf[n++] = (uint8_t)(0x80 | (k2 - 1));
				for (i = 0; i < k2; i += 2) {
					buf[n++] = (uint8_t)(cls[at + i] | (i + 1 < k2 ? cls[at + i + 1] << 4 : 0));
				}
				c->sent_cells += (uint64_t)k2;
				at += k2;
			}
			buf[0] = (uint8_t)(n - 2); buf[1] = (uint8_t)((n - 2) >> 8);
			put(c, buf, n);
			memcpy(prev, cls, (size_t)c->cells);
		}
		free(buf); free(chg); free(gain); free(srt);
	}
	memcpy(c->last, prev, (size_t)c->cells);
	free(cls); free(prev);
	c->first += (uint32_t)c->nf;
	c->nf = 0;
}

void zvc_frame(zvc_t *c, const uint8_t *y, const uint8_t *u, const uint8_t *v, int key) {
	int cx, cy, x, yy;
	int8_t *p;
	uint8_t *w;
	if (key && c->nf) flush_group(c);
	if (c->nf == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 64;
		c->uv = realloc(c->uv, (size_t)c->cap * c->cells * 2);
		c->wt = realloc(c->wt, (size_t)c->cap * c->cells);
	}
	p = c->uv + (size_t)c->nf * c->cells * 2;
	w = c->wt + (size_t)c->nf * c->cells;
	for (cy = 0; cy < c->ch; cy++)
		for (cx = 0; cx < c->cw; cx++) {
			int su = 0, sv = 0, sy = 0, m;
			for (yy = 0; yy < c->cell; yy++)
				for (x = 0; x < c->cell; x++) {
					size_t o = (size_t)(cy * c->cell + yy) * c->w + cx * c->cell + x;
					su += u[o] - 128; sv += v[o] - 128; sy += y[o];
				}
			{
				int n = c->cell * c->cell;
				*p++ = (int8_t)((su + (su >= 0 ? n / 2 : -n / 2)) / n);
				*p++ = (int8_t)((sv + (sv >= 0 ? n / 2 : -n / 2)) / n);
				// colour shows least in the darkest and lightest cells
				sy /= n;
			}
			m = sy < 255 - sy ? sy : 255 - sy;
			*w = (uint8_t)(m + 16 > 255 ? 255 : m + 16);
			c->tw += *w; c->tu += *w * (double)p[-2]; c->tv += *w * (double)p[-1];
			c->tuu += *w * (double)p[-2] * p[-2]; c->tvv += *w * (double)p[-1] * p[-1];
			c->tuv += *w * (double)p[-2] * p[-1];
			w++;
		}
	c->nf++;
	c->frame++;
}

size_t zvc_finish(zvc_t *c, uint8_t **out, double *coloured) {
	uint8_t *s;
	size_t hl, i;
	flush_group(c);
	*coloured = c->total ? (double)c->coloured / (double)c->total : 0;
	if (getenv("ZVCOL_STATS"))
		fprintf(stderr, "zvcol: %u frames, %.1f cells changed a frame, chroma error %.2f\n", c->frame,
			c->frame ? (double)c->changed / c->frame : 0.0, c->total ? sqrt(c->err / (double)c->total / 64.0) : 0.0);
	hl = ZV_COL_HDR + (size_t)c->ng * 8;
	s = malloc(hl + c->len);
	memset(s, 0, hl);
	s[0] = 'Z'; s[1] = 'V'; s[2] = 'C'; s[3] = '1';
	s[4] = (uint8_t)c->cw; s[5] = (uint8_t)(c->cw >> 8);
	s[6] = (uint8_t)c->ch; s[7] = (uint8_t)(c->ch >> 8);
	s[8] = (uint8_t)c->ng; s[9] = (uint8_t)(c->ng >> 8); s[10] = (uint8_t)(c->ng >> 16); s[11] = (uint8_t)(c->ng >> 24);
	s[12] = (uint8_t)c->cell;
	for (i = 0; i < c->ng * 2; i++) {
		uint32_t v = c->gidx[i] + ((i & 1) ? (uint32_t)hl : 0);
		s[16 + i * 4] = (uint8_t)v; s[17 + i * 4] = (uint8_t)(v >> 8);
		s[18 + i * 4] = (uint8_t)(v >> 16); s[19 + i * 4] = (uint8_t)(v >> 24);
	}
	memcpy(s + hl, c->out, c->len);
	*out = s;
	return hl + c->len;
}

double zvc_tint(const zvc_t *c, double *u, double *v) {
	double mu, mv, m, du, dv, euu, evv, euv, var;
	if (!c->tw) { *u = *v = 0; return 0; }
	mu = c->tu / c->tw; mv = c->tv / c->tw;
	euu = c->tuu / c->tw; evv = c->tvv / c->tw; euv = c->tuv / c->tw;
	*u = mu; *v = mv;
	m = sqrt(mu * mu + mv * mv);
	if (m < 2.0) {
		// about grey: how far from it, any way
		var = euu - mu * mu + evv - mv * mv;
	} else {
		// A tint is one hue, stronger in the mid-tones and fainter in
		// the shadows: the cells lie along the line from grey through
		// the mean. How far they lie off it.
		du = mu / m; dv = mv / m;
		var = dv * dv * euu - 2 * du * dv * euv + du * du * evv;
	}
	return var > 0 ? sqrt(var) : 0;
}

void zvc_free(zvc_t *c) {
	if (!c) return;
	free(c->uv); free(c->wt); free(c->out); free(c->gidx); free(c->last); free(c);
}
