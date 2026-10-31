/*
 * Zeitlos mahjong -- the canvas. See mj_canvas.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Everything below reduces to one operation: set the pixels under a
 * mask word in one row to an index. In colour that is an OR or an AND-
 * NOT per plane; in mono it is the ink's pattern under the mask. A
 * whole tile is a few hundred of them.
 */

#include <string.h>

#include "mj_canvas.h"

int mj_cv_words(int w, int h, int planes)
{
	int stride = (w + 31) >> 5;
	return stride * h * (planes ? planes : 1);
}

/* Ink rows as words, LSB leftmost: pixel x of the canvas is lit where
 * the ink's row has bit (0x80 >> (x & 7)). A word starts at a multiple
 * of 32 -- of 8 -- so the byte simply repeats four times. */
static void ink_words(mj_canvas_t *cv)
{
	int i, r, b;
	for (i = 0; i < 16; i++)
		for (r = 0; r < 8; r++) {
			uint32_t v = 0;
			uint8_t row = cv->ink ? cv->ink[i][r] : (i ? 0xff : 0);
			for (b = 0; b < 8; b++)
				if (row & (0x80u >> b)) v |= 1u << b;
			cv->inkw[i][r] = v | (v << 8) | (v << 16) | (v << 24);
		}
}

bool mj_cv_setup(mj_canvas_t *cv, uint32_t *mem, int mem_words,
		 int w, int h, int planes,
		 const uint8_t (*ink)[8], const uint8_t *ink_map)
{
	int k, n;

	if (w <= 0 || h <= 0 || planes < 0 || planes > MJ_CV_MAXP) return false;
	if (mj_cv_words(w, h, planes) > mem_words) return false;
	memset(cv, 0, sizeof(*cv));
	cv->w = w;
	cv->h = h;
	cv->stride = (w + 31) >> 5;
	cv->planes = planes;
	n = cv->stride * h;
	for (k = 0; k < (planes ? planes : 1); k++) cv->plane[k] = mem + k * n;
	cv->ink = ink;
	cv->ink_map = ink_map;
	if (!planes) ink_words(cv);     /* only mono draws through inks */
	mj_cv_noclip(cv);
	return true;
}

void mj_cv_noclip(mj_canvas_t *cv)
{
	cv->cx0 = 0;
	cv->cy0 = 0;
	cv->cx1 = cv->w;
	cv->cy1 = cv->h;
}

void mj_cv_clip(mj_canvas_t *cv, int x, int y, int w, int h)
{
	cv->cx0 = x < 0 ? 0 : x;
	cv->cy0 = y < 0 ? 0 : y;
	cv->cx1 = x + w > cv->w ? cv->w : x + w;
	cv->cy1 = y + h > cv->h ? cv->h : y + h;
	if (cv->cx1 < cv->cx0) cv->cx1 = cv->cx0;
	if (cv->cy1 < cv->cy0) cv->cy1 = cv->cy0;
}

/* Pixels under m, in word wi of row y, become index c. */
static void put_word(mj_canvas_t *cv, int y, int wi, uint32_t m, int c)
{
	int off = y * cv->stride + wi, k;

	if (!m) return;
	if (cv->planes) {
		for (k = 0; k < cv->planes; k++) {
			uint32_t *p = cv->plane[k] + off;
			if ((c >> k) & 1) *p |= m;
			else *p &= ~m;
		}
	} else {
		int i = cv->ink_map ? cv->ink_map[c & 0xff] : c;
		uint32_t *p = cv->plane[0] + off;
		*p = (*p & ~m) | (m & cv->inkw[i & 15][y & 7]);
	}
}

/* A span [x0, x1) of row y, already clipped. Plane by plane, the
 * whole words in the middle written outright: the felt behind the table
 * is one of these per row. */
static void span(mj_canvas_t *cv, int y, int x0, int x1, int c)
{
	int w0 = x0 >> 5, w1 = (x1 - 1) >> 5, wi, k, np = cv->planes ? cv->planes : 1;
	int pn = cv->stride * cv->h;
	uint32_t first = ~0u << (x0 & 31);
	uint32_t last = ((x1 & 31) ? (1u << (x1 & 31)) : 0u) - 1u;
	uint32_t *row = cv->plane[0] + y * cv->stride;

	if (w0 == w1) {
		put_word(cv, y, w0, first & last, c);
		return;
	}
	put_word(cv, y, w0, first, c);
	put_word(cv, y, w1, last, c);
	if (w1 - w0 < 2) return;
	for (k = 0; k < np; k++, row += pn) {
		uint32_t v;
		if (cv->planes) v = ((c >> k) & 1) ? ~0u : 0;
		else {
			int i = cv->ink_map ? cv->ink_map[c & 0xff] : c;
			v = cv->inkw[i & 15][y & 7];
		}
		for (wi = w0 + 1; wi < w1; wi++) row[wi] = v;
	}
}

