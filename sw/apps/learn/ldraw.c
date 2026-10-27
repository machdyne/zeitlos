/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- drawing, through a device. ldraw.h.
 *
 * Round shapes are spans: a row's extent worked out here, a filled
 * rectangle drawn by the device, and rows with the same extent joined
 * into one rectangle. Pixels are inside a shape when their centres are.
 * Text is glyphs drawn once into a cache and copied, a line at a time,
 * through a strip.
 */

#include <string.h>
#include <math.h>

#include "ldraw.h"
#include "zfont.h"
#include "zutf8.h"

ld_fonts_t LDF;

#define I(v) ((zc_fix)((v) * ZC_ONE))

static void open_or(zc_font_t *f, ld_font_loader load, const char *file, const zc_font_t *fallback) {
	uint32_t len = 0;
	const void *d = load ? load(file, &len) : NULL;
	char name[24];
	int n = 0;
	while (file[n] && file[n] != '.' && n < 23) { name[n] = file[n]; n++; }
	name[n] = 0;
	if (d && zc_font_open(f, d, len, name)) return;
	*f = *fallback;
}

void ld_fonts_load(ld_font_loader load) {
	zc_font_builtin(&LDF.f5, &z_font_5x8, 7, "mono5x8");
	zc_font_builtin(&LDF.f6, &z_font_6x12, 10, "mono6x12");
	open_or(&LDF.body, load, "m9x15.zfn", &LDF.f6);
	open_or(&LDF.body_b, load, "m9x15b.zfn", &LDF.body);
	open_or(&LDF.small, load, "m7x13.zfn", &LDF.f6);
	open_or(&LDF.small_b, load, "m7x13b.zfn", &LDF.small);
	open_or(&LDF.big, load, "m10x20.zfn", &LDF.body);
	LDF.big_b = LDF.big;
	open_or(&LDF.mono, load, "m6x13.zfn", &LDF.f6);
	open_or(&LDF.serif, load, "serif.zhf", &LDF.big);
	open_or(&LDF.serif_b, load, "serifb.zhf", &LDF.serif);
	open_or(&LDF.serif_i, load, "serifi.zhf", &LDF.serif);
	open_or(&LDF.sans_b, load, "sansb.zhf", &LDF.serif_b);
	uint32_t len = 0;
	const void *d = load ? load("jp12.zfn", &len) : NULL;
	LDF.have_jp = d && zc_font_open(&LDF.jp, d, len, "jp12");
	if (!LDF.have_jp) LDF.jp = LDF.body;
}

// -- the software device: the hardware's operations, in memory --

