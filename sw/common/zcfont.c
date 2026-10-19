/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zcfont -- see zcfont.h and docs/fonts.md.
 */

#include "zcfont.h"
#include "zutf8.h"

#include <string.h>

static inline uint16_t rd16(const uint8_t *p) {
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rd32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
		((uint32_t)p[3] << 24);
}

static void set_name(zc_font_t *f, const char *name) {
	size_t i;
	memset(f->name, 0, sizeof(f->name));
	if (!name) return;
	for (i = 0; i < sizeof(f->name) - 1 && name[i]; i++) f->name[i] = name[i];
}

// -- opening -----------------------------------------------------------

static bool open_zhf(zc_font_t *f, const uint8_t *d, uint32_t len) {
	uint32_t n, idx_end, i;
	if (len < 32 || d[4] != 1) return false;
	n = rd16(d + 12);
	idx_end = 32 + n * 4;
	if (n == 0 || idx_end > len) return false;
	f->kind = ZC_FONT_STROKE;
	f->flags = d[5];
	f->upm = d[6] ? d[6] : 32;
	f->cap = d[7];
	f->xh = d[8];
	f->asc = d[9];
	f->desc = d[10];
	f->space = d[11];
	f->count = (uint16_t)n;
	f->index = d + 32;
	f->glyphs = d + idx_end;
	f->len = len - idx_end;
	// every glyph must lie inside the data, codepoints ascending
	for (i = 0; i < n; i++) {
		uint32_t off = rd16(f->index + i * 4 + 2);
		if (i && rd16(f->index + i * 4) <= rd16(f->index + (i - 1) * 4)) return false;
		if (off + 6 > f->len) return false;
		if (off + 6 + (uint32_t)f->glyphs[off + 5] * 2 > f->len) return false;
	}
	if (!f->name[0]) {
		memcpy(f->name, d + 16, 16);
		f->name[16] = 0;
	}
	for (i = 0; i < n; i++) {
		uint32_t cp = rd16(f->index + i * 4);
		if (cp >= 0x20 && cp < 0x7F)
			f->ascii[cp - 0x20] = (uint16_t)(rd16(f->index + i * 4 + 2) + 1);
	}
	return true;
}

static bool open_zfn(zc_font_t *f, const uint8_t *d, uint32_t len) {
	uint32_t n, data, i;
	if (len < 12) return false;
	f->w = d[4];
	f->h = d[5];
	f->stride = d[6];
	n = rd32(d + 8);
	if (!f->w || !f->h || !f->stride || f->stride * 8 < f->w || n == 0 || n > 65535)
		return false;
	data = (12 + n * 2 + 3) & ~3u;
	if (data > len || (uint64_t)data + (uint64_t)n * f->h * f->stride > len)
		return false;
	f->kind = ZC_FONT_BITMAP;
	f->flags = 0;
	f->upm = f->h;
	f->asc = d[7] ? d[7] : f->h;
	f->desc = (uint8_t)(f->h - f->asc);
	f->cap = (uint8_t)(f->asc * 3 / 4);
	f->xh = (uint8_t)(f->asc / 2);
	f->space = f->w;
	f->count = (uint16_t)n;
	f->index = d + 12;
	f->glyphs = d + data;
	f->len = len - data;
	for (i = 1; i < n; i++)
		if (rd16(f->index + i * 2) <= rd16(f->index + (i - 1) * 2)) return false;
	for (i = 0; i < n && i < 65535; i++) {
		uint32_t cp = rd16(f->index + i * 2);
		if (cp >= 0x20 && cp < 0x7F) f->ascii[cp - 0x20] = (uint16_t)(i + 1);
	}
	return true;
}

