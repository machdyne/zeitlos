/*
 * pix -- pictures from elsewhere. See px_import.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "px_import.h"
#include "../../common/zimg.h"

#if !Z_IMG_HAVE_RGB
#error "pix needs zimg.c's colour rows: build it with -DZ_IMG_HAVE_RGB=1"
#endif

static void fail(char *err, int errlen, const char *msg) {
	if (err && errlen > 0) {
		strncpy(err, msg, (size_t)errlen - 1);
		err[errlen - 1] = 0;
	}
}

/* -- sources -- */

static int mem_read(void *ctx, uint8_t *buf, int len) {
	px_mem_t *m = ctx;
	uint32_t n = m->len - m->pos;
	if ((uint32_t)len < n) n = (uint32_t)len;
	memcpy(buf, m->data + m->pos, n);
	m->pos += n;
	return (int)n;
}

static bool mem_rewind(void *ctx) {
	((px_mem_t *)ctx)->pos = 0;
	return true;
}

void px_src_mem(px_src_t *s, px_mem_t *m, const uint8_t *data, uint32_t len) {
	m->data = data;
	m->len = len;
	m->pos = 0;
	s->read = mem_read;
	s->rewind = mem_rewind;
	s->ctx = m;
}

bool px_import_sniff(const uint8_t *hdr, int n) {
	z_img_fmt_t f = z_img_sniff(hdr, n);
	return f != Z_IMG_FMT_NONE;
}

/* -- one pass over the image -- */

typedef struct {
	/* pass 1: how many pixels of each RGB444 colour (opaque ones) */
	uint32_t *hist;
	/* pass 2: where they go */
	const uint8_t *lut;             /* RGB444 -> index */
	px_doc_t *doc;                  /* a picture: its one cell */
	px_piece_t *piece;              /* or a piece */
	int ink_level;                  /* two colours: grey at or past this is ink */
} pass_t;

static inline int rgb444(const uint8_t *q) {
	return ((q[0] >> 4) << 8) | ((q[1] >> 4) << 4) | (q[2] >> 4);
}

static void row_cb(void *user, const uint8_t *rgba, int y, int w) {
	pass_t *p = user;
	int x;
	for (x = 0; x < w; x++) {
		const uint8_t *q = rgba + x * 4;
		bool opaque = q[3] >= 128;
		int c = rgb444(q);
		if (p->hist) {
			if (opaque) p->hist[c]++;
			continue;
		}
		if (p->doc) {
			px_set(p->doc, 0, x, y, p->lut[c]);
		} else if (p->piece && x < p->piece->w && y < p->piece->h) {
			uint16_t v;
			if (!opaque) v = ZSP_T;
			else if (p->lut) v = p->lut[c];
			else v = (uint16_t)(zsp_lum((uint16_t)c) >= p->ink_level);
			p->piece->px[y * p->piece->w + x] = v;
		}
	}
}

/* One decode of the whole image, rows to row_cb. */
static int decode(px_src_t *src, pass_t *ps, int *w, int *h) {
	z_img_t im;
	uint8_t hdr[16];
	z_img_fmt_t fmt;
	int n, rv;
	if (!src->rewind(src->ctx)) return Z_IMG_E_IO;
	n = src->read(src->ctx, hdr, sizeof(hdr));
	fmt = z_img_sniff(hdr, n);
	if (fmt == Z_IMG_FMT_NONE) return Z_IMG_E_FORMAT;
	if (!src->rewind(src->ctx)) return Z_IMG_E_IO;
	memset(&im, 0, sizeof(im));
	im.read = src->read;
	im.ctx = src->ctx;
	im.doc_w = Z_IMG_MAX_W;
	im.doc_h = Z_IMG_MAX_H;
	rv = z_img_decode_rgba(&im, fmt, row_cb, ps);
	if (w) *w = im.out_w;
	if (h) *h = im.out_h;
	if (rv == Z_IMG_OK && (im.out_w <= 0 || im.out_h <= 0)) rv = Z_IMG_E_FORMAT;
	return rv;
}

/* -- choosing colours -- */

static int dist(uint16_t a, uint16_t b) {
	int dr = (int)((a >> 8) & 15) - (int)((b >> 8) & 15);
	int dg = (int)((a >> 4) & 15) - (int)((b >> 4) & 15);
	int db = (int)(a & 15) - (int)(b & 15);
	/* green counts most, blue least, as the eye has it */
	return dr * dr * 3 + dg * dg * 4 + db * db * 2;
}

/* For every RGB444 colour, the nearest of the n entries. */
static void make_lut(uint8_t *lut, const zsp_palette_t *p, int n) {
	int c, i;
	for (c = 0; c < 4096; c++) {
		int best = 0, bd = 1 << 30;
		for (i = 0; i < n; i++) {
			int d = dist((uint16_t)c, p->pal[i].rgb);
			if (d < bd) { bd = d; best = i; if (!d) break; }
		}
		lut[c] = (uint8_t)best;
	}
}

/* Median cut over the histogram: boxes of RGB444 space, the one with
 * the most pixels times its longest side split at its median, until
 * there are `want`; each box's colour is its pixels' mean. */
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