void mj_cv_fill(mj_canvas_t *cv, int x, int y, int w, int h, int c)
{
	int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
	if (x0 < cv->cx0) x0 = cv->cx0;
	if (y0 < cv->cy0) y0 = cv->cy0;
	if (x1 > cv->cx1) x1 = cv->cx1;
	if (y1 > cv->cy1) y1 = cv->cy1;
	if (x1 <= x0 || y1 <= y0) return;
	for (y = y0; y < y1; y++) span(cv, y, x0, x1, c);
}

void mj_cv_row(mj_canvas_t *cv, int x, int y, uint32_t bits, int n, int c)
{
	int sh;

	if (y < cv->cy0 || y >= cv->cy1 || n <= 0 || !bits) return;
	if (n < 32) bits &= (1u << n) - 1u;
	if (x < cv->cx0) {
		int d = cv->cx0 - x;
		if (d >= n) return;
		bits >>= d;
		n -= d;
		x = cv->cx0;
	}
	if (x + n > cv->cx1) {
		n = cv->cx1 - x;
		if (n <= 0) return;
		if (n < 32) bits &= (1u << n) - 1u;
	}
	if (!bits) return;
	sh = x & 31;
	put_word(cv, y, x >> 5, bits << sh, c);
	if (sh && (bits >> (32 - sh)))
		put_word(cv, y, (x >> 5) + 1, bits >> (32 - sh), c);
}

void mj_cv_bits(mj_canvas_t *cv, const uint32_t *rows, int w, int h,
		int x, int y, int c)
{
	int r;
	for (r = 0; r < h; r++) mj_cv_row(cv, x, y + r, rows[r], w, c);
}

int mj_cv_get(const mj_canvas_t *cv, int x, int y)
{
	int k, v = 0, off;
	if (x < 0 || y < 0 || x >= cv->w || y >= cv->h) return 0;
	off = y * cv->stride + (x >> 5);
	if (!cv->planes) return (int)((cv->plane[0][off] >> (x & 31)) & 1u);
	for (k = 0; k < cv->planes; k++)
		v |= (int)((cv->plane[k][off] >> (x & 31)) & 1u) << k;
	return v;
}

void mj_cv_shade(mj_canvas_t *cv, int x, int y, int w, int h, int bit)
{
	int x0 = x, y0 = y, x1 = x + w, y1 = y + h, yy, wi, w0, w1;
	uint32_t first, last;

	if (x0 < cv->cx0) x0 = cv->cx0;
	if (y0 < cv->cy0) y0 = cv->cy0;
	if (x1 > cv->cx1) x1 = cv->cx1;
	if (y1 > cv->cy1) y1 = cv->cy1;
	if (x1 <= x0 || y1 <= y0) return;
	if (cv->planes && (bit < 0 || bit >= cv->planes)) return;

	w0 = x0 >> 5;
	w1 = (x1 - 1) >> 5;
	first = ~0u << (x0 & 31);
	last = (x1 & 31) ? (1u << (x1 & 31)) - 1u : ~0u;
	for (yy = y0; yy < y1; yy++) {
		uint32_t *row = cv->plane[cv->planes ? bit : 0] + yy * cv->stride;
		/* mono: put out every other lit pixel */
		uint32_t chk = (yy & 1) ? 0xaaaaaaaau : 0x55555555u;
		for (wi = w0; wi <= w1; wi++) {
			uint32_t m = ~0u;
			if (wi == w0) m &= first;
			if (wi == w1) m &= last;
			if (cv->planes) row[wi] |= m;
			else row[wi] &= ~(m & chk);
		}
	}
}

/* The planes are laid out one after another in the canvas's memory
 * (mj_cv_setup()), so plane k of a word is the plane-0 word plus k
 * planes: one pointer and a stride, not a table look-up per plane.
 * This and the two functions after it are where a whole table's time
 * goes, at seven million instructions a second on the board. */