bool zc_font_open(zc_font_t *f, const void *file, uint32_t len, const char *name) {
	const uint8_t *d = (const uint8_t *)file;
	memset(f, 0, sizeof(*f));
	set_name(f, name);
	if (!d || len < 4) return false;
	if (!memcmp(d, "ZHF1", 4)) {
		if (open_zhf(f, d, len)) return true;
	} else if (!memcmp(d, "ZFN1", 4)) {
		if (open_zfn(f, d, len)) return true;
	}
	memset(f, 0, sizeof(*f));
	return false;
}

void zc_font_builtin(zc_font_t *f, const z_font_t *zf, int ascent, const char *name) {
	memset(f, 0, sizeof(*f));
	set_name(f, name);
	f->kind = ZC_FONT_BUILTIN;
	f->builtin = zf;
	f->w = zf->w;
	f->h = zf->h;
	f->stride = 1;
	f->upm = zf->h;
	f->asc = (uint8_t)(ascent > 0 && ascent <= zf->h ? ascent : zf->h);
	f->desc = (uint8_t)(f->h - f->asc);
	f->cap = (uint8_t)(f->asc * 3 / 4);
	f->xh = (uint8_t)(f->asc / 2);
	f->space = zf->w;
	{
		int c;
		for (c = 0x20; c < 0x7F; c++) {
			int i = z_font_index(zf, (uint32_t)c);
			if (i >= 0 && c != Z_GLYPH_MISSING) f->ascii[c - 0x20] = (uint16_t)(i + 1);
		}
	}
}

// -- glyph lookup ------------------------------------------------------

// Stroke glyph record for cp, or NULL.
static const uint8_t *stroke_glyph(const zc_font_t *f, uint32_t cp) {
	int lo = 0, hi = (int)f->count - 1;
	if (cp >= 0x20 && cp < 0x7F)
		return f->ascii[cp - 0x20] ? f->glyphs + f->ascii[cp - 0x20] - 1 : NULL;
	if (cp > 0xFFFF) return NULL;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		uint32_t c = rd16(f->index + mid * 4);
		if (c == cp) return f->glyphs + rd16(f->index + mid * 4 + 2);
		if (c < cp) lo = mid + 1; else hi = mid - 1;
	}
	return NULL;
}

// Bitmap rows (stride bytes each, h rows) for cp, or NULL.
static const uint8_t *bitmap_glyph(const zc_font_t *f, uint32_t cp) {
	if (cp >= 0x20 && cp < 0x7F) {
		uint32_t i = f->ascii[cp - 0x20];
		if (!i) return NULL;
		if (f->kind == ZC_FONT_BUILTIN) return f->builtin->glyphs + (i - 1) * f->builtin->h;
		return f->glyphs + (i - 1) * f->h * f->stride;
	}
	if (f->kind == ZC_FONT_BUILTIN) {
		int i;
		uint8_t b = z_cp_to_l9(cp);
		if (!b || b == Z_GLYPH_MISSING) return NULL;
		i = z_font_index(f->builtin, b);
		if (i < 0) return NULL;
		return f->builtin->glyphs + (uint32_t)i * f->builtin->h;
	} else {
		int lo = 0, hi = (int)f->count - 1;
		if (cp > 0xFFFF) return NULL;
		while (lo <= hi) {
			int mid = (lo + hi) / 2;
			uint32_t c = rd16(f->index + mid * 2);
			if (c == cp) return f->glyphs + (uint32_t)mid * f->h * f->stride;
			if (c < cp) lo = mid + 1; else hi = mid - 1;
		}
		return NULL;
	}
}

bool zc_font_has(const zc_font_t *f, uint32_t cp) {
	if (f->kind == ZC_FONT_STROKE) return stroke_glyph(f, cp) != NULL;
	if (f->kind == ZC_FONT_BITMAP || f->kind == ZC_FONT_BUILTIN)
		return bitmap_glyph(f, cp) != NULL;
	return false;
}

