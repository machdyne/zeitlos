/*
 * planedemo -- pictures and the work spread over frames. See pd_gfx.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>

#include "pd.h"
#include "pd_gfx.h"

#ifndef PD_HOST
#include "../../common/zeitlos.h"
#endif

/* -- chunky -- */

bool pd_chunky_new(pd_chunky_t *c, int w, int h) {
	c->w = w; c->h = h;
	c->px = (uint8_t *)calloc((size_t)w * (size_t)h, 1);
	return c->px != NULL;
}

void pd_chunky_free(pd_chunky_t *c) {
	free(c->px);
	c->px = NULL;
}

void pdc_fill(pd_chunky_t *c, int x, int y, int w, int h, int v) {
	int j;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > c->w) w = c->w - x;
	if (y + h > c->h) h = c->h - y;
	if (w <= 0 || h <= 0) return;
	for (j = 0; j < h; j++) memset(c->px + (y + j) * c->w + x, v, (size_t)w);
}

void pdc_line(pd_chunky_t *c, int x0, int y0, int x1, int y1, int v) {
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
	int err = dx + dy, e2;
	for (;;) {
		pdc_set(c, x0, y0, v);
		if (x0 == x1 && y0 == y1) break;
		e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

void pdc_disc(pd_chunky_t *c, int cx, int cy, int r, int v) {
	int x, y;
	for (y = -r; y <= r; y++)
		for (x = -r; x <= r; x++)
			if (x * x + y * y <= r * r) pdc_set(c, cx + x, cy + y, v);
}

int pdc_text_w(const char *s, int scale) {
	return (int)strlen(s) * 8 * scale;
}

void pdc_text(pd_chunky_t *c, int x, int y, const char *s, int v, int scale) {
	const z_font_t *f = &z_font_8x8;
	for (; *s; s++, x += 8 * scale) {
		int gi = z_font_index(f, (uint8_t)*s), r, b, a, d;
		if (gi < 0) continue;
		for (r = 0; r < f->h; r++) {
			uint8_t bits = f->glyphs[gi * f->h + r];
			for (b = 0; b < 8; b++)
				if (bits & (0x80u >> b))
					for (d = 0; d < scale; d++)
						for (a = 0; a < scale; a++)
							pdc_set(c, x + b * scale + a, y + r * scale + d, v);
		}
	}
}

/* -- planar -- */

bool pd_planar_from(pd_planar_t *p, const pd_chunky_t *c, int bit0, int n, int transparent) {
	int k, x, y, words;
	memset(p, 0, sizeof(*p));
	p->w = c->w; p->h = c->h; p->n = n;
	p->stride = ((c->w + 31) / 32) * 4;
	/* +1 word: the blitter may read one past the last it needs */
	words = (p->stride / 4) * c->h + 1;
	for (k = 0; k < n; k++) {
		p->pl[k] = (uint32_t *)calloc((size_t)words, 4);
		if (!p->pl[k]) return false;
	}
	if (transparent >= 0) {
		p->mask = (uint32_t *)calloc((size_t)words, 4);
		if (!p->mask) return false;
	}
	for (y = 0; y < c->h; y++) {
		const uint8_t *row = c->px + y * c->w;
		int o = y * (p->stride / 4);
		for (x = 0; x < c->w; x++) {
			int v = row[x];
			uint32_t bit = 1u << (x & 31);
			if (p->mask && v != transparent) p->mask[o + (x >> 5)] |= bit;
			v >>= bit0;
			for (k = 0; k < n; k++)
				if ((v >> k) & 1) p->pl[k][o + (x >> 5)] |= bit;
		}
	}
	return true;
}

/* -- made on the build host -- */

static void st_put(pd_store_t *st, const void *d, uint32_t n) {
	uint32_t pad = (4 - (n & 3)) & 3;
	if (st->bad) return;
	if (st->len + n + pad > st->cap) {
		uint32_t cap = st->cap ? st->cap * 2 : 1u << 20;
		uint8_t *b;
		while (cap < st->len + n + pad) cap *= 2;
		b = (uint8_t *)realloc(st->buf, cap);
		if (!b) { st->bad = true; return; }
		st->buf = b;
		st->cap = cap;
	}
	if (d) memcpy(st->buf + st->len, d, n); else memset(st->buf + st->len, 0, n);
	memset(st->buf + st->len + n, 0, pad);
	st->len += n + pad;
}

/* the next n bytes of what is read, padded to 4; NULL past the end */
static const uint8_t *st_get(pd_store_t *st, uint32_t n) {
	const uint8_t *p;
	uint32_t pad = (4 - (n & 3)) & 3;
	if (st->bad || st->pos + n + pad > st->in_len) { st->bad = true; return NULL; }
	p = st->in + st->pos;
	st->pos += n + pad;
	return p;
}

void pd_store_u32(pd_store_t *st, uint32_t *v) {
	if (st->writing) st_put(st, v, 4);
	else { const uint8_t *p = st_get(st, 4); if (p) memcpy(v, p, 4); }
}

void pd_store_bytes(pd_store_t *st, void *data, uint32_t len) {
	uint32_t n = len;
	pd_store_u32(st, &n);
	if (st->writing) { st_put(st, data, len); return; }
	if (n != len) { st->bad = true; return; }
	{
		const uint8_t *p = st_get(st, len);
		if (p) memcpy(data, p, len);
	}
}

void pd_store_block(pd_store_t *st, void **data, uint32_t *len) {
	pd_store_u32(st, len);
	if (st->writing) { st_put(st, *data, *len); return; }
	*data = (void *)(uintptr_t)st_get(st, *len);
}

void pd_store_planar(pd_store_t *st, pd_planar_t *p) {
	uint32_t hd[5], words, k;
	if (st->writing) {
		hd[0] = (uint32_t)p->w; hd[1] = (uint32_t)p->h; hd[2] = (uint32_t)p->stride;
		hd[3] = (uint32_t)p->n; hd[4] = p->mask != NULL;
	}
	for (k = 0; k < 5; k++) pd_store_u32(st, &hd[k]);
	if (!st->writing) {
		memset(p, 0, sizeof(*p));
		if (st->bad || hd[3] > 8) { st->bad = true; return; }
		p->w = (int)hd[0]; p->h = (int)hd[1]; p->stride = (int)hd[2]; p->n = (int)hd[3];
	}
	/* +1 word: the blitter may read one past the last it needs */
	words = (uint32_t)(p->stride / 4) * (uint32_t)p->h + 1;
	for (k = 0; k < (uint32_t)p->n + hd[4]; k++) {
		uint32_t **pl = k < (uint32_t)p->n ? &p->pl[k] : &p->mask;
		if (st->writing) st_put(st, *pl, words * 4);
		else *pl = (uint32_t *)(uintptr_t)st_get(st, words * 4);
	}
}

/* -- fewer planes -- */

int pd_planes = 8;
static int phys[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
static int logic[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };

void pd_keep(unsigned m6, unsigned m4) {
	unsigned m = pd_planes >= 8 ? 0xffu : pd_planes >= 6 ? m6 : m4;
	int k, n = 0;
	for (k = 0; k < 8; k++) { phys[k] = -1; logic[k] = -1; }
	for (k = 0; k < 8 && n < pd_planes; k++)
		if ((m >> k) & 1) { phys[k] = n; logic[n] = k; n++; }
}

int pd_phys(int k) { return k >= 0 && k < 8 ? phys[k] : -1; }
#define PHYS(k) ((unsigned)(k) < 8u ? phys[k] : -1)

unsigned pd_logical(unsigned j) {
	unsigned v = 0;
	int b;
	for (b = 0; b < 8; b++)
		if (((j >> b) & 1) && logic[b] >= 0) v |= 1u << logic[b];
	return v;
}

/* -- the frame's drawing, queued (docs/planedemo.md, "Racing the beam") --
 *
 * Every plane operation of a frame is kept here, with its source (the
 * pictures live for the whole film, so a pointer is enough), and run
 * at the frame boundary: pd_queue_band() runs the part of each that
 * falls in a band of rows, in the order they were asked for, so the
 * top of the picture is finished first, ahead of the beam. A line
 * falls in every band it crosses and is drawn there clipped to it (the
 * same pixels: the rasterizer clips pixel by pixel). */

enum { Q_COPY, Q_OR, Q_ANDN, Q_FILL, Q_LINE };
typedef struct {
	uint8_t op, k;
	int8_t arg;                     /* a fill's bit, a line's op */
	uint8_t whole;                  /* a line drawn whole, in its last band */
	const void *bits;
	int32_t stride;
	int16_t sx, sy, x, y, w, h;     /* a line: x0 y0 (x y), x1 y1 (w h) */
	int16_t y0, y1;                 /* the rows it touches, y0 .. y1-1 */
} pd_cmd_t;

#define PD_Q_MAX 3072
static pd_cmd_t q[PD_Q_MAX];
static int qn;
static bool queue_on;

void pd_queue(bool on) {
	if (!on) pd_queue_flush();
	queue_on = on;
}

int pd_queue_len(void) { return qn; }

static void run_rows(const pd_cmd_t *c, int r0, int r1) {
	int a = c->y0 > r0 ? c->y0 : r0, b = c->y1 < r1 ? c->y1 : r1;
	if (a >= b) return;
	switch (c->op) {
	case Q_COPY: ar_plane(c->k, c->bits, c->stride, c->sx, c->sy + (a - c->y), c->x, a, c->w, b - a); break;
	case Q_OR:   ar_plane_or(c->k, c->bits, c->stride, c->sx, c->sy + (a - c->y), c->x, a, c->w, b - a); break;
	case Q_ANDN: ar_plane_andn(c->k, c->bits, c->stride, c->sx, c->sy + (a - c->y), c->x, a, c->w, b - a); break;
	case Q_FILL: ar_plane_fill(c->k, c->x, a, c->w, b - a, c->arg); break;
	case Q_LINE: ar_plane_line_rows(c->k, c->x, c->y, c->w, c->h, c->arg, a, b - 1); break;
	}
}

/* Before the first band: which lines can be drawn whole, once, at the
 * end of the band of their last row -- those nothing queued after them
 * in their plane and rows would change: only lines that set and ORs
 * come after (both commute with the line), so the line may as well go
 * after everything else in that band. Drawn then, the rows above are
 * still ahead of the beam and already hold everything asked before it.
 * A line a band at a time costs a call a band, and going through every
 * line for every band costs more than drawing them: the vectors are
 * 400 lines. So the rest is gone through band by band (few), and the
 * whole lines are sorted into their bands. */
#define PD_BANDS_MAX 16
static int nbands = 1;
static uint8_t band_row[PD_H];      /* band of each row, for nbands */
static int band_row_n;
static uint16_t rest[PD_Q_MAX], whole[PD_Q_MAX];
static int nrest, whole_at[PD_BANDS_MAX + 1];

void pd_queue_prepare(int nb) {
	int16_t lo[8], hi[8];           /* rows something later changes, per plane */
	int i, k, b, at[PD_BANDS_MAX];
	nbands = nb < 1 ? 1 : nb > PD_BANDS_MAX ? PD_BANDS_MAX : nb;
	if (band_row_n != nbands) {
		for (i = 0; i < PD_H; i++) band_row[i] = (uint8_t)(i * nbands / PD_H);
		band_row_n = nbands;
	}
	for (k = 0; k < 8; k++) { lo[k] = PD_H; hi[k] = 0; }
	for (b = 0; b <= nbands; b++) whole_at[b] = 0;
	for (i = qn - 1; i >= 0; i--) {
		pd_cmd_t *c = &q[i];
		k = c->k & 7;
		if (c->op == Q_LINE && c->arg == 1) {
			c->whole = (uint8_t)(c->y1 <= lo[k] || c->y0 >= hi[k]);
			if (c->whole) whole_at[band_row[c->y1 - 1] + 1]++;
			continue;
		}
		c->whole = 0;
		if (c->op == Q_OR) continue;
		if (c->y0 < lo[k]) lo[k] = c->y0;
		if (c->y1 > hi[k]) hi[k] = c->y1;
	}
	for (b = 0; b < nbands; b++) { whole_at[b + 1] += whole_at[b]; at[b] = whole_at[b]; }
	nrest = 0;
	for (i = 0; i < qn; i++) {
		if (q[i].whole) whole[at[band_row[q[i].y1 - 1]]++] = (uint16_t)i;
		else rest[nrest++] = (uint16_t)i;
	}
}

int pd_queue_bands(void) { return nbands; }

void pd_queue_band(int b, int *row0) {
	int r0 = b * PD_H / nbands, r1 = (b + 1) * PD_H / nbands, i;
	if (row0) *row0 = r0;
	if (b < 0 || b >= nbands) return;
	for (i = 0; i < nrest; i++) {
		const pd_cmd_t *c = &q[rest[i]];
		if (c->y0 < r1 && c->y1 > r0) run_rows(c, r0, r1);
	}
	for (i = whole_at[b]; i < whole_at[b + 1]; i++) {
		const pd_cmd_t *c = &q[whole[i]];
		ar_plane_line(c->k, c->x, c->y, c->w, c->h, c->arg);
	}
}

void pd_queue_done(void) { qn = 0; }

void pd_queue_flush(void) {
	int b;
	pd_queue_prepare(1);
	for (b = 0; b < pd_queue_bands(); b++) pd_queue_band(b, NULL);
	pd_queue_done();
}

static inline pd_cmd_t *q_add(int op, int k) {
	pd_cmd_t *c;
	if (qn == PD_Q_MAX) pd_queue_flush();      /* too much for one frame: now, in order */
	c = &q[qn++];
	c->op = (uint8_t)op; c->k = (uint8_t)k;
	c->whole = 0;
	return c;
}

static void q_rect(int op, int k, const void *bits, int stride, int sx, int sy, int x, int y, int w, int h, int arg) {
	pd_cmd_t *c;
	if (y < 0) { sy -= y; h += y; y = 0; }
	if (y + h > PD_H) h = PD_H - y;
	if (h <= 0 || w <= 0 || x >= PD_W || x + w <= 0) return;
	c = q_add(op, k);
	c->bits = bits; c->stride = stride; c->arg = (int8_t)arg;
	c->sx = (int16_t)sx; c->sy = (int16_t)sy; c->x = (int16_t)x; c->y = (int16_t)y;
	c->w = (int16_t)w; c->h = (int16_t)h;
	c->y0 = (int16_t)y; c->y1 = (int16_t)(y + h);
}

void pdp_plane(int k, const void *bits, int stride, int sx, int sy, int x, int y, int w, int h) {
	if ((k = PHYS(k)) < 0) return;
	if (queue_on) q_rect(Q_COPY, k, bits, stride, sx, sy, x, y, w, h, 0);
	else ar_plane(k, bits, stride, sx, sy, x, y, w, h);
}
void pdp_or(int k, const void *bits, int stride, int sx, int sy, int x, int y, int w, int h) {
	if ((k = PHYS(k)) < 0) return;
	if (queue_on) q_rect(Q_OR, k, bits, stride, sx, sy, x, y, w, h, 0);
	else ar_plane_or(k, bits, stride, sx, sy, x, y, w, h);
}
void pdp_andn(int k, const void *bits, int stride, int sx, int sy, int x, int y, int w, int h) {
	if ((k = PHYS(k)) < 0) return;
	if (queue_on) q_rect(Q_ANDN, k, bits, stride, sx, sy, x, y, w, h, 0);
	else ar_plane_andn(k, bits, stride, sx, sy, x, y, w, h);
}
void pdp_fill(int k, int x, int y, int w, int h, int bit) {
	if ((k = PHYS(k)) < 0) return;
	if (queue_on) q_rect(Q_FILL, k, NULL, 0, 0, 0, x, y, w, h, bit ? 1 : 0);
	else ar_plane_fill(k, x, y, w, h, bit);
}
void pdp_line(int k, int x0, int y0, int x1, int y1, int op) {
	pd_cmd_t *c;
	int a = y0 < y1 ? y0 : y1, b = (y0 < y1 ? y1 : y0) + 1;
	if ((k = PHYS(k)) < 0) return;
	if (!queue_on) { ar_plane_line(k, x0, y0, x1, y1, op); return; }
	if (a < 0) a = 0;
	if (b > PD_H) b = PD_H;
	if (a >= b) return;
	c = q_add(Q_LINE, k);
	c->arg = (int8_t)op;
	c->x = (int16_t)x0; c->y = (int16_t)y0; c->w = (int16_t)x1; c->h = (int16_t)y1;
	c->y0 = (int16_t)a; c->y1 = (int16_t)b;
}

/* -- canvas -- */

bool pd_canvas_new(pd_planar_t *c, int w, int h, int n) {
	int k, words;
	memset(c, 0, sizeof(*c));
	c->w = w; c->h = h; c->n = n;
	c->stride = ((w + 31) / 32) * 4;
	words = (c->stride / 4) * h + 1;
	for (k = 0; k < n; k++)
		if (!(c->pl[k] = (uint32_t *)calloc((size_t)words, 4))) return false;
	return true;
}

/* (newlib's memset goes a byte at a time) */
static void zero_words(uint32_t *d, int n) {
	while (n >= 4) { d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 0; d += 4; n -= 4; }
	while (n-- > 0) *d++ = 0;
}

void pd_canvas_clear(pd_planar_t *c) {
	int k;
	for (k = 0; k < c->n; k++) if (c->pl[k]) zero_words(c->pl[k], (c->stride / 4) * c->h);
}

void pd_canvas_zero(pd_planar_t *c, int k, int y, int h) {
	if (k >= 0 && k < c->n && c->pl[k] && y >= 0 && h > 0 && y + h <= c->h)
		zero_words(c->pl[k] + y * (c->stride / 4), (c->stride / 4) * h);
}

/* w bits of `src` from bit sx (and of `m`, a mask, or NULL for opaque)
 * into the row `d` at x: d = (d & ~m) | (src & m); or with `op` 1 set,
 * 2 cleared, where src has a bit. A word at a time; a row may be read a
 * word past its end (every picture has a word to spare at its end). */
static void row_put(uint32_t *d, const uint32_t *src, const uint32_t *m, int sx, int x, int w, int op) {
	const uint32_t *sp = src + (sx >> 5), *mp = m ? m + (sx >> 5) : NULL;
	int so = sx & 31, o = x & 31;
	d += x >> 5;
	while (w > 0) {
		int n = 32 - o;
		uint32_t wm, sv;
		if (n > w) n = w;
		wm = (n == 32 ? 0xffffffffu : ((1u << n) - 1u)) << o;
		sv = (so ? (sp[0] >> so) | (sp[1] << (32 - so)) : sp[0]) << o;
		if (op == 1) *d |= sv & wm;
		else if (op == 2) *d &= ~(sv & wm);
		else {
			uint32_t mv = mp ? ((so ? (mp[0] >> so) | (mp[1] << (32 - so)) : mp[0]) << o) & wm : wm;
			*d = (*d & ~mv) | (sv & mv);
		}
		so += n;
		sp += so >> 5;
		if (mp) mp += so >> 5;
		so &= 31;
		d++;
		o = 0;
		w -= n;
	}
}

/* clip a source rectangle to the canvas and to a source w x h */
static bool canvas_clip(const pd_planar_t *c, int pw, int ph, int *sx, int *sy, int *x, int *y, int *w, int *h) {
	if (*x < 0) { *sx -= *x; *w += *x; *x = 0; }
	if (*y < 0) { *sy -= *y; *h += *y; *y = 0; }
	if (*sx < 0) { *x -= *sx; *w += *sx; *sx = 0; }
	if (*sy < 0) { *y -= *sy; *h += *sy; *sy = 0; }
	if (*x + *w > c->w) *w = c->w - *x;
	if (*y + *h > c->h) *h = c->h - *y;
	if (*sx + *w > pw) *w = pw - *sx;
	if (*sy + *h > ph) *h = ph - *sy;
	return *w > 0 && *h > 0;
}

void pd_canvas_put(pd_planar_t *c, int base, const pd_planar_t *p, bool masked,
	int sx, int sy, int x, int y, int w, int h) {
	int k, r, snw = p->stride / 4, dnw = c->stride / 4;
	if (!canvas_clip(c, p->w, p->h, &sx, &sy, &x, &y, &w, &h)) return;
	for (k = 0; k < p->n && base + k < c->n; k++) {
		if (base + k < 0 || !p->pl[k] || !c->pl[base + k]) continue;
		for (r = 0; r < h; r++)
			row_put(c->pl[base + k] + (y + r) * dnw, p->pl[k] + (sy + r) * snw,
				masked && p->mask ? p->mask + (sy + r) * snw : NULL, sx, x, w, 0);
	}
}

void pd_canvas_shape(pd_planar_t *c, const uint32_t *bits, int stride, int base, int n, int value,
	int sx, int sy, int x, int y, int w, int h) {
	int k, r, snw = stride / 4, dnw = c->stride / 4;
	/* (the source is as wide as its stride and as tall as asked) */
	if (!canvas_clip(c, stride * 8, sy + h, &sx, &sy, &x, &y, &w, &h)) return;
	for (k = 0; k < n; k++) {
		if (base + k < 0 || base + k >= c->n || !c->pl[base + k]) continue;
		for (r = 0; r < h; r++)
			row_put(c->pl[base + k] + (y + r) * dnw, bits + (sy + r) * snw, NULL, sx, x, w,
				((value >> k) & 1) ? 1 : 2);
	}
}

void pd_planar_free(pd_planar_t *p) {
	int k;
	for (k = 0; k < 8; k++) free(p->pl[k]);
	free(p->mask);
	memset(p, 0, sizeof(*p));
}

void pd_put(const pd_planar_t *p, int base, int sx, int sy, int x, int y, int w, int h) {
	int k;
	for (k = 0; k < p->n; k++) pdp_plane(base + k, p->pl[k], p->stride, sx, sy, x, y, w, h);
}

void pd_put_masked(const pd_planar_t *p, int base, int sx, int sy, int x, int y, int w, int h) {
	int k;
	if (!p->mask) { pd_put(p, base, sx, sy, x, y, w, h); return; }
	for (k = 0; k < p->n; k++) {
		pdp_andn(base + k, p->mask, p->stride, sx, sy, x, y, w, h);
		pdp_or(base + k, p->pl[k], p->stride, sx, sy, x, y, w, h);
	}
}

void pd_put_wrap(const pd_planar_t *p, int base, int scroll, int sy, int y, int h) {
	int s = scroll % p->w, first;
	if (s < 0) s += p->w;
	first = p->w - s;
	if (first >= PD_W) { pd_put(p, base, s, sy, 0, y, PD_W, h); return; }
	pd_put(p, base, s, sy, 0, y, first, h);
	pd_put(p, base, 0, sy, first, y, PD_W - first, h);
}

void pd_planes_fill(int base, int n, int x, int y, int w, int h, int bit) {
	int k;
	for (k = 0; k < n; k++) pdp_fill(base + k, x, y, w, h, bit);
}

void pd_shape(const uint32_t *bits, int stride, int base, int n, int value,
	int sx, int sy, int x, int y, int w, int h) {
	int k;
	for (k = 0; k < n; k++) {
		if ((value >> k) & 1) pdp_or(base + k, bits, stride, sx, sy, x, y, w, h);
		else pdp_andn(base + k, bits, stride, sx, sy, x, y, w, h);
	}
}

int pd_dither(int x, int y) {
	static const uint8_t b[4][4] = {
		{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
	};
	return b[y & 3][x & 3];
}

/* -- angles -- */

static int16_t sin_q14[4096];

void pd_trig_init(void) {
	/* rotation by 2*pi/4096 in Q30: no libm */
	const int64_t c = 1073740561LL, s = 1647099LL;
	int64_t x = 1LL << 30, y = 0;
	int i;
	for (i = 0; i < 4096; i++) {
		int64_t nx, ny;
		sin_q14[i] = (int16_t)(y >> 16);
		nx = (x * c - y * s) >> 30;
		ny = (x * s + y * c) >> 30;
		x = nx; y = ny;
	}
}

int pd_sin(int a) { return sin_q14[a & 4095]; }
int pd_cos(int a) { return sin_q14[(a + 1024) & 4095]; }

int pd_atan2(int y, int x) {
	/* an octant, then a first-order fit: good to about a degree */
	int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, a;
	if (ax == 0 && ay == 0) return 0;
	/* (32-bit while it fits: a 64-bit divide is a library call on the
	 * board, and the tunnel's map asks this 77,000 times) */
	if (ax >= (1 << 20) || ay >= (1 << 20)) { ax >>= 11; ay >>= 11; if (!ax && !ay) ax = 1; }
	if (ax >= ay) a = ax ? ay * 512 / ax : 0;              /* 0..512 = 0..45 degrees */
	else a = 1024 - ax * 512 / ay;
	/* the fit: atan(t) ~ t*(45 + 15.6*(1-t)) degrees, in 4096ths */
	{
		int t = ax >= ay ? (ax ? ay * 1024 / ax : 0) : ax * 1024 / ay;
		int f = (t * (512 + (178 * (1024 - t) >> 10))) >> 10;
		a = ax >= ay ? f : 1024 - f;
	}
	if (x < 0) a = 2048 - a;
	if (y < 0) a = 4096 - a;
	return a & 4095;
}

uint32_t pd_isqrt(uint32_t v) {
	uint32_t r = 0, b = 1u << 30;
	while (b > v) b >>= 2;
	while (b) {
		if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
		else r >>= 1;
		b >>= 2;
	}
	return r;
}

/* -- time and jobs -- */

uint32_t pd_ticks(void) {
#ifdef PD_HOST
	static uint32_t n;
	return n++;
#else
	return z_uptime_ticks();
#endif
}

#define MAX_JOBS 8
static pd_job_fn jobs[MAX_JOBS];
static int njobs;

void pd_job_add(pd_job_fn fn) {
	if (njobs < MAX_JOBS) jobs[njobs++] = fn;
}

static void drop(int i) {
	memmove(&jobs[i], &jobs[i + 1], sizeof(jobs[0]) * (size_t)(njobs - i - 1));
	njobs--;
}

bool pd_jobs_run(uint32_t ticks) {
	uint32_t t0 = pd_ticks();
	while (njobs) {
		if (jobs[0]()) drop(0);
		if (pd_ticks() - t0 >= ticks) break;
	}
	return njobs == 0;
}

void pd_job_finish(pd_job_fn fn) {
	int i;
	for (i = 0; i < njobs; i++)
		if (jobs[i] == fn) {
			while (!fn()) { }
			drop(i);
			return;
		}
}