void mj_cv_stamp(mj_canvas_t *cv, const uint32_t *src, int sstride, int sw, int sh,
		 int x, int y)
{
	int np = cv->planes ? cv->planes : 1, r, r0, r1, j, k;
	int pw = sstride * sh, pn = cv->stride * cv->h;
	const uint32_t *mask = src + np * pw;
	uint32_t cm[4];
	int wi[4], s5[4];

	if (sstride > 4) sstride = 4;
	r0 = cv->cy0 - y > 0 ? cv->cy0 - y : 0;
	r1 = cv->cy1 - y < sh ? cv->cy1 - y : sh;
	if (r1 <= r0) return;

	/* each source word's clip, shift and destination word, once */
	for (j = 0; j < sstride; j++) {
		int x0 = x + 32 * j, lo = cv->cx0 - x0, hi = cv->cx1 - x0, wd = sw - 32 * j;
		uint32_t m = ~0u;
		if (lo > 0) m &= lo >= 32 ? 0 : ~0u << lo;
		if (hi < 32) m &= hi <= 0 ? 0 : (1u << hi) - 1u;
		if (wd <= 0) m = 0;
		else if (wd < 32) m &= (1u << wd) - 1u;
		cm[j] = m;
		s5[j] = x0 & 31;
		wi[j] = x0 >> 5;    /* floor: x0 may be negative, and then cm clears it */
	}

	for (r = r0; r < r1; r++) {
		uint32_t *row = cv->plane[0] + (y + r) * cv->stride;
		for (j = 0; j < sstride; j++) {
			uint32_t m = mask[r * sstride + j] & cm[j];
			const uint32_t *sp = src + r * sstride + j;
			int sh5 = s5[j], w0 = wi[j];
			if (!m) continue;
			if (!sh5) {
				uint32_t *d = row + w0;
				for (k = 0; k < np; k++, d += pn)
					*d = (*d & ~m) | (sp[k * pw] & m);
			} else {
				uint32_t lo = m << sh5, hi = m >> (32 - sh5);
				uint32_t *d = row + w0;
				bool ok0 = lo && w0 >= 0, ok1 = hi && w0 + 1 < cv->stride;
				for (k = 0; k < np; k++, d += pn) {
					uint32_t s = sp[k * pw];
					if (ok0) d[0] = (d[0] & ~lo) | ((s << sh5) & lo);
					if (ok1) d[1] = (d[1] & ~hi) | ((s >> (32 - sh5)) & hi);
				}
			}
		}
	}
}

void mj_cv_row_multi(mj_canvas_t *cv, int x, int y, const uint32_t *bits,
		     const uint8_t *c, int nc, int n)
{
	uint32_t u = 0, v[MJ_CV_MAXP], cut = n < 32 ? (1u << n) - 1u : ~0u;
	int i, k, sh, wi, np = cv->planes, pn = cv->stride * cv->h;
	uint32_t *row;

	if (y < cv->cy0 || y >= cv->cy1 || n <= 0 || nc <= 0) return;
	for (i = 0; i < nc; i++) u |= bits[i];
	u &= cut;
	/* the clip, in the row's own pixels */
	if (x < cv->cx0) {
		int d = cv->cx0 - x;
		if (d >= 32) return;
		u &= ~0u << d;
	}
	if (x + 32 > cv->cx1) {
		int d = cv->cx1 - x;
		if (d <= 0) return;
		if (d < 32) u &= (1u << d) - 1u;
	}
	if (!u) return;

	/* what each plane gets: each colour's pixels into the planes of its
	 * set bits (glyph colours have one or two) */
	if (np) {
		for (k = 0; k < np; k++) v[k] = 0;
		for (i = 0; i < nc; i++) {
			uint32_t b = bits[i] & u;
			int ci = c[i];
			for (k = 0; ci && k < np; k++, ci >>= 1)
				if (ci & 1) v[k] |= b;
		}
	} else {
		v[0] = 0;
		for (i = 0; i < nc; i++) {
			int ci = cv->ink_map ? cv->ink_map[c[i]] : c[i];
			/* an ink row repeats every 8 pixels, and 32 is a multiple
			 * of 8, so the shift to x is a rotation of the same word */
			uint32_t ink = cv->inkw[ci & 15][y & 7];
			int r = x & 7;
			if (r) ink = (ink >> r) | (ink << (32 - r));
			v[0] |= bits[i] & u & ink;
		}
		np = 1;
	}

	sh = x & 31;
	wi = x >> 5;
	row = cv->plane[0] + y * cv->stride + wi;
	{
		uint32_t lo = u << sh, hi = sh ? u >> (32 - sh) : 0;
		bool ok0 = lo && wi >= 0 && wi < cv->stride;
		bool ok1 = hi && wi + 1 >= 0 && wi + 1 < cv->stride;
		uint32_t *d = row;
		for (k = 0; k < np; k++, d += pn) {
			if (ok0) d[0] = (d[0] & ~lo) | (v[k] << sh);
			if (ok1) d[1] = (d[1] & ~hi) | (v[k] >> (32 - sh));
		}
	}
}