// The codepoint actually drawn for cp: itself, a look-alike, or 0 (box).
static uint32_t resolve(const zc_font_t *f, uint32_t cp) {
	uint8_t l;
	if (cp == 0x09) cp = ' ';
	if (zc_font_has(f, cp)) return cp;
	l = z_cp_lookalike(cp);
	if (l) {
		uint32_t c2 = z_l9_to_cp(l);
		if (zc_font_has(f, c2)) return c2;
	}
	if (cp == 0xA0 && zc_font_has(f, ' ')) return ' ';
	return 0;
}

// -- metrics -----------------------------------------------------------

// font units -> user units. The ratio is worked out once per font and
// size (a 64-bit division is hundreds of instructions on this CPU, and
// text asks for it several times a character).
static const zc_font_t *fu_font;
static zc_fix fu_size;
static int64_t fu_mult;		// user units per font unit, 16.16

static inline zc_fix fu(const zc_style_t *st, int v) {
	if (st->font != fu_font || st->size != fu_size) {
		fu_font = st->font;
		fu_size = st->size;
		fu_mult = ((int64_t)st->size * 65536) / (st->font->upm ? st->font->upm : 1);
	}
	return (zc_fix)(((int64_t)v * fu_mult) >> 16);
}

void zc_text_metrics(const zc_style_t *st, zc_fix *ascent, zc_fix *descent,
	zc_fix *line) {
	const zc_font_t *f = st->font;
	zc_fix a, d;
	if (!f || f->kind == ZC_FONT_NONE) {
		if (ascent) *ascent = 0;
		if (descent) *descent = 0;
		if (line) *line = st->size;
		return;
	}
	if (f->kind == ZC_FONT_STROKE) {
		// the cap height plus room for accents, and the descent; a line
		// is 1.25 em, which is what these proportions read best at
		a = fu(st, f->cap + (f->upm * 3) / 16);
		d = fu(st, f->desc);
		if (line) *line = st->size + st->size / 4;
	} else {
		a = fu(st, f->asc);
		d = fu(st, f->desc);
		if (line) *line = fu(st, f->h + 1);
	}
	if (ascent) *ascent = a;
	if (descent) *descent = d;
}

static zc_fix advance_of(const zc_style_t *st, uint32_t c) {
	const zc_font_t *f = st->font;
	zc_fix a;
	if (f->kind == ZC_FONT_STROKE) {
		const uint8_t *g = c ? stroke_glyph(f, c) : NULL;
		a = g ? fu(st, g[0]) : fu(st, (f->upm * 5) / 8);
		if (st->bold && !(f->flags & ZC_FONT_BOLD)) a += fu(st, 1);
	} else {
		a = fu(st, f->w);
		if (st->bold) a += fu(st, 1);
	}
	return a + st->track;
}

zc_fix zc_text_advance(const zc_style_t *st, uint32_t cp) {
	if (!st->font || st->font->kind == ZC_FONT_NONE) return 0;
	if (cp == '\n' || cp == '\r') return 0;
	return advance_of(st, resolve(st->font, cp));
}

zc_fix zc_text_width(const zc_style_t *st, const char *s, uint32_t len) {
	const char *e = s + len;
	zc_fix w = 0;
	while (s < e) w += zc_text_advance(st, z_utf8_next(&s, e));
	return w;
}

// -- drawing: stroke glyphs --------------------------------------------

// Working paths: one glyph's points (255 at most), and the capsules a
// thick pen makes of them (flushed whenever full).
static zc_pt_t g_pts[320];
static uint16_t g_sub[96];
static zc_pt_t w_pts[1536];
static uint16_t w_sub[160];
static zc_path_t g_path, w_path;
static bool paths_ready;

static void paths_init(void) {
	if (paths_ready) return;
	zc_path_init(&g_path, g_pts, (int)(sizeof(g_pts) / sizeof(g_pts[0])),
		g_sub, (int)(sizeof(g_sub) / sizeof(g_sub[0])));
	zc_path_init(&w_path, w_pts, (int)(sizeof(w_pts) / sizeof(w_pts[0])),
		w_sub, (int)(sizeof(w_sub) / sizeof(w_sub[0])));
	paths_ready = true;
}