// the blitter's 4x4 ordered dither (rtl/gpu/gpu_blit.v): lit where the
// threshold is below the level
static const uint8_t bayer[4][4] = {
	{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

// one word of a shade or a pattern for row y (pixel i of the word at bit i)
static uint32_t ink_word(const ld_ink_t *k, int y) {
	uint32_t w = 0;
	if (k->kind == LD_K_SHADE) {
		const uint8_t *b = bayer[y & 3];
		for (int i = 0; i < 32; i++) if (b[i & 3] < k->level) w |= 1u << i;
	} else {
		uint8_t b = k->pat[y & 7];
		for (int i = 0; i < 32; i++) if ((b >> (7 - (i & 7))) & 1) w |= 1u << i;
	}
	return w;
}

// the eight row words of the last shade or pattern used (a polygon's
// rows each ask for the same one)
static ld_ink_t pw_ink;
static uint32_t pw_rows[8];
static bool pw_ok;

static const uint32_t *ink_rows(const ld_ink_t *k) {
	if (!pw_ok || memcmp(&pw_ink, k, sizeof *k)) {
		for (int r = 0; r < 8; r++) pw_rows[r] = ink_word(k, r);
		pw_ink = *k;
		pw_ok = true;
	}
	return pw_rows;
}

static void soft_fill(ld_page_t *p, int x, int y, int w, int h, const ld_ink_t *k) {
	int w0 = x >> 5, w1 = (x + w - 1) >> 5;
	uint32_t m0 = ~0u << (x & 31), m1 = ~0u >> (31 - ((x + w - 1) & 31));
	const uint32_t *pr = k->kind >= LD_K_SHADE ? ink_rows(k) : NULL;
	for (int r = 0; r < h; r++) {
		uint32_t *row = p->bits + (size_t)(y + r) * (size_t)p->wpl;
		uint32_t pw = pr ? pr[(y + r) & 7] : 0;
		if (w0 == w1) {
			uint32_t m = m0 & m1;
			switch (k->kind) {
			case LD_K_DARK: row[w0] &= ~m; break;
			case LD_K_INK: row[w0] |= m; break;
			case LD_K_XOR: row[w0] ^= m; break;
			default: row[w0] = (row[w0] & ~m) | (pw & m); break;
			}
			continue;
		}
		switch (k->kind) {
		case LD_K_DARK:
			row[w0] &= ~m0;
			for (int i = w0 + 1; i < w1; i++) row[i] = 0;
			row[w1] &= ~m1;
			break;
		case LD_K_INK:
			row[w0] |= m0;
			for (int i = w0 + 1; i < w1; i++) row[i] = ~0u;
			row[w1] |= m1;
			break;
		case LD_K_XOR:
			row[w0] ^= m0;
			for (int i = w0 + 1; i < w1; i++) row[i] = ~row[i];
			row[w1] ^= m1;
			break;
		default:
			row[w0] = (row[w0] & ~m0) | (pw & m0);
			for (int i = w0 + 1; i < w1; i++) row[i] = pw;
			row[w1] = (row[w1] & ~m1) | (pw & m1);
			break;
		}
	}
}

static inline void plot(ld_page_t *p, int x, int y, int c, const ld_clip_t *cl) {
	if (x < cl->x0 || x >= cl->x1 || y < cl->y0 || y >= cl->y1) return;
	uint32_t *w = p->bits + (size_t)y * (size_t)p->wpl + (x >> 5), b = 1u << (x & 31);
	if (c == LD_INK) *w |= b;
	else if (c == LD_XOR) *w ^= b;
	else *w &= ~b;
}

static void soft_line(ld_page_t *p, int x0, int y0, int x1, int y1, int c, const ld_clip_t *cl) {
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
	int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (;;) {
		plot(p, x0, y0, c, cl);
		if (x0 == x1 && y0 == y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; x0 += sx; }
		if (e2 <= dx) { err += dx; y0 += sy; }
	}
}

// 32 bits of a row from bit `at` (bits outside the row are 0)
static inline uint32_t bits32(const uint32_t *row, int wpl, int at) {
	int wi = at >> 5, s = at & 31;
	if (at < 0) {
		// (only from a negative source x, never asked for)
		return 0;
	}
	uint32_t lo = wi < wpl ? row[wi] : 0;
	if (!s) return lo;
	uint32_t hi = wi + 1 < wpl ? row[wi + 1] : 0;
	return (lo >> s) | (hi << (32 - s));
}

// a rectangle of one bitmap onto another
static void copy_bits(uint32_t *dst, int dwpl, int dx, int dy, const uint32_t *src, int swpl,
	int sx, int sy, int w, int h, int rop) {
	int w0 = dx >> 5, w1 = (dx + w - 1) >> 5;
	uint32_t m0 = ~0u << (dx & 31), m1 = ~0u >> (31 - ((dx + w - 1) & 31));
	for (int j = 0; j < h; j++) {
		uint32_t *drow = dst + (size_t)(dy + j) * (size_t)dwpl;
		const uint32_t *srow = src + (size_t)(sy + j) * (size_t)swpl;
		for (int i = w0; i <= w1; i++) {
			int first = i * 32;
			uint32_t m = ~0u;
			if (i == w0) m &= m0;
			if (i == w1) m &= m1;
			int at = sx + (first - dx);
			uint32_t v = at >= 0 ? bits32(srow, swpl, at) : (bits32(srow, swpl, 0) << (-at));
			switch (rop) {
			case LD_ROP_OR: drow[i] |= v & m; break;
			case LD_ROP_XOR: drow[i] ^= v & m; break;
			case LD_ROP_ANDN: drow[i] &= ~(v & m); break;
			default: drow[i] = (drow[i] & ~m) | (v & m); break;
			}
		}
	}
}

static void soft_blit(ld_page_t *p, const uint32_t *bits, int wpl, int sx, int sy, int x, int y, int w, int h, int rop) {
	copy_bits(p->bits, p->wpl, x, y, bits, wpl, sx, sy, w, h, rop);
}

static bool soft_scroll(ld_page_t *p, int x, int y, int w, int h, int dy) {
	if (dy < 0) {
		for (int j = 0; j < h + dy; j++)
			copy_bits(p->bits, p->wpl, x, y + j, p->bits, p->wpl, x, y + j - dy, w, 1, LD_ROP_COPY);
	} else {
		for (int j = h - 1; j >= dy; j--)
			copy_bits(p->bits, p->wpl, x, y + j, p->bits, p->wpl, x, y + j - dy, w, 1, LD_ROP_COPY);
	}
	return true;
}

static bool soft_save(ld_page_t *p, int x, int y, int w, int h, uint32_t *bits, int wpl) {
	copy_bits(bits, wpl, 0, 0, p->bits, p->wpl, x, y, w, h, LD_ROP_COPY);
	return true;
}

const ld_dev_t ld_soft = { soft_fill, soft_line, soft_blit, soft_scroll, soft_save };

void ld_page(ld_page_t *p, uint32_t *bits, int w, int h) {
	memset(p, 0, sizeof *p);
	p->dev = &ld_soft;
	p->bits = bits;
	p->wpl = (w + 31) / 32;
	p->w = w;
	p->h = h;
	ld_clip_reset(p);
}

void ld_page_dev(ld_page_t *p, const ld_dev_t *dev, void *ctx, int w, int h) {
	memset(p, 0, sizeof *p);
	p->dev = dev;
	p->ctx = ctx;
	p->w = w;
	p->h = h;
	ld_clip_reset(p);
}

// -- working memory --

typedef struct glyph glyph_t;
static void gl_flush(void);

// the glyph cache's bitmaps: given by ld_init(), this until then
static uint32_t gl_own[8192];
static uint32_t *gl_mem = gl_own;
static uint32_t gl_cap = 8192, gl_used;	// words

void ld_init(void *scratch, uint32_t scratch_bytes, void *cache, uint32_t cache_bytes) {
	zc_scratch(scratch, scratch_bytes);		// (zcanvas: drawing a glyph the first time)
	if (cache && cache_bytes >= 4096) {
		gl_mem = (uint32_t *)cache;
		gl_cap = cache_bytes / 4;
	}
	gl_flush();
}

// -- the clip --

void ld_clip_reset(ld_page_t *p) {
	p->cx0 = 0;
	p->cy0 = 0;
	p->cx1 = p->w;
	p->cy1 = p->h;
}

ld_clip_t ld_clip_push(ld_page_t *p, int x0, int y0, int x1, int y1) {
	ld_clip_t was = { p->cx0, p->cy0, p->cx1, p->cy1 };
	if (x0 > p->cx0) p->cx0 = x0;
	if (y0 > p->cy0) p->cy0 = y0;
	if (x1 < p->cx1) p->cx1 = x1;
	if (y1 < p->cy1) p->cy1 = y1;
	return was;
}

void ld_clip_pop(ld_page_t *p, ld_clip_t was) {
	p->cx0 = was.x0;
	p->cy0 = was.y0;
	p->cx1 = was.x1;
	p->cy1 = was.y1;
}

static bool clipr(const ld_page_t *p, int *x, int *y, int *w, int *h) {
	int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
	if (x0 < p->cx0) x0 = p->cx0;
	if (y0 < p->cy0) y0 = p->cy0;
	if (x1 > p->cx1) x1 = p->cx1;
	if (y1 > p->cy1) y1 = p->cy1;
	if (x1 <= x0 || y1 <= y0) return false;
	*x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
	return true;
}

// -- rectangles --

ld_ink_t ld_ink(int c) {
	ld_ink_t k;
	memset(&k, 0, sizeof k);
	k.kind = c == LD_INK ? LD_K_INK : c == LD_XOR ? LD_K_XOR : LD_K_DARK;
	return k;
}

void ld_paint(ld_page_t *p, int x, int y, int w, int h, const ld_ink_t *k) {
	if (w <= 0 || h <= 0 || !clipr(p, &x, &y, &w, &h)) return;
	p->dev->fill(p, x, y, w, h, k);
}

void ld_clear(ld_page_t *p) {
	ld_ink_t k = ld_ink(LD_DARK);
	ld_paint(p, p->cx0, p->cy0, p->cx1 - p->cx0, p->cy1 - p->cy0, &k);
}

void ld_fill(ld_page_t *p, int x, int y, int w, int h, int c) {
	ld_ink_t k = ld_ink(c);
	ld_paint(p, x, y, w, h, &k);
}

void ld_shade(ld_page_t *p, int x, int y, int w, int h, int tone) {
	ld_ink_t k;
	memset(&k, 0, sizeof k);
	k.kind = LD_K_SHADE;
	k.level = (uint8_t)(tone < 0 ? 0 : tone > 16 ? 16 : tone);
	ld_paint(p, x, y, w, h, &k);
}

void ld_hline(ld_page_t *p, int x0, int x1, int y, int c) { ld_fill(p, x0, y, x1 - x0 + 1, 1, c); }
void ld_vline(ld_page_t *p, int x, int y0, int y1, int c) { ld_fill(p, x, y0, 1, y1 - y0 + 1, c); }

void ld_frame(ld_page_t *p, int x, int y, int w, int h, int lw, int c) {
	ld_fill(p, x, y, w, lw, c);
	ld_fill(p, x, y + h - lw, w, lw, c);
	ld_fill(p, x, y + lw, lw, h - 2 * lw, c);
	ld_fill(p, x + w - lw, y + lw, lw, h - 2 * lw, c);
}

void ld_blit(ld_page_t *p, const uint32_t *bits, int wpl, int sx, int sy, int x, int y, int w, int h, int rop) {
	int nx = x, ny = y, nw = w, nh = h;
	if (w <= 0 || h <= 0 || !clipr(p, &nx, &ny, &nw, &nh)) return;
	p->dev->blit(p, bits, wpl, sx + (nx - x), sy + (ny - y), nx, ny, nw, nh, rop);
}

bool ld_scroll(ld_page_t *p, int x, int y, int w, int h, int dy) {
	if (!p->dev->scroll || dy == 0 || (dy < 0 ? -dy : dy) >= h) return false;
	return p->dev->scroll(p, x, y, w, h, dy);
}

bool ld_save(ld_page_t *p, int x, int y, int w, int h, uint32_t *bits, int wpl) {
	if (!p->dev->save || x < 0 || y < 0 || x + w > p->w || y + h > p->h) return false;
	return p->dev->save(p, x, y, w, h, bits, wpl);
}

// -- spans --

// Rows with the same spans (at most two) become one rectangle each.
typedef struct {
	ld_page_t		*p;
	const ld_ink_t	*k;
	int				y0, y1;		// rows [y0, y1) pending
	int				n;			// spans pending
	int				xs[4];
} spans_t;

static void sp_flush(spans_t *s) {
	for (int i = 0; i < s->n; i++)
		ld_paint(s->p, s->xs[2 * i], s->y0, s->xs[2 * i + 1] - s->xs[2 * i], s->y1 - s->y0, s->k);
	s->n = 0;
}

static void sp_row(spans_t *s, int y, const int *xs, int n) {
	if (n == s->n && y == s->y1 && !memcmp(xs, s->xs, (size_t)n * 2 * sizeof(int))) { s->y1++; return; }
	sp_flush(s);
	if (n > 2) {	// (only a polygon: drawn as they come)
		for (int i = 0; i < n; i++) ld_paint(s->p, xs[2 * i], y, xs[2 * i + 1] - xs[2 * i], 1, s->k);
		return;
	}
	s->n = n;
	memcpy(s->xs, xs, (size_t)n * 2 * sizeof(int));
	s->y0 = y;
	s->y1 = y + 1;
}

static int isqrt(int v) {
	if (v <= 0) return 0;
	int r = (int)sqrtf((float)v);
	while (r * r > v) r--;
	while ((r + 1) * (r + 1) <= v) r++;
	return r;
}

// a rounded rectangle's row j: how far in from each side
static int rr_in(int r, int j, int h) {
	if (r <= 0) return 0;
	int d2;
	if (j < r) d2 = 2 * (r - j) - 1;
	else if (j >= h - r) d2 = 2 * (j - (h - r)) + 1;
	else return 0;
	int s = isqrt(4 * r * r - d2 * d2);
	return r - (s + 1) / 2;
}

static int rr_r(int r, int w, int h) {
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	return r < 0 ? 0 : r;
}

void ld_rrect(ld_page_t *p, int x, int y, int w, int h, int r, int c) {
	if (w <= 0 || h <= 0 || !ld_clip_hits(p, x, y, w, h)) return;
	ld_ink_t k = ld_ink(c);
	r = rr_r(r, w, h);
	spans_t s = { p, &k, 0, 0, 0, { 0 } };
	for (int j = 0; j < h; j++) {
		int in = rr_in(r, j, h);
		int xs[2] = { x + in, x + w - in };
		sp_row(&s, y + j, xs, 1);
	}
	sp_flush(&s);
}

void ld_rframe(ld_page_t *p, int x, int y, int w, int h, int r, int lw, int c) {
	if (w <= 0 || h <= 0 || !ld_clip_hits(p, x, y, w, h)) return;
	if (2 * lw >= w || 2 * lw >= h) { ld_rrect(p, x, y, w, h, r, c); return; }
	ld_ink_t k = ld_ink(c);
	r = rr_r(r, w, h);
	int iw = w - 2 * lw, ih = h - 2 * lw, ir = rr_r(r - lw, iw, ih);
	spans_t s = { p, &k, 0, 0, 0, { 0 } };
	for (int j = 0; j < h; j++) {
		int in = rr_in(r, j, h);
		if (j < lw || j >= h - lw) {
			int xs[2] = { x + in, x + w - in };
			sp_row(&s, y + j, xs, 1);
		} else {
			int ii = rr_in(ir, j - lw, ih);
			int xs[4] = { x + in, x + lw + ii, x + w - lw - ii, x + w - in };
			if (xs[1] <= xs[0]) xs[1] = xs[0] + 1;
			if (xs[2] >= xs[3]) xs[2] = xs[3] - 1;
			sp_row(&s, y + j, xs, 2);
		}
	}
	sp_flush(&s);
}

// rows of a figure clipped to the page's clip
static void rows_of(const ld_page_t *p, float top, float bottom, int *y0, int *y1) {
	int a = (int)floorf(top), b = (int)ceilf(bottom) + 1;
	if (a < p->cy0) a = p->cy0;
	if (b > p->cy1) b = p->cy1;
	*y0 = a;
	*y1 = b;
}

// Round shapes in sixteenths of a pixel, integers from there on: a
// square root a row, by shifts (the CPU has no floating point).
static uint32_t isqrt32(uint32_t v) {
	uint32_t r = 0, b = 1u << 30;
	while (b > v) b >>= 2;
	while (b) {
		if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
		else r >>= 1;
		b >>= 2;
	}
	return r;
}

// the columns whose centres are in [l, r] (sixteenths)
static inline int c16_l(int l) { return (l + 7) >> 4; }
static inline int c16_r(int r) { return ((r - 8) >> 4) + 1; }

void ld_k_disc(ld_page_t *p, float cx, float cy, float r, const ld_ink_t *k) {
	if (r <= 0 || r > 2000) return;
	int y0, y1;
	rows_of(p, cy - r, cy + r, &y0, &y1);
	if (cx + r < p->cx0 || cx - r > p->cx1) return;
	int X = (int)(cx * 16), Y = (int)(cy * 16), R = (int)(r * 16);
	uint32_t R2 = (uint32_t)R * (uint32_t)R;
	spans_t s = { p, k, 0, 0, 0, { 0 } };
	for (int y = y0; y < y1; y++) {
		int dy = y * 16 + 8 - Y;
		uint32_t ady = (uint32_t)(dy < 0 ? -dy : dy), d2 = ady * ady;
		if (d2 >= R2) { sp_flush(&s); continue; }
		int hw = (int)isqrt32(R2 - d2);
		int xs[2] = { c16_l(X - hw), c16_r(X + hw) };
		if (xs[1] <= xs[0]) { sp_flush(&s); continue; }
		sp_row(&s, y, xs, 1);
	}
	sp_flush(&s);
}

void ld_k_line(ld_page_t *p, float x0, float y0, float x1, float y1, float w, const ld_ink_t *k) {
	if (w <= 1.25f && k->kind <= LD_K_XOR) {
		int c = k->kind == LD_K_INK ? LD_INK : k->kind == LD_K_XOR ? LD_XOR : LD_DARK;
		int a = (int)floorf(x0 + 0.5f), b = (int)floorf(y0 + 0.5f), e = (int)floorf(x1 + 0.5f), f = (int)floorf(y1 + 0.5f);
		int lx = a < e ? a : e, hx = a < e ? e : a, ly = b < f ? b : f, hy = b < f ? f : b;
		if (hx < p->cx0 || lx >= p->cx1 || hy < p->cy0 || ly >= p->cy1) return;
		if (a == e || b == f) { ld_fill(p, lx, ly, hx - lx + 1, hy - ly + 1, c); return; }
		ld_clip_t cl = { p->cx0, p->cy0, p->cx1, p->cy1 };
		p->dev->line(p, a, b, e, f, c, &cl);
		return;
	}
	if (w < 1) w = 1;
	float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
	if (len < 0.001f) { ld_k_disc(p, x0, y0, w / 2, k); return; }
	if (w <= 6 && (k->kind == LD_K_INK || k->kind == LD_K_DARK)) {
		// a few one-pixel lines side by side, a pixel apart across the
		// line's run (the rasterizer draws them; no spans to work out)
		float adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
		int n = (int)(w * (adx > ady ? adx : ady) / len + 0.5f);
		if (n < 2) n = 2;
		float off = (n - 1) * 0.5f;
		for (int i = 0; i < n; i++) {
			float d = i - off;
			if (adx > ady) ld_k_line(p, x0, y0 + d, x1, y1 + d, 1, k);
			else ld_k_line(p, x0 + d, y0, x1 + d, y1, 1, k);
		}
		return;
	}
	float nx = -dy / len * w / 2, ny = dx / len * w / 2;
	float q[8] = { x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny };
	ld_k_poly(p, q, 4, k);
}

void ld_k_polyline(ld_page_t *p, const float *xy, int n, float w, bool closed, const ld_ink_t *k) {
	for (int i = 1; i < n; i++) ld_k_line(p, xy[2 * i - 2], xy[2 * i - 1], xy[2 * i], xy[2 * i + 1], w, k);
	if (closed && n > 2) ld_k_line(p, xy[2 * n - 2], xy[2 * n - 1], xy[0], xy[1], w, k);
	if (w >= 2.5f)		// round joins: no notches where the segments meet
		for (int i = closed ? 0 : 1; i < (closed ? n : n - 1); i++) ld_k_disc(p, xy[2 * i], xy[2 * i + 1], w / 2, k);
}

void ld_k_ring(ld_page_t *p, float cx, float cy, float r, float w, const ld_ink_t *k) {
	if (r <= 0) return;
	float ro = r + w / 2, ri = r - w / 2;
	if (cx + ro < p->cx0 || cx - ro > p->cx1 || cy + ro < p->cy0 || cy - ro > p->cy1) return;
	if (w <= 1.25f && k->kind <= LD_K_XOR) {
		// thin: lines round it (one-pixel spans would leave gaps)
		int n = (int)(r * 0.75f) + 12;
		if (n > 120) n = 120;
		float pts[240], c = cosf(6.2831853f / n), sn = sinf(6.2831853f / n), vx = r, vy = 0;
		for (int i = 0; i < n; i++) {
			pts[2 * i] = cx + vx;
			pts[2 * i + 1] = cy + vy;
			float t = vx * c - vy * sn;
			vy = vx * sn + vy * c;
			vx = t;
		}
		ld_k_polyline(p, pts, n, 1, true, k);
		return;
	}
	if (ri <= 0) { ld_k_disc(p, cx, cy, ro, k); return; }
	if (ro > 2000) return;
	int y0, y1;
	rows_of(p, cy - ro, cy + ro, &y0, &y1);
	int X = (int)(cx * 16), Y = (int)(cy * 16), RO = (int)(ro * 16), RI = (int)(ri * 16);
	uint32_t RO2 = (uint32_t)RO * (uint32_t)RO, RI2 = (uint32_t)RI * (uint32_t)RI;
	spans_t s = { p, k, 0, 0, 0, { 0 } };
	for (int y = y0; y < y1; y++) {
		int dy = y * 16 + 8 - Y;
		uint32_t ady = (uint32_t)(dy < 0 ? -dy : dy), d2 = ady * ady;
		if (d2 >= RO2) { sp_flush(&s); continue; }
		int ho = (int)isqrt32(RO2 - d2);
		int l = c16_l(X - ho), r_ = c16_r(X + ho);
		if (d2 >= RI2) {
			int xs[2] = { l, r_ };
			if (xs[1] > xs[0]) sp_row(&s, y, xs, 1);
			continue;
		}
		int hi = (int)isqrt32(RI2 - d2);
		int xs[4] = { l, c16_l(X - hi), c16_r(X + hi), r_ };
		if (xs[1] <= xs[0]) xs[1] = xs[0] + 1;
		if (xs[2] >= xs[3]) xs[2] = xs[3] - 1;
		sp_row(&s, y, xs, 2);
	}
	sp_flush(&s);
}

// a polygon, nonzero winding: 16.16 edges, crossings a row at a time
#define POLY_MAX 512
static int32_t pe_x[POLY_MAX], pe_y[POLY_MAX];
static int64_t pe_s[POLY_MAX];	// 16.16; the slope, x a row

void ld_k_poly(ld_page_t *p, const float *xy, int n, const ld_ink_t *k) {
	if (n < 3) return;
	if (n > POLY_MAX) n = POLY_MAX;
	float top = xy[1], bot = xy[1], lef = xy[0], rig = xy[0];
	for (int i = 0; i < n; i++) {
		float x = xy[2 * i], y = xy[2 * i + 1];
		if (y < top) top = y;
		if (y > bot) bot = y;
		if (x < lef) lef = x;
		if (x > rig) rig = x;
		pe_x[i] = (int32_t)(x * 65536.0f);
		pe_y[i] = (int32_t)(y * 65536.0f);
	}
	for (int i = 0; i < n; i++) {
		int j = i + 1 == n ? 0 : i + 1;
		int32_t dy = pe_y[j] - pe_y[i];
		pe_s[i] = dy ? (((int64_t)(pe_x[j] - pe_x[i]) << 16) / dy) : 0;
	}
	if (rig < p->cx0 || lef > p->cx1) return;
	int y0, y1;
	rows_of(p, top, bot, &y0, &y1);
	spans_t s = { p, k, 0, 0, 0, { 0 } };
	int32_t cx[64];
	int8_t cd[64];
	for (int y = y0; y < y1; y++) {
		int32_t yc = (int32_t)(y << 16) + 0x8000;
		int nc = 0;
		for (int i = 0; i < n && nc < 64; i++) {
			int j = i + 1 == n ? 0 : i + 1;
			int32_t ya = pe_y[i], yb = pe_y[j];
			int d;
			if (ya <= yc && yc < yb) d = 1;
			else if (yb <= yc && yc < ya) d = -1;
			else continue;
			int32_t x = pe_x[i] + (int32_t)(((int64_t)(yc - ya) * pe_s[i]) >> 16);
			// in order of x
			int at = nc++;
			while (at > 0 && cx[at - 1] > x) { cx[at] = cx[at - 1]; cd[at] = cd[at - 1]; at--; }
			cx[at] = x;
			cd[at] = (int8_t)d;
		}
		int xs[64], ns = 0, wind = 0;
		for (int i = 0; i < nc; i++) {
			int was = wind;
			wind += cd[i];
			if (!was && wind) xs[2 * ns] = (int)((cx[i] - 0x8000 + 0xFFFF) >> 16);
			else if (was && !wind && ns < 31) {
				xs[2 * ns + 1] = (int)((cx[i] - 0x8000 + 0xFFFF) >> 16);
				if (xs[2 * ns + 1] > xs[2 * ns]) ns++;
			}
		}
		if (!ns) { sp_flush(&s); continue; }
		sp_row(&s, y, xs, ns);
	}
	sp_flush(&s);
}

void ld_line(ld_page_t *p, double x0, double y0, double x1, double y1, double w, int c) {
	ld_ink_t k = ld_ink(c);
	ld_k_line(p, (float)x0, (float)y0, (float)x1, (float)y1, (float)w, &k);
}

void ld_disc(ld_page_t *p, double cx, double cy, double r, int c) {
	ld_ink_t k = ld_ink(c);
	ld_k_disc(p, (float)cx, (float)cy, (float)r, &k);
}

void ld_ring(ld_page_t *p, double cx, double cy, double r, double w, int c) {
	ld_ink_t k = ld_ink(c);
	ld_k_ring(p, (float)cx, (float)cy, (float)r, (float)w, &k);
}

void ld_poly(ld_page_t *p, const double *xy, int n, int c) {
	float f[64];
	if (n > 32) n = 32;
	for (int i = 0; i < 2 * n; i++) f[i] = (float)xy[i];
	ld_ink_t k = ld_ink(c);
	ld_k_poly(p, f, n, &k);
}

void ld_arc(ld_page_t *p, double cx, double cy, double r, int a0, int a1, double w, int c) {
	float pts[2 * 34];
	int n = 0, steps = (a1 - a0) / 32 + 2;
	if (steps > 33) steps = 33;
	for (int i = 0; i <= steps; i++) {
		float a = (float)(a0 + (a1 - a0) * i / steps) * 6.2831853f / 1024.0f;
		pts[2 * n] = (float)cx + (float)r * cosf(a);
		pts[2 * n + 1] = (float)cy + (float)r * sinf(a);
		n++;
	}
	ld_ink_t k = ld_ink(c);
	ld_k_polyline(p, pts, n, (float)w, false, &k);
}

// -- text --

ld_style_t ld_st(const zc_font_t *f, int size) {
	ld_style_t s;
	memset(&s, 0, sizeof s);
	s.font = f;
	s.size = size;
	return s;
}

static int cell_h(const zc_font_t *f) {
	return f->kind == ZC_FONT_BUILTIN ? f->builtin->h : f->h;
}

static zc_style_t zst(const ld_style_t *s) {
	zc_style_t z;
	memset(&z, 0, sizeof z);
	z.font = s->font;
	if (s->font->kind == ZC_FONT_STROKE) z.size = I(s->size ? s->size : 16);
	else z.size = I(s->size ? s->size : cell_h(s->font));
	z.bold = s->bold;
	z.italic = s->italic;
	return z;
}

// The glyph cache: each glyph of a face, size and style drawn once (by
// zcanvas, as it would draw it on a page) and kept as rows of words.
struct glyph {
	const zc_font_t	*f;
	uint32_t		cp;
	uint16_t		size;
	uint8_t			flags;		// 1 used, 2 bold, 4 italic
	int8_t			bx, by;		// its bitmap's top left, from the pen on the baseline
	uint8_t			w, h, wpr;	// pixels; words a row
	uint32_t		off;		// in gl_mem
};

#define GL_SLOTS 1024
static glyph_t gl[GL_SLOTS];
static int gl_n;
static uint32_t gl_drawn, gl_found;

static void gl_flush(void) {
	memset(gl, 0, sizeof gl);
	gl_n = 0;
	gl_used = 0;
}

void ld_glyph_stats(uint32_t *drawn, uint32_t *found) {
	if (drawn) *drawn = gl_drawn;
	if (found) *found = gl_found;
}

// where a glyph is drawn the first time: the pen at (GX, GY)
#define GT_W 128
#define GT_H 112
#define GT_WPL (GT_W / 32)
#define GX 32
#define GY 84
static uint32_t gt[GT_WPL * GT_H + 2];

static const glyph_t *glyph(const ld_style_t *st, const zc_style_t *z, uint32_t cp) {
	uint8_t flags = (uint8_t)(1 | (st->bold ? 2 : 0) | (st->italic ? 4 : 0));
	uint16_t size = (uint16_t)(z->size >> ZC_FRAC);
	uint32_t h = (uint32_t)(uintptr_t)st->font * 2654435761u ^ cp * 40503u ^ size * 977u ^ flags;
	int i = (int)((h ^ (h >> 15)) & (GL_SLOTS - 1));
	for (;;) {
		glyph_t *g = &gl[i];
		if (!g->flags) break;
		if (g->cp == cp && g->f == st->font && g->size == size && g->flags == flags) { gl_found++; return g; }
		i = (i + 1) & (GL_SLOTS - 1);
	}
	// not there: drawn now (emptied first when it is full)
	if (gl_n >= GL_SLOTS * 3 / 4 || gl_used + GT_WPL * GT_H > gl_cap) {
		gl_flush();
		return glyph(st, z, cp);
	}
	gl_drawn++;
	glyph_t *g = &gl[i];
	g->f = st->font;
	g->cp = cp;
	g->size = size;
	g->flags = flags;
	gl_n++;
	// a bitmap face at its own size: its rows as they are
	const zc_font_t *f = st->font;
	if ((f->kind == ZC_FONT_BITMAP || f->kind == ZC_FONT_BUILTIN) && !st->bold && !st->italic &&
		size == cell_h(f) && f->w <= 32) {
		uint32_t rc = zc_font_resolve(f, cp);
		const uint8_t *rows = NULL;
		int stride = 1;
		if (rc && f->kind == ZC_FONT_BITMAP) { rows = zc_font_rows(f, rc); stride = f->stride; }
		else if (rc && f->kind == ZC_FONT_BUILTIN && rc < 256) {
			int gi = z_font_index(f->builtin, rc);
			if (gi >= 0) rows = f->builtin->glyphs + (size_t)gi * f->builtin->h;
		}
		if (rows) {
			int gw = f->kind == ZC_FONT_BUILTIN ? f->builtin->w : f->w;
			g->bx = 0;
			g->by = (int8_t)-f->asc;
			g->w = (uint8_t)gw;
			g->h = (uint8_t)size;
			g->wpr = 1;
			g->off = gl_used;
			for (int y = 0; y < size; y++) {
				uint32_t v = 0;
				for (int x = 0; x < gw; x++)
					if ((rows[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1) v |= 1u << x;
				gl_mem[gl_used++] = v;
			}
			return g;
		}
	}
	// anything else as zcanvas draws it on a page, into rows around the pen
	int em = size ? size : 16, ry0 = GY - em * 3 / 2 - 4, ry1 = GY + em * 3 / 4 + 4;
	if (ry0 < 0) ry0 = 0;
	if (ry1 > GT_H) ry1 = GT_H;
	memset(gt + ry0 * GT_WPL, 0, (size_t)(ry1 - ry0) * GT_WPL * 4);
	zc_target_t t;
	zc_target_bits(&t, gt, GT_WPL, GT_W, GT_H);
	zc_xf_t xf = zc_xf_identity();
	zc_paint_t k = zc_white();
	char u[8];
	int n = z_utf8_put(cp, u);
	zc_clip(&t, 0, ry0, GT_W, ry1);
	zc_text_draw(&t, &xf, z, I(GX), I(GY), u, (uint32_t)n, &k);
	// its ink's box
	int x0 = GT_W, x1 = -1, y0 = GT_H, y1 = -1;
	for (int y = ry0; y < ry1; y++) {
		const uint32_t *row = gt + y * GT_WPL;
		for (int wi = 0; wi < GT_WPL; wi++) {
			uint32_t v = row[wi];
			if (!v) continue;
			if (y < y0) y0 = y;
			y1 = y;
			int lo = wi * 32 + __builtin_ctz(v), hi = wi * 32 + 31 - __builtin_clz(v);
			if (lo < x0) x0 = lo;
			if (hi > x1) x1 = hi;
		}
	}
	if (x1 < 0) { g->w = g->h = 0; g->wpr = 0; g->off = 0; return g; }
	g->bx = (int8_t)(x0 - GX);
	g->by = (int8_t)(y0 - GY);
	g->w = (uint8_t)(x1 - x0 + 1);
	g->h = (uint8_t)(y1 - y0 + 1);
	g->wpr = (uint8_t)((g->w + 31) / 32);
	g->off = gl_used;
	for (int y = 0; y < g->h; y++)
		for (int k2 = 0; k2 < g->wpr; k2++)
			gl_mem[gl_used++] = bits32(gt + (y0 + y) * GT_WPL, GT_WPL, x0 + k2 * 32);
	return g;
}

// A line of text is composed here and copied in one blit: strips of
// ST_W pixels (a longer line is more than one), rows from ST_TOP above
// the baseline. Two, used in turn, so the board's copy of one can run
// while the next is composed.
#define ST_W 672
#define ST_WPL (ST_W / 32)
#define ST_H 112
#define ST_TOP 84
static uint32_t strip[2][ST_WPL * ST_H + 2];	// (+2: the blitter may read a word past the end)
static int strip_i;

typedef struct {
	ld_page_t	*p;
	uint32_t	*s;
	int			si;			// which strip
	int			x0;			// the page's x of the strip's bit 0
	int			base;
	int			r0, r1;		// rows used
	int			c0, c1;		// columns used
	int			rop;
} comp_t;

// what each strip has on it: cleaned when it is next used, not after its
// copy -- on the board the copy may still be running when ld_text returns
static int st_used[2][4];		// rows [0, 1), words [2, 3]

static void comp_start(comp_t *c, int x0) {
	int *u = st_used[strip_i];
	c->s = strip[strip_i];
	for (int r = u[0]; r < u[1]; r++) {
		uint32_t *row = c->s + r * ST_WPL;
		for (int i = u[2]; i <= u[3]; i++) row[i] = 0;
	}
	u[0] = u[1] = 0;
	c->si = strip_i;
	strip_i ^= 1;
	c->x0 = x0;
	c->r0 = ST_H;
	c->r1 = 0;
	c->c0 = ST_W;
	c->c1 = 0;
}

static void comp_end(comp_t *c) {
	if (c->r1 <= c->r0 || c->c1 <= c->c0) return;
	int *u = st_used[c->si];
	u[0] = c->r0;
	u[1] = c->r1;
	u[2] = c->c0 >> 5;
	u[3] = (c->c1 - 1) >> 5;
	if (u[3] + 1 < ST_WPL) u[3]++;
	ld_blit(c->p, c->s, ST_WPL, c->c0, c->r0, c->x0 + c->c0, c->base - ST_TOP + c->r0,
		c->c1 - c->c0, c->r1 - c->r0, c->rop);
}

static void comp_glyph(comp_t *c, const glyph_t *g, int gx) {
	int cx = gx + g->bx - c->x0, r = ST_TOP + g->by;
	if (cx < 0 || cx + g->w > ST_W - 32 || r < 0 || r + g->h > ST_H) return;
	const uint32_t *src = gl_mem + g->off;
	for (int y = 0; y < g->h; y++) {
		uint32_t *row = c->s + (r + y) * ST_WPL;
		for (int k = 0; k < g->wpr; k++) {
			uint32_t v = *src++;
			if (!v) continue;
			int at = cx + k * 32, wi = at >> 5, sh = at & 31;
			row[wi] |= v << sh;
			if (sh) row[wi + 1] |= v >> (32 - sh);
		}
	}
	if (r < c->r0) c->r0 = r;
	if (r + g->h > c->r1) c->r1 = r + g->h;
	if (cx < c->c0) c->c0 = cx;
	if (cx + g->w > c->c1) c->c1 = cx + g->w;
}

// rows [y0, y1) are outside what is being drawn (a part of the page)
static bool rows_off(const ld_page_t *p, int y0, int y1) { return y1 <= p->cy0 || y0 >= p->cy1; }

int ld_text(ld_page_t *p, const ld_style_t *st, int x, int base, const char *s, int len, int c) {
	if (len <= 0) return x;
	// a line of text outside the clip: only its width is wanted
	int em = st->size ? st->size : cell_h(st->font);
	if (rows_off(p, base - em - 8, base + em / 2 + 8)) return x + ld_width(st, s, len);
	zc_style_t z = zst(st);
	comp_t cm;
	cm.p = p;
	cm.base = base;
	cm.rop = c == LD_INK ? LD_ROP_OR : c == LD_XOR ? LD_ROP_XOR : LD_ROP_ANDN;
	comp_start(&cm, x - 32);
	zc_fix pen = I(x);
	const char *q = s, *e = s + len;
	while (q < e) {
		uint32_t cp = z_utf8_next(&q, e);
		zc_fix adv = zc_text_advance(&z, cp);
		int gx = ZC_TOINT(pen);
		if (cp != ' ' && cp != '\n' && cp != '\r' && gx + 64 > p->cx0 && gx - 64 < p->cx1) {
			const glyph_t *g = glyph(st, &z, cp);
			if (g && g->w) {
				if (gx + g->bx + g->w - cm.x0 > ST_W - 32) { comp_end(&cm); comp_start(&cm, gx - 32); }
				comp_glyph(&cm, g, gx);
			}
		}
		pen += adv;
	}
	comp_end(&cm);
	int end = ZC_TOINT(pen);
	if (st->underline && end > x) {
		int asc, desc;
		ld_metrics(st, &asc, &desc, NULL);
		ld_fill(p, x, base + (desc > 2 ? desc / 2 : 1), end - x, 1, c);
	}
	return end;
}

int ld_textz(ld_page_t *p, const ld_style_t *st, int x, int base, const char *s, int c) {
	return ld_text(p, st, x, base, s, (int)strlen(s), c);
}

int ld_width(const ld_style_t *st, const char *s, int len) {
	const zc_font_t *f = st->font;
	// a fixed-width bitmap face at its own size: characters times the cell
	if ((f->kind == ZC_FONT_BITMAP || f->kind == ZC_FONT_BUILTIN) && !st->bold &&
		(!st->size || st->size == cell_h(f))) {
		int adv = f->kind == ZC_FONT_BUILTIN ? f->builtin->w : f->w, n = 0;
		for (int i = 0; i < len; i++) {
			unsigned char ch = (unsigned char)s[i];
			if ((ch & 0xC0) != 0x80 && ch != '\n' && ch != '\r') n++;
		}
		return n * adv;
	}
	zc_style_t z = zst(st);
	return ZC_TOINT(zc_text_width(&z, s, (uint32_t)len) + ZC_ONE - 1);
}

int ld_widthz(const ld_style_t *st, const char *s) { return ld_width(st, s, (int)strlen(s)); }

void ld_metrics(const ld_style_t *st, int *asc, int *desc, int *line) {
	zc_style_t z = zst(st);
	zc_fix a, d, l;
	zc_text_metrics(&z, &a, &d, &l);
	if (asc) *asc = ZC_TOINT(a + ZC_ONE - 1);
	if (desc) *desc = ZC_TOINT(d + ZC_ONE - 1);
	if (line) *line = ZC_TOINT(l + ZC_ONE - 1);
}

int ld_fitz(ld_page_t *p, const ld_style_t *st, int x, int base, int w, const char *s, int c) {
	int len = (int)strlen(s);
	if (ld_width(st, s, len) <= w) return ld_text(p, st, x, base, s, len, c);
	int dots = ld_widthz(st, "...");
	while (len > 0 && ld_width(st, s, len) + dots > w) {
		len--;
		while (len > 0 && ((unsigned char)s[len] & 0xC0) == 0x80) len--;	// whole characters
	}
	x = ld_text(p, st, x, base, s, len, c);
	return ld_textz(p, st, x, base, "...", c);
}

int ld_text_c(ld_page_t *p, const ld_style_t *st, int cx, int base, const char *s, int c) {
	return ld_textz(p, st, cx - ld_widthz(st, s) / 2, base, s, c);
}

int ld_text_r(ld_page_t *p, const ld_style_t *st, int rx, int base, const char *s, int c) {
	return ld_textz(p, st, rx - ld_widthz(st, s), base, s, c);
}

int ld_wrap(ld_page_t *p, const ld_style_t *st, int x, int base, int w, int lead,
	const char *s, int c) {
	zc_style_t z = zst(st);
	zc_line_t lines[48];
	int n = zc_text_wrap(&z, s, (uint32_t)strlen(s), I(w), lines, 48, NULL);
	for (int i = 0; i < n; i++)
		ld_text(p, st, x, base + i * lead, s + lines[i].start, (int)lines[i].len, c);
	return base + n * lead;
}

int ld_wrap_lines(const ld_style_t *st, int w, const char *s) {
	zc_style_t z = zst(st);
	zc_line_t lines[48];
	return zc_text_wrap(&z, s, (uint32_t)strlen(s), I(w), lines, 48, NULL);
}

// -- learn's pieces --

int ld_keycap(ld_page_t *p, int x, int base, const char *k, int c) {
	ld_style_t s = ld_st(&LDF.f5, 0);
	int w = ld_widthz(&s, k) + 5;
	if (rows_off(p, base - 10, base + 6)) return x + w;
	ld_rframe(p, x, base - 8, w, 11, 2, 1, c);
	ld_textz(p, &s, x + 3, base, k, c);
	return x + w;
}

int ld_keys(ld_page_t *p, int x, int base, const char *spec, int c) {
	ld_style_t s = ld_st(&LDF.f6, 0);
	const char *q = spec;
	while (*q) {
		const char *bar = strchr(q, '|');
		if (!bar) break;
		const char *end = strchr(bar, ';');
		if (!end) end = bar + strlen(bar);
		char key[16];
		int kl = (int)(bar - q) < 15 ? (int)(bar - q) : 15;
		memcpy(key, q, (size_t)kl);
		key[kl] = 0;
		x = ld_keycap(p, x, base, key, c) + 3;
		x = ld_text(p, &s, x, base + 1, bar + 1, (int)(end - bar - 1), c) + 14;
		q = *end ? end + 1 : end;
	}
	return x;
}

void ld_button(ld_page_t *p, int x, int y, int w, int h, const char *label, bool filled, bool focus) {
	if (rows_off(p, y - 4, y + h + 4)) return;
	ld_style_t s = ld_st(filled ? &LDF.small_b : &LDF.small, 0);
	int asc, desc;
	ld_metrics(&s, &asc, &desc, NULL);
	int base = y + (h + asc - desc) / 2;
	if (filled) {
		ld_rrect(p, x, y, w, h, 4, LD_INK);
		ld_text_c(p, &s, x + w / 2, base, label, LD_DARK);
	} else {
		ld_rframe(p, x, y, w, h, 4, 1, LD_INK);
		ld_text_c(p, &s, x + w / 2, base, label, LD_INK);
	}
	if (focus) ld_rframe(p, x - 3, y - 3, w + 6, h + 6, 6, 1, LD_INK);
}

void ld_bar(ld_page_t *p, int x, int y, int w, int h, int pct) {
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	ld_frame(p, x, y, w, h, 1, LD_INK);
	ld_fill(p, x + 2, y + 2, (w - 4) * pct / 100, h - 4, LD_INK);
}

void ld_mastery(ld_page_t *p, int x, int y, int level) {
	float cx = x + 5.5f, cy = y + 5.5f;
	ld_ink_t k = ld_ink(LD_INK);
	if (level >= 4) { ld_k_disc(p, cx, cy, 5.5f, &k); return; }
	ld_k_ring(p, cx, cy, 5.0f, 1.5f, &k);
	for (int q = 0; q < level; q++) {
		float xy[22];
		int n = 0;
		xy[n++] = cx; xy[n++] = cy;
		for (int i = 0; i <= 8; i++) {
			float a = (-0.25f + q * 0.25f + i * 0.25f / 8) * 6.2831853f;
			xy[n++] = cx + 5.5f * cosf(a);
			xy[n++] = cy + 5.5f * sinf(a);
		}
		ld_k_poly(p, xy, n / 2, &k);
	}
}

void ld_speaker(ld_page_t *p, int x, int y, int c) {
	double body[] = { x, y + 4, x + 4, y + 4, x + 9, y, x + 9, y + 13, x + 4, y + 9, x, y + 9 };
	ld_poly(p, body, 6, c);
	ld_arc(p, x + 9, y + 6.5, 4, -128, 128, 1, c);
	ld_arc(p, x + 9, y + 6.5, 7.5, -128, 128, 1, c);
}

void ld_tick(ld_page_t *p, int x, int y, int c) {
	ld_line(p, x + 1, y + 6, x + 4, y + 9, 2.2, c);
	ld_line(p, x + 4, y + 9, x + 10, y + 1, 2.2, c);
}

void ld_cross(ld_page_t *p, int x, int y, int c) {
	ld_line(p, x + 1, y + 1, x + 9, y + 9, 2.2, c);
	ld_line(p, x + 9, y + 1, x + 1, y + 9, 2.2, c);
}