/* One row of `n` words moved by dx pixels: positive to the right.
 * Less than a word -- every step the pointer or the wheel takes -- is a
 * funnel shift of each word with its neighbour, a few instructions a
 * word; a whole row of six planes of the screen is under 70 words. */
static void shift_row(uint32_t *w, int n, int dx)
{
	int i, q, r;
	if (dx > 0) {
		q = dx >> 5;
		r = dx & 31;
		if (!q && r) {
			for (i = n - 1; i > 0; i--) w[i] = (w[i] << r) | (w[i - 1] >> (32 - r));
			w[0] <<= r;
			return;
		}
		for (i = n - 1; i >= 0; i--) {
			uint32_t a = i - q >= 0 ? w[i - q] : 0;
			uint32_t b = i - q - 1 >= 0 ? w[i - q - 1] : 0;
			w[i] = r ? (a << r) | (b >> (32 - r)) : a;
		}
	} else if (dx < 0) {
		dx = -dx;
		q = dx >> 5;
		r = dx & 31;
		if (!q && r) {
			for (i = 0; i < n - 1; i++) w[i] = (w[i] >> r) | (w[i + 1] << (32 - r));
			w[n - 1] >>= r;
			return;
		}
		for (i = 0; i < n; i++) {
			uint32_t a = i + q < n ? w[i + q] : 0;
			uint32_t b = i + q + 1 < n ? w[i + q + 1] : 0;
			w[i] = r ? (a >> r) | (b << (32 - r)) : a;
		}
	}
}

void mj_cv_scroll(mj_canvas_t *cv, int y0, int y1, int dx, int dy)
{
	int k, y, np = cv->planes ? cv->planes : 1;
	if (y0 < 0) y0 = 0;
	if (y1 > cv->h) y1 = cv->h;
	if (y1 <= y0 || (!dx && !dy)) return;
	if (dy >= y1 - y0 || -dy >= y1 - y0) return;    /* nothing survives */

	for (k = 0; k < np; k++) {
		uint32_t *base = cv->plane[k];
		int st = cv->stride;
		/* rows first, in the order that does not overwrite a source */
		if (dy > 0)
			for (y = y1 - 1; y - dy >= y0; y--)
				memcpy(base + y * st, base + (y - dy) * st, (size_t)st * 4);
		else if (dy < 0)
			for (y = y0; y - dy < y1; y++)
				memcpy(base + y * st, base + (y - dy) * st, (size_t)st * 4);
		if (dx)
			for (y = y0; y < y1; y++) shift_row(base + y * st, st, dx);
	}
	/* mono's inks are anchored to the canvas: a dither moved by an odd
	 * amount is out of phase with one drawn in place. Callers scroll
	 * mono by multiples of 8 (mj_play.c), so it never is. */
}

/* Font rows are MSB leftmost (zfont.h); the canvas is LSB leftmost. */
static uint32_t flip8(uint8_t b, int w)
{
	uint32_t v = 0;
	int i;
	for (i = 0; i < w && i < 8; i++)
		if (b & (0x80u >> i)) v |= 1u << i;
	return v;
}

int mj_cv_text_width(const char *s, const z_font_t *f)
{
	int n = 0;
	while (s && s[n]) n++;
	return n * f->w;
}

int mj_cv_text(mj_canvas_t *cv, int x, int y, const char *s, int c,
	       const z_font_t *f)
{
	int x0 = x, r;
	for (; s && *s; s++, x += f->w) {
		int gi = z_font_index(f, (uint8_t)*s);
		const uint8_t *g;
		if (gi < 0) gi = z_font_index(f, '?');
		if (gi < 0) continue;
		g = f->glyphs + gi * f->h;
		for (r = 0; r < f->h; r++)
			if (g[r]) mj_cv_row(cv, x, y + r, flip8(g[r], f->w), f->w, c);
	}
	return x - x0;
}