static zc_fix pen_of(const zc_style_t *st) {
	const zc_font_t *f = st->font;
	zc_fix w;
	if (st->weight > 0) return st->weight;
	// 1.4 units of a 32-unit em for a regular face: one pixel at the
	// sizes where one pixel is right, and enough to close up the
	// parallel strokes of a duplex or triplex face when large
	w = fu(st, 14) / 10;
	if (st->bold && !(f->flags & ZC_FONT_BOLD)) w = (w * 9) / 5;
	return w;
}

// Builds glyph g at user (x, y) into g_path (through xf).
static void build_stroke(const zc_style_t *st, const zc_xf_t *xf,
	const uint8_t *g, zc_fix x, zc_fix y) {
	int n = g[5], i;
	const int8_t *p = (const int8_t *)(g + 6);
	bool pen_up = true;
	bool slant = st->italic && !(st->font->flags & ZC_FONT_ITALIC);
	zc_path_set_xf(&g_path, xf);
	zc_path_reset(&g_path);
	for (i = 0; i < n; i++) {
		int gx = p[2 * i], gy = p[2 * i + 1];
		zc_fix ux, uy;
		if (gx == -128) { pen_up = true; continue; }
		ux = x + fu(st, gx);
		uy = y + fu(st, gy);
		if (slant) ux -= fu(st, gy) / 5;
		if (pen_up) zc_path_move(&g_path, ux, uy);
		else zc_path_line(&g_path, ux, uy);
		pen_up = false;
	}
}

static void draw_stroke_direct(zc_target_t *t, const zc_xf_t *xf,
	const zc_style_t *st, const uint8_t *g, zc_fix x, zc_fix y,
	const zc_paint_t *p) {
	build_stroke(st, xf, g, x, y);
	zc_stroke(t, &g_path, pen_of(st), false, &w_path, p);
}

// -- the glyph cache ---------------------------------------------------

typedef struct cache_ent {
	const zc_font_t	*font;
	zc_fix			size, weight;
	int32_t			scale;		// xf a (== d)
	uint16_t		cp;
	uint8_t			flags;		// bold | italic << 1
	uint8_t			used;
	int16_t			bx, by;		// bitmap's top-left from the origin, device px
	uint16_t		w, h, wpl;
	uint32_t		*bits;
	struct cache_ent *next;
} cache_ent_t;

#define CACHE_BUCKETS 128

static uint8_t *c_mem;
static uint32_t c_bytes, c_used;
static cache_ent_t **c_tab;
static uint32_t c_hits, c_misses;

void zc_text_cache(void *mem, uint32_t bytes) {
	c_mem = (uint8_t *)mem;
	c_bytes = bytes;
	zc_text_cache_flush();
}

void zc_text_cache_flush(void) {
	c_used = 0;
	c_tab = NULL;
	if (!c_mem || c_bytes < CACHE_BUCKETS * sizeof(cache_ent_t *) + 1024) return;
	c_tab = (cache_ent_t **)c_mem;
	memset(c_tab, 0, CACHE_BUCKETS * sizeof(cache_ent_t *));
	c_used = (CACHE_BUCKETS * sizeof(cache_ent_t *) + 7) & ~7u;
}

void zc_text_cache_stats(uint32_t *hits, uint32_t *misses) {
	if (hits) *hits = c_hits;
	if (misses) *misses = c_misses;
}

static void *c_alloc(uint32_t n) {
	void *p;
	n = (n + 7) & ~7u;
	if (c_used + n > c_bytes) return NULL;
	p = c_mem + c_used;
	c_used += n;
	return p;
}

static uint32_t c_hash(const zc_font_t *f, zc_fix size, int32_t scale, uint32_t cp) {
	uint32_t h = (uint32_t)(uintptr_t)f * 2654435761u;
	h ^= (uint32_t)size * 40503u;
	h ^= (uint32_t)scale * 2246822519u;
	h ^= cp * 3266489917u;
	return (h >> 7) % CACHE_BUCKETS;
}