static int median_cut(const uint32_t *hist, uint16_t *out, int want) {
	static box_t box[256];
	int nb = 1, i;
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
							int c3[3] = { r, g, bl };
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

/* The order of a palette matters: its first 16 are the 16-colour
 * form, its first 4 the slots' start. So the colours go in spread out
 * first: the most used, then each time the one farthest from those
 * already in (by count when as far), the rest by how much they are
 * used. */
static void order(uint16_t *c, uint32_t *cnt, int n) {
	int i, j, k;
	for (i = 0; i < n; i++) {
		int best = i;
		if (i == 0 || i >= 16) {
			for (j = i + 1; j < n; j++) if (cnt[j] > cnt[best]) best = j;
		} else {
			int bd = -1;
			for (j = i; j < n; j++) {
				int d = 1 << 30;
				for (k = 0; k < i; k++) { int e = dist(c[j], c[k]); if (e < d) d = e; }
				if (d > bd || (d == bd && cnt[j] > cnt[best])) { bd = d; best = j; }
			}
		}
		if (best != i) {
			uint16_t t = c[i]; uint32_t u = cnt[i];
			c[i] = c[best]; cnt[i] = cnt[best];
			c[best] = t; cnt[best] = u;
		}
	}
}

bool px_import_picture(px_doc_t *d, px_src_t *src, int *colours_seen, char *err, int errlen) {
	pass_t ps;
	uint32_t *hist = calloc(4096, sizeof(uint32_t));
	uint8_t *lut = malloc(4096);
	uint16_t col[256];
	uint32_t cnt[256];
	int w = 0, h = 0, rv, n = 0, c, tier, i;

	if (!hist || !lut) { free(hist); free(lut); fail(err, errlen, "out of memory"); return false; }
	memset(&ps, 0, sizeof(ps));
	ps.hist = hist;
	rv = decode(src, &ps, &w, &h);
	if (rv != Z_IMG_OK) { free(hist); free(lut); fail(err, errlen, z_img_strerror(rv)); return false; }
	if ((uint32_t)w * (uint32_t)h > PX_MAX_COLOUR_PX) { free(hist); free(lut); fail(err, errlen, "too big"); return false; }

	for (c = 0; c < 4096; c++) if (hist[c]) n++;
	if (colours_seen) *colours_seen = n;
	if (n == 0) { hist[0] = 1; n = 1; }       /* all see-through: black */
	if (n <= 256) {
		int k = 0;
		for (c = 0; c < 4096; c++) if (hist[c]) { col[k] = (uint16_t)c; cnt[k] = hist[c]; k++; }
	} else {
		n = median_cut(hist, col, 256);
		for (i = 0; i < n; i++) cnt[i] = 0;
		/* how much each is used: by the colours nearest it */
		{
			zsp_palette_t tmp;
			zsp_palette_init(&tmp, 256);
			for (i = 0; i < n; i++) tmp.pal[i].rgb = col[i];
			make_lut(lut, &tmp, n);
			for (c = 0; c < 4096; c++) cnt[lut[c]] += hist[c];
		}
	}
	order(col, cnt, n);
	tier = n <= 4 ? 4 : n <= 16 ? 16 : n <= 64 ? 64 : 256;

	px_doc_free(d);
	px_doc_init(d, PX_ZBM);
	d->depth = 8;
	zsp_palette_init(&d->pal, tier);
	for (i = 0; i < n; i++) {
		d->pal.pal[i].rgb = col[i];
		d->pal.pal[i].given = 0;
	}
	/* the rest: what a new palette has there, unless it repeats */
	zsp_defaults_all(&d->pal);
	make_lut(lut, &d->pal, n);

	if (px_cell_insert(d, 0, w, h, "picture") < 0) {
		free(hist); free(lut);
		px_doc_free(d);
		px_doc_init(d, PX_ZBM);
		fail(err, errlen, "out of memory");
		return false;
	}
	memset(&ps, 0, sizeof(ps));
	ps.lut = lut;
	ps.doc = d;
	rv = decode(src, &ps, NULL, NULL);
	free(hist);
	free(lut);
	if (rv != Z_IMG_OK) { fail(err, errlen, z_img_strerror(rv)); return false; }
	d->dirty = true;
	return true;
}

bool px_import_piece(const px_doc_t *d, px_src_t *src, px_piece_t *p, char *err, int errlen) {
	pass_t ps;
	uint8_t *lut = NULL;
	int w = 0, h = 0, rv;
	uint32_t *hist = calloc(4096, sizeof(uint32_t));

	if (!hist) { fail(err, errlen, "out of memory"); return false; }
	/* the size first (and that it is one) */
	memset(&ps, 0, sizeof(ps));
	ps.hist = hist;
	rv = decode(src, &ps, &w, &h);
	free(hist);
	if (rv != Z_IMG_OK) { fail(err, errlen, z_img_strerror(rv)); return false; }

	px_piece_free(p);
	p->px = malloc((size_t)w * (size_t)h * sizeof(uint16_t));
	if (!p->px) { fail(err, errlen, "out of memory"); return false; }
	p->w = w;
	p->h = h;
	{
		int k;
		for (k = 0; k < w * h; k++) p->px[k] = ZSP_T;
	}
	memset(&ps, 0, sizeof(ps));
	ps.piece = p;
	ps.ink_level = 8;
	if (px_colour(d)) {
		lut = malloc(4096);
		if (!lut) { px_piece_free(p); fail(err, errlen, "out of memory"); return false; }
		make_lut(lut, &d->pal, d->pal.colours);
		ps.lut = lut;
	}
	rv = decode(src, &ps, NULL, NULL);
	free(lut);
	if (rv != Z_IMG_OK) { px_piece_free(p); fail(err, errlen, z_img_strerror(rv)); return false; }
	return true;
}