// The cached rendering of glyph g, or NULL if it cannot be cached (no
// cache, too big, no room even after emptying).
static cache_ent_t *cache_get(const zc_style_t *st, const zc_xf_t *xf,
	const uint8_t *g, uint32_t cp) {
	uint32_t hb;
	cache_ent_t *e;
	zc_fix pen = pen_of(st);
	uint8_t fl = (uint8_t)((st->bold ? 1 : 0) | (st->italic ? 2 : 0));
	int64_t s = xf->a;
	int x0, y0, x1, y1, w, h, wpl;
	zc_target_t t;
	zc_xf_t gx;
	zc_paint_t ink = zc_black();
	zc_fix r;
	int attempt;

	if (!c_tab) return NULL;
	hb = c_hash(st->font, st->size, xf->a, cp);
	for (e = c_tab[hb]; e; e = e->next) {
		if (e->font == st->font && e->size == st->size && e->weight == pen &&
			e->scale == xf->a && e->cp == cp && e->flags == fl) {
			c_hits++;
			return e;
		}
	}
	c_misses++;

	// device box of the glyph at origin (0, 0): ink box, slant, pen
	{
		const int8_t *b = (const int8_t *)(g + 1);
		zc_fix ux0 = fu(st, b[0]), ux1 = fu(st, b[2]);
		zc_fix uy0 = fu(st, b[1]), uy1 = fu(st, b[3]);
		if (st->italic && !(st->font->flags & ZC_FONT_ITALIC)) {
			ux1 -= uy0 / 5;
			ux0 -= uy1 / 5;
		}
		r = (zc_fix)((((int64_t)pen * s) >> 16) / 2) + ZC_ONE * 2;
		x0 = (int)(((((int64_t)ux0 * s) >> 16) - r) >> ZC_FRAC) - 1;
		x1 = (int)(((((int64_t)ux1 * s) >> 16) + r) >> ZC_FRAC) + 2;
		y0 = (int)(((((int64_t)uy0 * s) >> 16) - r) >> ZC_FRAC) - 1;
		y1 = (int)(((((int64_t)uy1 * s) >> 16) + r) >> ZC_FRAC) + 2;
	}
	w = x1 - x0;
	h = y1 - y0;
	if (w <= 0 || h <= 0 || w > 512 || h > 512) return NULL;
	wpl = (w + 31) / 32;

	for (attempt = 0; attempt < 2; attempt++) {
		e = (cache_ent_t *)c_alloc(sizeof(cache_ent_t));
		if (e) {
			e->bits = (uint32_t *)c_alloc((uint32_t)(wpl * h * 4));
			if (e->bits) break;
		}
		zc_text_cache_flush();
		if (!c_tab) return NULL;
		hb = c_hash(st->font, st->size, xf->a, cp);
		e = NULL;
	}
	if (!e) return NULL;

	memset(e->bits, 0xFF, (size_t)wpl * h * 4);
	zc_target_bits(&t, e->bits, wpl, w, h);
	zc_target_origin(&t, x0, y0);
	gx = *xf;
	gx.e = 0;
	gx.f = 0;
	draw_stroke_direct(&t, &gx, st, g, 0, 0, &ink);

	e->font = st->font;
	e->size = st->size;
	e->weight = pen;
	e->scale = xf->a;
	e->cp = (uint16_t)cp;
	e->flags = fl;
	e->bx = (int16_t)x0;
	e->by = (int16_t)y0;
	e->w = (uint16_t)w;
	e->h = (uint16_t)h;
	e->wpl = (uint16_t)wpl;
	e->next = c_tab[hb];
	c_tab[hb] = e;
	return e;
}

// -- drawing: bitmap glyphs --------------------------------------------

// Bits of a byte in the other order: font rows are MSB-first, the
// framebuffer least significant bit leftmost.
static uint8_t rev_byte(uint8_t b) {
	b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
	b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
	return (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
}

// One bitmap glyph, as a 1bpp mask (0 = ink) at device scale `k` (16.16
// device pixels per font pixel), its top-left at device (dx, dy). Bold
// smears one pixel right; italic shifts rows right, a pixel per four.
static void draw_bitmap(zc_target_t *t, const zc_style_t *st,
	const uint8_t *rows, zc_fix dx, zc_fix dy, int32_t k, const zc_xf_t *turned,
	const zc_paint_t *p) {
	const zc_font_t *f = st->font;
	uint32_t mask[32 * 2];			// up to 32 rows of up to 64 bits
	int w = f->w, h = f->h, stride = f->stride, y, i;
	int slant = st->italic ? (h - 1) / 4 : 0;
	int mw = w + (st->bold ? 1 : 0) + slant;
	int wpl = (mw + 31) / 32;
	if (h > 32 || mw > 64 || stride > 4) return;
	for (y = 0; y < h; y++) {
		uint64_t ink = 0;
		for (i = 0; i < stride; i++)
			ink |= (uint64_t)rev_byte(rows[y * stride + i]) << (8 * i);
		ink &= (w >= 64) ? ~0ull : ((1ull << w) - 1);
		if (st->bold) ink |= ink << 1;
		if (slant) ink <<= (h - 1 - y) / 4;
		mask[y * wpl] = ~(uint32_t)ink;
		if (wpl > 1) mask[y * wpl + 1] = ~(uint32_t)(ink >> 32);
	}
	if (turned) {
		// a card laid sideways on a sheet: the glyph turns with it
		zc_blit_xf(t, ZC_IMG_MASK, mask, wpl, mw, h, turned, p);
		return;
	}
	zc_blit_mask(t, mask, wpl, mw, h, dx, dy,
		(zc_fix)(((int64_t)mw * k) >> 8), (zc_fix)(((int64_t)h * k) >> 8), p);
}

// -- drawing -----------------------------------------------------------

static void draw_box(zc_target_t *t, const zc_xf_t *xf, const zc_style_t *st,
	zc_fix x, zc_fix y, const zc_paint_t *p) {
	const zc_font_t *f = st->font;
	zc_fix adv = advance_of(st, 0) - st->track;
	zc_fix bw = (adv * 3) / 4, bh = fu(st, f->kind == ZC_FONT_STROKE ? f->cap : f->asc);
	zc_fix pen = f->kind == ZC_FONT_STROKE ? pen_of(st) : fu(st, 1);
	paths_init();
	zc_path_set_xf(&g_path, xf);
	zc_path_reset(&g_path);
	zc_path_rect(&g_path, x + adv / 8, y - bh, bw, bh);
	zc_stroke(t, &g_path, pen, true, &w_path, p);
}

// Rounds a device coordinate to the pixel grid (glyph origins sit on
// whole pixels, so a cached glyph and a drawn one are the same pixels).
static inline zc_fix snap(zc_fix v) {
	return (v + ZC_HALF) & ~(ZC_ONE - 1);
}

zc_fix zc_text_draw(zc_target_t *t, const zc_xf_t *xf, const zc_style_t *st,
	zc_fix x, zc_fix y, const char *s, uint32_t len, const zc_paint_t *p) {
	const zc_font_t *f = st->font;
	const char *e = s + len;
	zc_fix x_start = x;
	bool axis = zc_xf_is_axis(xf);
	int32_t scale = zc_xf_scale_of(xf);
	zc_fix asc, desc;

	if (!f || f->kind == ZC_FONT_NONE) return x;
	paths_init();
	zc_text_metrics(st, &asc, &desc, NULL);

	if (t->fmt == ZC_REC) {
		// described, not drawn: the run as it stands, and its underline
		// (below) as an ordinary stroke
		zc_style_t plain = *st;
		plain.underline = 0;
		if (t->rec->text && !zc_clip_empty(t))
			t->rec->text(t, xf, &plain, x, y, s, len, p);
		x += zc_text_width(st, s, len);
		s = e;
	}

	while (s < e) {
		uint32_t cp = z_utf8_next(&s, e);
		uint32_t c;
		zc_fix adv;
		if (cp == '\n' || cp == '\r') continue;
		c = resolve(f, cp);
		adv = advance_of(st, c);

		// cull characters outside the clip (a page drawn in bands
		// draws each line once or twice, not once per band)
		{
			zc_fix dx0, dy0, dx1, dy1, lx, hx, ly, hy;
			zc_xf_apply(xf, x - adv, y - asc * 2, &dx0, &dy0);
			zc_xf_apply(xf, x + adv * 2, y + desc * 2, &dx1, &dy1);
			lx = dx0 < dx1 ? dx0 : dx1; hx = dx0 < dx1 ? dx1 : dx0;
			ly = dy0 < dy1 ? dy0 : dy1; hy = dy0 < dy1 ? dy1 : dy0;
			if (!axis) {
				// rotated: use a square that covers any orientation
				zc_fix rr = (zc_fix)((((int64_t)(adv * 3 + asc * 2 + desc * 2)) * scale) >> 16);
				zc_xf_apply(xf, x, y, &dx0, &dy0);
				lx = dx0 - rr; hx = dx0 + rr; ly = dy0 - rr; hy = dy0 + rr;
			}
			if (hx < (zc_fix)t->cx0 * ZC_ONE || lx > (zc_fix)t->cx1 * ZC_ONE ||
				hy < (zc_fix)t->cy0 * ZC_ONE || ly > (zc_fix)t->cy1 * ZC_ONE) {
				x += adv;
				continue;
			}
		}

		if (c == ' ' || c == 0xA0) {
			// nothing to draw
		} else if (c == 0) {
			draw_box(t, xf, st, x, y, p);
		} else if (f->kind == ZC_FONT_STROKE) {
			const uint8_t *g = stroke_glyph(f, c);
			if (axis) {
				zc_fix ox, oy;
				zc_xf_apply(xf, x, y, &ox, &oy);
				ox = snap(ox);
				oy = snap(oy);
				{
					cache_ent_t *ce = NULL;
					// a cached glyph is the same pixels on a grey band as on
					// the screen: coverage is all or nothing either way
					if (xf->a == xf->d) ce = cache_get(st, xf, g, c);
					if (ce) {
						zc_blit_mask(t, ce->bits, ce->wpl, ce->w, ce->h,
							ox + (zc_fix)ce->bx * ZC_ONE, oy + (zc_fix)ce->by * ZC_ONE,
							(zc_fix)ce->w * ZC_ONE, (zc_fix)ce->h * ZC_ONE, p);
					} else {
						zc_xf_t gx = *xf;
						gx.e = ox;
						gx.f = oy;
						draw_stroke_direct(t, &gx, st, g, 0, 0, p);
					}
				}
			} else {
				draw_stroke_direct(t, xf, st, g, x, y, p);
			}
		} else {
			const uint8_t *rows = bitmap_glyph(f, c);
			zc_fix ox, oy;
			// a bitmap is drawn upright, scaled by the transform
			int32_t k = (int32_t)(((int64_t)scale * st->size) / ((int64_t)(f->h ? f->h : 1) * ZC_ONE));
			zc_xf_apply(xf, x, y - fu(st, f->asc), &ox, &oy);
			if (axis) {
				draw_bitmap(t, st, rows, snap(ox), snap(oy), k, NULL, p);
			} else {
				// font pixels -> user units -> device, turned
				int32_t u = (int32_t)(((int64_t)st->size << 16) / ((int64_t)(f->h ? f->h : 1) * ZC_ONE));
				zc_xf_t g = { u, 0, 0, u, x, y - fu(st, f->asc) };
				zc_xf_t m = zc_xf_mul(xf, &g);
				draw_bitmap(t, st, rows, ox, oy, k, &m, p);
			}
		}
		x += adv;
	}

	if (st->underline && x > x_start) {
		zc_fix uy = y + desc / 2 + fu(st, 1) / 2;
		zc_fix pen = f->kind == ZC_FONT_STROKE ? pen_of(st) : fu(st, 1);
		zc_path_set_xf(&g_path, xf);
		zc_path_reset(&g_path);
		zc_path_move(&g_path, x_start, uy);
		zc_path_line(&g_path, x - st->track, uy);
		zc_stroke(t, &g_path, pen, false, &w_path, p);
	}
	return x;
}

// -- wrapping ----------------------------------------------------------

int zc_text_wrap(const zc_style_t *st, const char *s, uint32_t len,
	zc_fix maxw, zc_line_t *lines, int max_lines, bool *more) {
	uint32_t pos = 0;
	int n = 0;
	if (more) *more = false;
	if (max_lines <= 0) return 0;

	for (;;) {
		uint32_t i = pos, end = len, next = len;
		uint32_t sp = 0;			// start of the last run of spaces
		bool have_sp = false, prev_sp = false, last = true;
		zc_fix w = 0, w_sp = 0;

		while (i < len) {
			const char *c = s + i;
			uint32_t cp = z_utf8_next(&c, s + len);
			uint32_t ni = (uint32_t)(c - s);
			zc_fix a;
			if (cp == '\n') { end = i; next = ni; last = false; break; }
			a = zc_text_advance(st, cp);
			if (cp == ' ' || cp == '\t') {
				if (!prev_sp && i > pos) { sp = i; w_sp = w; have_sp = true; }
				prev_sp = true;
				w += a;
				i = ni;
				continue;
			}
			prev_sp = false;
			if (maxw > 0 && w + a > maxw && i > pos) {
				last = false;
				if (have_sp) {
					end = sp;
					w = w_sp;
					next = sp;
					while (next < len && (s[next] == ' ' || s[next] == '\t')) next++;
				} else {
					end = i;
					next = i;
				}
				break;
			}
			w += a;
			i = ni;
		}

		// trailing spaces are not part of the line or its width
		{
			uint32_t le = end;
			while (le > pos && (s[le - 1] == ' ' || s[le - 1] == '\t')) le--;
			if (le != end) w = zc_text_width(st, s + pos, le - pos);
			end = le;
		}

		if (n == max_lines) {
			if (more) *more = true;
			break;
		}
		lines[n].start = pos;
		lines[n].len = end - pos;
		lines[n].width = w;
		n++;
		if (last) break;
		pos = next;
	}
	return n;
}

// -- for font writers --------------------------------------------------

uint32_t zc_font_resolve(const zc_font_t *f, uint32_t cp) {
	if (!f || f->kind == ZC_FONT_NONE) return 0;
	return resolve(f, cp);
}

const int8_t *zc_font_strokes(const zc_font_t *f, uint32_t c, int *npts, int *adv,
	int8_t box[4]) {
	const uint8_t *g;
	if (!f || f->kind != ZC_FONT_STROKE || !c) return NULL;
	g = stroke_glyph(f, c);
	if (!g) return NULL;
	if (npts) *npts = g[5];
	if (adv) *adv = g[0];
	if (box) { box[0] = (int8_t)g[1]; box[1] = (int8_t)g[2]; box[2] = (int8_t)g[3]; box[3] = (int8_t)g[4]; }
	return (const int8_t *)(g + 6);
}

const uint8_t *zc_font_rows(const zc_font_t *f, uint32_t c) {
	if (!f || (f->kind != ZC_FONT_BITMAP && f->kind != ZC_FONT_BUILTIN) || !c) return NULL;
	return bitmap_glyph(f, c);
}

zc_fix zc_text_pen(const zc_style_t *st) {
	if (!st->font || st->font->kind != ZC_FONT_STROKE) return st->font ? fu(st, 1) : 0;
	return pen_of(st);
}
