/*
 * Zeitlos -- the arcade runtime, the arithmetic half. See zarcade.h for
 * the model and zarcade_int.h for how the two halves split.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Nothing here includes a Zeitlos header beyond the font type, so the
 * host tests compile this file unchanged.
 *
 * -- the budget --
 *
 * picorv32 at 48MHz retires a few million instructions a second, so a
 * 60Hz frame is on the order of 100k instructions for everything: the
 * game, this file, and the blitter calls. Everything per-frame below is
 * linear in what changed, never in the size of the screen:
 *
 *   - overlays are compared with last frame's IN ORDER (entry i with
 *     entry i), not searched -- a game submits them in the same order
 *     every frame, so that is the same answer for a fraction of the work
 *   - the dirty map is 30 rows of 40 bits, walked as words
 *   - colour costs one blitter operation per plane, which is the
 *     unavoidable part (docs/color.md, "Drawing")
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zarcade_int.h"

ar_state_t ar__s;

/* ===================================================================
 * inks
 * =================================================================== */

const ar_ink_t ar_ink_black  = {{ 0, 0, 0, 0, 0, 0, 0, 0 }};
const ar_ink_t ar_ink_white  = {{ 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff }};
const ar_ink_t ar_ink_grey12 = {{ 0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00 }};
const ar_ink_t ar_ink_grey25 = {{ 0x88, 0x22, 0x88, 0x22, 0x88, 0x22, 0x88, 0x22 }};
const ar_ink_t ar_ink_grey50 = {{ 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55 }};
const ar_ink_t ar_ink_grey75 = {{ 0x77, 0xdd, 0x77, 0xdd, 0x77, 0xdd, 0x77, 0xdd }};
const ar_ink_t ar_ink_hlines = {{ 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00 }};
const ar_ink_t ar_ink_vlines = {{ 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa }};
const ar_ink_t ar_ink_diag   = {{ 0x88, 0x44, 0x22, 0x11, 0x88, 0x44, 0x22, 0x11 }};
const ar_ink_t ar_ink_dots   = {{ 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00 }};

/* The blitter's own 4x4 ordered dither, so a level here and a
 * z_fb_hw_fill_shade() of the same level are the same picture. */
static const uint8_t bayer4[4][4] = {
	{  0,  8,  2, 10 },
	{ 12,  4, 14,  6 },
	{  3, 11,  1,  9 },
	{ 15,  7, 13,  5 }
};

void ar_ink_level(ar_ink_t *out, int level) {
	int y, x;
	if (level < 0) level = 0;
	if (level > 16) level = 16;
	for (y = 0; y < 8; y++) {
		uint8_t r = 0;
		for (x = 0; x < 8; x++)
			if (level > bayer4[y & 3][x & 3]) r |= (uint8_t)(0x80u >> x);
		out->row[y] = r;
	}
}

static bool ink_solid(const uint8_t *p, int *bit) {
	int i;
	for (i = 1; i < 8; i++) if (p[i] != p[0]) return false;
	if (p[0] == 0x00) { *bit = 0; return true; }
	if (p[0] == 0xff) { *bit = 1; return true; }
	return false;
}

static int ink_weight(const uint8_t *p) {
	int i, n = 0;
	for (i = 0; i < 8; i++) {
		uint8_t v = p[i];
		while (v) { n += v & 1; v >>= 1; }
	}
	return n;
}

/* ===================================================================
 * where things land: plane 0 rectangles, and the other planes
 * =================================================================== */

static ar_rect_t clip_r = { 0, 0, AR_W, AR_H };

static int plane_count(void) {
	return ar__s.mode == AR_MODE_FULL_COLOR ? 4 : 1;
}

/* Plane k's framebuffer offset from plane 0 (z_color_16: quadrants). */
static void plane_off(int k, int *dx, int *dy) {
	*dx = (k & 1) ? 320 : 0;
	*dy = (k & 2) ? 240 : 0;
}

/* Clip a logical rectangle to the current clip. Returns false if empty;
 * sx and sy are moved by the same amount as the rectangle's origin. */
static bool clip_logical(int *x, int *y, int *w, int *h, int *sx, int *sy) {
	int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;
	int cx0 = clip_r.x, cy0 = clip_r.y;
	int cx1 = clip_r.x + clip_r.w, cy1 = clip_r.y + clip_r.h;
	if (x0 < cx0) { if (sx) *sx += cx0 - x0; x0 = cx0; }
	if (y0 < cy0) { if (sy) *sy += cy0 - y0; y0 = cy0; }
	if (x1 > cx1) x1 = cx1;
	if (y1 > cy1) y1 = cy1;
	if (x1 <= x0 || y1 <= y0) return false;
	*x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
	return true;
}

/* Logical (already clipped) -> the pieces of it that may be drawn, in
 * plane-0 framebuffer coordinates. Calls `fn` once per piece with the
 * source shift the piece needs. */
typedef void (*piece_fn)(int x, int y, int w, int h, int dsx, int dsy,
	const void *ctx);

static void for_pieces(int x, int y, int w, int h, piece_fn fn,
	const void *ctx) {

	int i;
	int fx, fy;

	if (ar__s.targets_stale) arh_targets();
	if (!ar__s.can_draw) return;
	fx = ar__s.ox + x;
	fy = ar__s.oy + y;

	for (i = 0; i < ar__s.nvis; i++) {
		const ar_rect_t *v = &ar__s.vis[i];
		int x0 = fx > v->x ? fx : v->x;
		int y0 = fy > v->y ? fy : v->y;
		int x1 = (fx + w) < (v->x + v->w) ? (fx + w) : (v->x + v->w);
		int y1 = (fy + h) < (v->y + v->h) ? (fy + h) : (v->y + v->h);
		if (x1 <= x0 || y1 <= y0) continue;
		fn(x0, y0, x1 - x0, y1 - y0, x0 - fx, y0 - fy, ctx);
	}
}

/* ===================================================================
 * fills
 * =================================================================== */

typedef struct {
	uint8_t pat[4][8];     /* per plane (mono uses [0]) */
} fill_ctx_t;

static void pattern_or_fill(int x, int y, int w, int h, const uint8_t *p) {
	int bit;
	if (ink_solid(p, &bit)) arh_fill(x, y, w, h, bit);
	else arh_pattern(x, y, w, h, p);
}

static void fill_piece(int x, int y, int w, int h, int dsx, int dsy,
	const void *ctx) {
	const fill_ctx_t *f = (const fill_ctx_t *)ctx;
	int k, n = plane_count();
	(void)dsx; (void)dsy;
	for (k = 0; k < n; k++) {
		int px, py;
		plane_off(k, &px, &py);
		pattern_or_fill(x + px, y + py, w, h, f->pat[k]);
	}
}

/* Per-plane patterns for "c1 where pat is set, c0 elsewhere". In mono
 * the two inks are combined the same way -- an 8x8 screen-aligned
 * pattern of two 8x8 screen-aligned patterns is still one -- so the
 * result is exact, not an approximation. */
static void make_fill2(fill_ctx_t *f, int c0, int c1, const uint8_t *pat) {
	int k, r;
	if (ar__s.mode == AR_MODE_FULL_COLOR) {
		for (k = 0; k < 4; k++) {
			uint8_t m0 = ((c0 >> k) & 1) ? 0xff : 0x00;
			uint8_t m1 = ((c1 >> k) & 1) ? 0xff : 0x00;
			for (r = 0; r < 8; r++)
				f->pat[k][r] = (uint8_t)((pat[r] & m1) | (~pat[r] & m0));
		}
	} else {
		const uint8_t *i0 = ar__s.d->ink[c0 & 15].row;
		const uint8_t *i1 = ar__s.d->ink[c1 & 15].row;
		for (r = 0; r < 8; r++)
			f->pat[0][r] = (uint8_t)((pat[r] & i1[r]) | (~pat[r] & i0[r]));
	}
}

void ar_fill(int x, int y, int w, int h, int c) {
	fill_ctx_t f;
	if (!clip_logical(&x, &y, &w, &h, NULL, NULL)) return;
	make_fill2(&f, c, c, ar_ink_black.row);
	for_pieces(x, y, w, h, fill_piece, &f);
}

void ar_fill2(int x, int y, int w, int h, int c0, int c1,
	const ar_ink_t *pat) {
	fill_ctx_t f;
	if (!clip_logical(&x, &y, &w, &h, NULL, NULL)) return;
	make_fill2(&f, c0, c1, pat->row);
	for_pieces(x, y, w, h, fill_piece, &f);
}

/* ===================================================================
 * bitmaps and sprites
 * =================================================================== */

typedef struct {
	const void *bits;
	int stride, sx, sy, c;
	const ar_sprite_t *spr;
} blit_ctx_t;

static void bitmap_piece(int x, int y, int w, int h, int dsx, int dsy,
	const void *ctx) {

	const blit_ctx_t *b = (const blit_ctx_t *)ctx;
	int sx = b->sx + dsx, sy = b->sy + dsy;

	if (ar__s.mode == AR_MODE_FULL_COLOR) {
		int k;
		for (k = 0; k < 4; k++) {
			int px, py;
			plane_off(k, &px, &py);
			arh_blit(b->bits, b->stride, sx, sy, x + px, y + py, w, h,
				((b->c >> k) & 1) ? ARH_OR : ARH_ANDN);
		}
	} else {
		const uint8_t *ink = ar__s.d->ink[b->c & 15].row;
		int bit;
		if (!ink_solid(ink, &bit)) bit = ink_weight(ink) >= 32;
		arh_blit(b->bits, b->stride, sx, sy, x, y, w, h,
			bit ? ARH_OR : ARH_ANDN);
	}
}

void ar_bitmap(const void *bits, int stride, int sx, int sy,
	int x, int y, int w, int h, int c) {
	blit_ctx_t b;
	if (!clip_logical(&x, &y, &w, &h, &sx, &sy)) return;
	b.bits = bits; b.stride = stride; b.sx = sx; b.sy = sy; b.c = c;
	b.spr = NULL;
	for_pieces(x, y, w, h, bitmap_piece, &b);
}

static void sprite_piece(int x, int y, int w, int h, int dsx, int dsy,
	const void *ctx) {

	const blit_ctx_t *b = (const blit_ctx_t *)ctx;
	const ar_sprite_t *s = b->spr;
	int sx = b->sx + dsx, sy = b->sy + dsy;

	if (ar__s.mode == AR_MODE_FULL_COLOR) {
		int k;
		for (k = 0; k < 4; k++) {
			int px, py;
			plane_off(k, &px, &py);
			if (s->plane[k])
				arh_sprite(s->plane[k], s->mask, s->stride, sx, sy,
					x + px, y + py, w, h);
			else
				arh_blit(s->mask, s->stride, sx, sy, x + px, y + py,
					w, h, ARH_ANDN);
		}
	} else {
		arh_sprite(s->mono, s->mask, s->stride, sx, sy, x, y, w, h);
	}
}

void ar_sprite(const ar_sprite_t *s, int x, int y) {
	ar_sprite_part(s, 0, 0, s->w, s->h, x, y);
}

void ar_sprite_part(const ar_sprite_t *s, int sx, int sy, int w, int h,
	int x, int y) {
	blit_ctx_t b;
	if (!s->mask) return;
	if (sx < 0) { w += sx; x -= sx; sx = 0; }
	if (sy < 0) { h += sy; y -= sy; sy = 0; }
	if (sx + w > s->w) w = s->w - sx;
	if (sy + h > s->h) h = s->h - sy;
	if (w <= 0 || h <= 0) return;
	if (!clip_logical(&x, &y, &w, &h, &sx, &sy)) return;
	b.bits = NULL; b.stride = s->stride; b.sx = sx; b.sy = sy; b.c = 0;
	b.spr = s;
	for_pieces(x, y, w, h, sprite_piece, &b);
}

/* -- the sprite pool --
 *
 * Sprites live for the whole run, so a bump allocator is all that is
 * needed: no free, no fragmentation. Sized for a few hundred small
 * sprites; a game that needs more says so with -DAR_POOL_WORDS. */
#ifndef AR_POOL_WORDS
#define AR_POOL_WORDS 6144     /* 24KB */
#endif

static uint32_t pool[AR_POOL_WORDS];
static int pool_used;

static uint32_t *pool_take(int words) {
	uint32_t *p;
	if (pool_used + words > AR_POOL_WORDS) return NULL;
	p = &pool[pool_used];
	pool_used += words;
	memset(p, 0, (size_t)words * 4u);
	return p;
}

static int hexval(char ch) {
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
	if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
	return -1;
}

static bool sprite_alloc(ar_sprite_t *s, int w, int h, const bool used[4]) {
	int words, k;
	s->w = (int16_t)w;
	s->h = (int16_t)h;
	s->stride = (int16_t)(((w + 31) / 32) * 4);
	/* +1 word: the blitter may read one word past the last it needs
	 * (zgfx.h, z_fb_hw_blit_mem()). */
	words = (s->stride / 4) * h + 1;
	s->mask = pool_take(words);
	s->mono = pool_take(words);
	if (!s->mask || !s->mono) return false;
	for (k = 0; k < 4; k++) {
		s->plane[k] = NULL;
		if (used[k]) {
			s->plane[k] = pool_take(words);
			if (!s->plane[k]) return false;
		}
	}
	return true;
}

static void setbit(uint32_t *b, int stride, int x, int y) {
	b[y * (stride / 4) + (x >> 5)] |= 1u << (x & 31);
}

bool ar_sprite_from_pixels(ar_sprite_t *s, const uint8_t *px, int w, int h) {

	bool used[4] = { false, false, false, false };
	int x, y, k;

	memset(s, 0, sizeof(*s));

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			uint8_t c = px[y * w + x];
			if (c == 0xff) continue;
			for (k = 0; k < 4; k++) if ((c >> k) & 1) used[k] = true;
		}

	if (!sprite_alloc(s, w, h, used)) return false;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			uint8_t c = px[y * w + x];
			const uint8_t *ink;
			if (c == 0xff) continue;
			c &= 15;
			setbit(s->mask, s->stride, x, y);
			for (k = 0; k < 4; k++)
				if ((c >> k) & 1) setbit(s->plane[k], s->stride, x, y);
			ink = ar__s.d ? ar__s.d->ink[c].row : ar_ink_white.row;
			if (ink[y & 7] & (0x80u >> (x & 7)))
				setbit(s->mono, s->stride, x, y);
		}

	return true;
}

bool ar_sprite_make(ar_sprite_t *s, const char *const *rows, int w, int h,
	const uint8_t *recolor) {

	/* Built through a pixel array so both entry points share one
	 * path. 64x64 is far beyond any sprite these games draw. */
	static uint8_t px[64 * 64];
	int x, y;

	if (w > 64 || h > 64 || w <= 0 || h <= 0) return false;

	for (y = 0; y < h; y++) {
		const char *r = rows[y];
		bool ended = false;
		for (x = 0; x < w; x++) {
			int v = -1;
			if (!ended && r[x] == 0) ended = true;
			if (!ended) v = hexval(r[x]);
			if (v >= 0 && recolor) v = recolor[v] & 15;
			px[y * w + x] = (v < 0) ? 0xff : (uint8_t)v;
		}
	}

	return ar_sprite_from_pixels(s, px, w, h);
}

/* ===================================================================
 * text
 * =================================================================== */

static int glyph_bits(const z_font_t *f, uint8_t ch, int row) {
	int gi = z_font_index(f, ch);
	if (gi < 0) gi = z_font_index(f, '?');
	if (gi < 0) return 0;
	return f->glyphs[gi * f->h + row];
}

int ar_text_width(const char *s, const z_font_t *f, int scale) {
	int n = (int)strlen(s);
	if (scale < 1) scale = 1;
	return n * f->w * scale;
}

/* Render into a 1bpp buffer, zgfx layout. */
static void render_text(uint32_t *bits, int stride, int maxw, int maxh,
	const char *s, const z_font_t *f, int scale) {

	int words = stride / 4;
	int cx = 0;

	memset(bits, 0, (size_t)stride * (size_t)maxh);

	for (; *s; s++) {
		int row, col, sy, sxx;
		for (row = 0; row < f->h; row++) {
			int g = glyph_bits(f, (uint8_t)*s, row);
			if (!g) continue;
			for (col = 0; col < f->w; col++) {
				if (!(g & (0x80 >> col))) continue;
				for (sy = 0; sy < scale; sy++) {
					int py = row * scale + sy;
					if (py >= maxh) break;
					for (sxx = 0; sxx < scale; sxx++) {
						int px = cx + col * scale + sxx;
						if (px >= maxw) break;
						bits[py * words + (px >> 5)] |= 1u << (px & 31);
					}
				}
			}
		}
		cx += f->w * scale;
		if (cx >= maxw) break;
	}
}

/* Scratch for ar_text(): 320 wide, up to 64 high (8x16 at 4x). */
#define TXT_STRIDE 44
#define TXT_H      64
static uint32_t txt_buf[(TXT_STRIDE / 4) * TXT_H + 1];

int ar_text(int x, int y, const char *s, int c, const z_font_t *f,
	int scale) {
	int w, h;
	if (scale < 1) scale = 1;
	if (scale > 4) scale = 4;
	w = ar_text_width(s, f, scale);
	h = f->h * scale;
	if (w > TXT_STRIDE * 8) w = TXT_STRIDE * 8;
	if (h > TXT_H) h = TXT_H;
	render_text(txt_buf, TXT_STRIDE, w, h, s, f, scale);
	ar_bitmap(txt_buf, TXT_STRIDE, 0, 0, x, y, w, h, c);
	return w;
}

bool ar_label_make(ar_label_t *l, uint32_t *buf, int buf_bytes,
	const char *s, const z_font_t *f, int scale) {
	int w, h, stride;
	if (scale < 1) scale = 1;
	w = ar_text_width(s, f, scale);
	h = f->h * scale;
	stride = ((w + 31) / 32) * 4;
	if (w <= 0 || stride * h + 4 > buf_bytes) return false;
	render_text(buf, stride, w, h, s, f, scale);
	l->bits = buf;
	l->w = (int16_t)w;
	l->h = (int16_t)h;
	l->stride = (int16_t)stride;
	return true;
}

/* ===================================================================
 * dirty tiles
 * =================================================================== */

static uint64_t dirty_rows[AR_TH];

void ar_dirty(int x, int y, int w, int h) {
	int tx0, ty0, tx1, ty1, ty;
	uint64_t m;
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > AR_W) w = AR_W - x;
	if (y + h > AR_H) h = AR_H - y;
	if (w <= 0 || h <= 0) return;
	tx0 = x / AR_TILE;
	ty0 = y / AR_TILE;
	tx1 = (x + w - 1) / AR_TILE;
	ty1 = (y + h - 1) / AR_TILE;
	m = ((tx1 - tx0 + 1) >= 64) ? ~0ull
		: (((1ull << (tx1 - tx0 + 1)) - 1ull) << tx0);
	for (ty = ty0; ty <= ty1; ty++) dirty_rows[ty] |= m;
}

void ar_dirty_all(void) {
	int ty;
	for (ty = 0; ty < AR_TH; ty++) dirty_rows[ty] = (1ull << AR_TW) - 1ull;
}

/* ===================================================================
 * overlays
 * =================================================================== */

enum { OV_FILL = 1, OV_SPRITE, OV_BITMAP };

typedef struct {
	uint8_t type, c;
	int16_t x, y, w, h;
	int16_t sx, sy, stride;
	const void *p;
} ov_t;

#ifndef AR_MAX_OV
#define AR_MAX_OV 160
#endif

static ov_t ov_a[AR_MAX_OV], ov_b[AR_MAX_OV];
static ov_t *ov_cur = ov_a, *ov_prev = ov_b;
static int n_cur, n_prev;
static bool ov_forget;      /* nothing of the previous list is on screen */

static void ov_add(const ov_t *o) {
	if (n_cur >= AR_MAX_OV) return;
	ov_cur[n_cur++] = *o;
}

void ar_ov_fill(int x, int y, int w, int h, int c) {
	ov_t o;
	memset(&o, 0, sizeof(o));
	o.type = OV_FILL; o.c = (uint8_t)c;
	o.x = (int16_t)x; o.y = (int16_t)y; o.w = (int16_t)w; o.h = (int16_t)h;
	ov_add(&o);
}

void ar_ov_sprite(const ar_sprite_t *s, int x, int y) {
	ar_ov_sprite_part(s, 0, 0, s->w, s->h, x, y);
}

void ar_ov_sprite_part(const ar_sprite_t *s, int sx, int sy, int w, int h,
	int x, int y) {
	ov_t o;
	if (w <= 0 || h <= 0) return;
	memset(&o, 0, sizeof(o));
	o.type = OV_SPRITE; o.p = s;
	o.sx = (int16_t)sx; o.sy = (int16_t)sy;
	o.x = (int16_t)x; o.y = (int16_t)y; o.w = (int16_t)w; o.h = (int16_t)h;
	ov_add(&o);
}

void ar_ov_bitmap(const void *bits, int stride, int sx, int sy,
	int x, int y, int w, int h, int c) {
	ov_t o;
	memset(&o, 0, sizeof(o));
	o.type = OV_BITMAP; o.c = (uint8_t)c; o.p = bits;
	o.stride = (int16_t)stride; o.sx = (int16_t)sx; o.sy = (int16_t)sy;
	o.x = (int16_t)x; o.y = (int16_t)y; o.w = (int16_t)w; o.h = (int16_t)h;
	ov_add(&o);
}

void ar_ov_label(const ar_label_t *l, int x, int y, int c) {
	ar_ov_bitmap(l->bits, l->stride, 0, 0, x, y, l->w, l->h, c);
}

/* Where overlays may draw: the whole picture unless the game says
 * otherwise (ar_ov_area()). */
static ar_rect_t ov_area = { 0, 0, AR_W, AR_H };

void ar_ov_area(int x, int y, int w, int h) {
	ov_area.x = (int16_t)x;
	ov_area.y = (int16_t)y;
	ov_area.w = (int16_t)w;
	ov_area.h = (int16_t)h;
	ar__invalidate();
}

static bool ov_same(const ov_t *a, const ov_t *b) {
	return a->type == b->type && a->c == b->c && a->x == b->x &&
		a->y == b->y && a->w == b->w && a->h == b->h && a->p == b->p &&
		a->sx == b->sx && a->sy == b->sy && a->stride == b->stride;
}

static void ov_draw(const ov_t *o) {
	switch (o->type) {
	case OV_FILL:
		ar_fill(o->x, o->y, o->w, o->h, o->c);
		break;
	case OV_SPRITE:
		ar_sprite_part((const ar_sprite_t *)o->p, o->sx, o->sy, o->w, o->h,
			o->x, o->y);
		break;
	case OV_BITMAP:
		ar_bitmap(o->p, o->stride, o->sx, o->sy, o->x, o->y, o->w, o->h,
			o->c);
		break;
	}
}

/* ===================================================================
 * the frame
 * =================================================================== */

#define MAX_REG 96

static ar_rect_t reg[MAX_REG];   /* what was (re)drawn this frame */
static int n_reg;
static bool reg_all;             /* too much to list: assume everything */

static void reg_add(int x, int y, int w, int h) {
	if (reg_all) return;
	if (n_reg >= MAX_REG) { reg_all = true; return; }
	reg[n_reg].x = (int16_t)x; reg[n_reg].y = (int16_t)y;
	reg[n_reg].w = (int16_t)w; reg[n_reg].h = (int16_t)h;
	n_reg++;
}

static bool reg_hits(int x, int y, int w, int h) {
	int i;
	if (reg_all) return true;
	for (i = 0; i < n_reg; i++) {
		const ar_rect_t *r = &reg[i];
		if (x < r->x + r->w && r->x < x + w && y < r->y + r->h &&
		    r->y < y + h)
			return true;
	}
	return false;
}

void ar__invalidate(void) {
	ar_dirty_all();
	ov_forget = true;
}

#ifdef AR_HOST_TESTS
/* How long a moving overlay is off the glass: blitter operations from
 * the first repaint under it to its drawing, the most and the total
 * (tests/ar_host.c reports them). */
extern uint64_t arh_ops;
uint64_t ar__gap_max, ar__gap_sum, ar__gap_n;
static uint64_t run_ops[320];
#endif

/* One dirty run, painted by the game with every draw clipped to it. */
static void paint_rect(const ar_rect_t *r) {
	clip_r = *r;
	if (ar__s.d && ar__s.d->paint)
		ar__s.d->paint(ar__s.d->user, r);
}

#define MAX_RUNS 320
static struct { ar_rect_t r; bool done; } runs[MAX_RUNS];
static int n_runs;
static bool runs_all;
static bool ov_draw_now[AR_MAX_OV];

static bool rects_meet(const ar_rect_t *r, const ov_t *o) {
	return o->x < r->x + r->w && r->x < o->x + o->w &&
		o->y < r->y + r->h && r->y < o->y + o->h;
}

static bool runs_hit(int x, int y, int w, int h) {
	int k;
	if (runs_all) return true;
	for (k = 0; k < n_runs; k++) {
		const ar_rect_t *r = &runs[k].r;
		if (x < r->x + r->w && r->x < x + w && y < r->y + r->h && r->y < y + h)
			return true;
	}
	return false;
}

void ar__render(void) {

	int i, k, ty;

	/* Frozen (wm is dragging a window over us): nothing would reach
	 * the glass. Keep everything dirty that is dirty, and last frame's
	 * overlays as the truth about the screen, for the frame after. */
	if (ar__s.targets_stale) arh_targets();
	if (!ar__s.can_draw) {
		n_cur = 0;
		return;
	}

	/* 1. Overlays that moved, changed or went away: where they WERE
	 *    is dirty. Compared in order; see the note at the top. */
	if (!ov_forget) {
		for (i = 0; i < n_prev; i++)
			if (i >= n_cur || !ov_same(&ov_prev[i], &ov_cur[i]))
				ar_dirty(ov_prev[i].x, ov_prev[i].y, ov_prev[i].w,
					ov_prev[i].h);
	}

	n_reg = 0;
	reg_all = false;

	/* The game's copies, before anything is painted over them. */
	if (ar__s.d && ar__s.d->before_paint && !ar__s.verifying) {
		clip_r.x = 0; clip_r.y = 0; clip_r.w = AR_W; clip_r.h = AR_H;
		ar__s.d->before_paint(ar__s.d->user);
	}

	/* 2. The dirty tiles, as horizontal runs merged downward while the
	 *    run below is identical. Collected first, painted below. */
	n_runs = 0;
	runs_all = false;
	for (ty = 0; ty < AR_TH; ty++) {
		uint64_t row = dirty_rows[ty];
		while (row) {
			int tx0 = __builtin_ctzll(row);
			int tx1 = tx0;
			uint64_t run;
			int ty1 = ty;
			while (tx1 + 1 < AR_TW && (row >> (tx1 + 1)) & 1ull) tx1++;
			run = ((tx1 - tx0 + 1) >= 64) ? ~0ull
				: (((1ull << (tx1 - tx0 + 1)) - 1ull) << tx0);
			while (ty1 + 1 < AR_TH &&
			       (dirty_rows[ty1 + 1] & run) == run)
				ty1++;
			{
				int k;
				ar_rect_t r;
				for (k = ty; k <= ty1; k++) dirty_rows[k] &= ~run;
				row &= ~run;
				r.x = (int16_t)(tx0 * AR_TILE);
				r.y = (int16_t)(ty * AR_TILE);
				r.w = (int16_t)((tx1 - tx0 + 1) * AR_TILE);
				r.h = (int16_t)((ty1 - ty + 1) * AR_TILE);
				if (n_runs < MAX_RUNS) {
					runs[n_runs].r = r;
					runs[n_runs].done = false;
					n_runs++;
				} else {
					/* too many to keep: paint it now, and assume
					 * every overlay is under something painted */
					paint_rect(&r);
					runs_all = true;
				}
			}
		}
	}

	/* 3. Which overlays get drawn: the ones that are new or changed,
	 *    the ones a dirty run is under, and -- in submission order --
	 *    any that overlap one already chosen, so the order on screen
	 *    stays the order they were submitted in. */
	n_reg = 0;
	reg_all = runs_all;
	for (i = 0; i < n_cur; i++) {
		const ov_t *o = &ov_cur[i];
		bool changed = ov_forget || i >= n_prev ||
			!ov_same(&ov_prev[i], o);
		ov_draw_now[i] = changed || runs_hit(o->x, o->y, o->w, o->h) ||
			reg_hits(o->x, o->y, o->w, o->h);
		if (ov_draw_now[i]) reg_add(o->x, o->y, o->w, o->h);
	}

	/* 4. Paint and draw in an order that keeps each overlay's absence
	 *    short. The picture is single-buffered: between repainting the
	 *    tiles under a moving sprite and drawing it again, the sprite
	 *    is not on the screen, and if the beam passes in that moment
	 *    it is missing for a frame -- a ball or a snake's head
	 *    blinking out. So: first every run no overlay is drawn over,
	 *    then, overlay by overlay in submission order, the runs under
	 *    it followed at once by the overlay itself. Every run under an
	 *    overlay is still painted before it is drawn, and overlapping
	 *    overlays still land in submission order, so the picture is
	 *    exactly what painting everything first would give. */
	for (k = 0; k < n_runs; k++) {
		bool under = false;
		for (i = 0; i < n_cur && !under; i++)
			if (ov_draw_now[i] && rects_meet(&runs[k].r, &ov_cur[i]))
				under = true;
		if (!under) {
			paint_rect(&runs[k].r);
			runs[k].done = true;
		}
#ifdef AR_HOST_TESTS
		run_ops[k] = arh_ops;
#endif
	}
	for (i = 0; i < n_cur; i++) {
		const ov_t *o = &ov_cur[i];
		if (!ov_draw_now[i]) continue;
		for (k = 0; k < n_runs; k++)
			if (!runs[k].done && rects_meet(&runs[k].r, o)) {
#ifdef AR_HOST_TESTS
				run_ops[k] = arh_ops;
#endif
				paint_rect(&runs[k].r);
				runs[k].done = true;
			}
		clip_r = ov_area;
#ifdef AR_HOST_TESTS
		{
			uint64_t first = arh_ops, gap;
			bool any = false;
			for (k = 0; k < n_runs; k++)
				if (rects_meet(&runs[k].r, o) && run_ops[k] < first) {
					first = run_ops[k];
					any = true;
				}
			if (any && !ar__s.verifying) {
				gap = arh_ops - first;
				if (gap > ar__gap_max) ar__gap_max = gap;
				ar__gap_sum += gap;
				ar__gap_n++;
			}
		}
#endif
		ov_draw(o);
	}
	/* (none left, but a run under nothing drawn would be) */
	for (k = 0; k < n_runs; k++)
		if (!runs[k].done) paint_rect(&runs[k].r);

	clip_r.x = 0; clip_r.y = 0; clip_r.w = AR_W; clip_r.h = AR_H;

	/* 4. This frame's list becomes last frame's. */
	{
		ov_t *t = ov_prev;
		ov_prev = ov_cur;
		ov_cur = t;
		n_prev = n_cur;
		ov_forget = false;
	}
	n_cur = 0;
}

/* ===================================================================
 * copies
 * =================================================================== */

bool ar_copy(int sx, int sy, int dx, int dy, int w, int h) {

	int k, n;
	int fsx, fsy, fdx, fdy;

	if (ar__s.targets_stale) arh_targets();
	if (!ar__s.can_draw) return false;
	if (ar__s.mode == AR_MODE_WINDOW && !ar__s.vis_whole) return false;

	/* Both rectangles inside the play area: a copy is never clipped,
	 * because a clipped copy is the case the blitter gets wrong
	 * (docs/gpu_blitter.md, "Copy and the scissor"). */
	if (w <= 0 || h <= 0) return true;
	if (sx < 0 || sy < 0 || dx < 0 || dy < 0) return false;
	if (sx + w > AR_W || dx + w > AR_W) return false;
	if (sy + h > AR_H || dy + h > AR_H) return false;

	/* Overlapping and moving sideways on the same rows would read
	 * pixels already written. Not needed by anything here. */
	if (dy == sy && dx != sx && (dx < sx + w && sx < dx + w)) return false;

	n = plane_count();
	for (k = 0; k < n; k++) {
		int px, py;
		plane_off(k, &px, &py);
		fsx = ar__s.ox + sx + px; fsy = ar__s.oy + sy + py;
		fdx = ar__s.ox + dx + px; fdy = ar__s.oy + dy + py;

		if (dy > sy && dy < sy + h) {
			/* Down and overlapping: strips of (dy - sy) rows, bottom
			 * to top, so each strip is read before the one below it
			 * is written -- what z_fb_hw_scroll() does. */
			int step = dy - sy;
			int top = h;
			while (top > 0) {
				int hh = top >= step ? step : top;
				top -= hh;
				arh_vcopy(fsx, fsy + top, fdx, fdy + top, w, hh);
			}
		} else {
			arh_vcopy(fsx, fsy, fdx, fdy, w, h);
		}
	}

	return true;
}

/* ===================================================================
 * particles
 * =================================================================== */

typedef struct {
	int32_t x, y, vx, vy;    /* 8.8 */
	int16_t life;
	uint8_t c0, nc, age;
	int16_t g;
} part_t;

static part_t parts[AR_MAX_PARTS];
static int part_next;

/* A direction on the unit circle for each of 16 angles, x 256. */
static const int16_t dir16[16][2] = {
	{ 256, 0 }, { 237, 98 }, { 181, 181 }, { 98, 237 },
	{ 0, 256 }, { -98, 237 }, { -181, 181 }, { -237, 98 },
	{ -256, 0 }, { -237, -98 }, { -181, -181 }, { -98, -237 },
	{ 0, -256 }, { 98, -237 }, { 181, -181 }, { 237, -98 }
};

void ar_parts_burst(int x, int y, int n, int c0, int nc, int speed,
	int life, int gravity) {
	int i;
	if (nc < 1) nc = 1;
	for (i = 0; i < n; i++) {
		part_t *p = &parts[part_next];
		int a = (int)ar_rand_below(16);
		int s = speed / 3 + (int)ar_rand_below((uint32_t)(speed - speed / 3 + 1));
		part_next = (part_next + 1) % AR_MAX_PARTS;
		p->x = x * 256;
		p->y = y * 256;
		p->vx = (dir16[a][0] * s) >> 8;
		p->vy = (dir16[a][1] * s) >> 8;
		p->life = (int16_t)(life / 2 + (int)ar_rand_below((uint32_t)(life / 2 + 1)));
		p->c0 = (uint8_t)c0;
		p->nc = (uint8_t)nc;
		p->age = (uint8_t)ar_rand_below(4);
		p->g = (int16_t)gravity;
	}
}

void ar_parts_frame(uint32_t dt) {
	int i;
	for (i = 0; i < AR_MAX_PARTS; i++) {
		part_t *p = &parts[i];
		uint32_t f;
		int px, py;
		if (p->life <= 0) continue;
		for (f = 0; f < dt && p->life > 0; f++) {
			p->x += p->vx;
			p->y += p->vy;
			p->vy += p->g;
			/* a little drag, so a burst slows into a drift */
			p->vx -= p->vx >> 4;
			p->vy -= p->vy >> 5;
			p->life--;
			p->age++;
		}
		if (p->life <= 0) continue;
		px = p->x >> 8;
		py = p->y >> 8;
		if (px < -2 || py < -2 || px >= AR_W || py >= AR_H) {
			p->life = 0;
			continue;
		}
		ar_ov_fill(px, py, 2, 2, p->c0 + (p->age >> 2) % p->nc);
	}
}

void ar_parts_clear(void) {
	int i;
	for (i = 0; i < AR_MAX_PARTS; i++) parts[i].life = 0;
}

int ar_parts_live(void) {
	int i, n = 0;
	for (i = 0; i < AR_MAX_PARTS; i++) if (parts[i].life > 0) n++;
	return n;
}

/* ===================================================================
 * palette and shake
 * =================================================================== */

void ar_pal(int i, uint16_t rgb) {
	i &= 15;
	rgb &= 0xfff;
	if (ar__s.pal[i] == rgb) return;
	ar__s.pal[i] = rgb;
	ar__s.pal_dirty |= (uint16_t)(1u << i);
}

uint16_t ar_pal_get(int i) {
	return ar__s.pal[i & 15];
}

void ar_pal_load(const uint16_t *rgb, int n) {
	int i;
	for (i = 0; i < n && i < 16; i++) ar_pal(i, rgb[i]);
}

uint16_t ar_rgb_mix(uint16_t a, uint16_t b, int t) {
	int k;
	uint16_t out = 0;
	if (t < 0) t = 0;
	if (t > 256) t = 256;
	for (k = 0; k < 12; k += 4) {
		int ca = (a >> k) & 15, cb = (b >> k) & 15;
		int c = ca + (((cb - ca) * t + 128) >> 8);
		if (c < 0) c = 0;
		if (c > 15) c = 15;
		out |= (uint16_t)(c << k);
	}
	return out;
}

void ar_shake(int amp, int frames) {
	if (amp > 8) amp = 8;
	if (amp < ar__s.shake_amp && ar__s.shake_left > 0) return;
	ar__s.shake_amp = amp;
	ar__s.shake_left = frames;
	ar__s.shake_total = frames > 0 ? frames : 1;
}

void ar__shake_step(void) {
	int a;
	if (ar__s.shake_left <= 0 || ar__s.mode == AR_MODE_WINDOW) {
		ar__s.view_dx = ar__s.view_dy = 0;
		ar__s.shake_left = 0;
		return;
	}
	/* Decays linearly to nothing; the direction is random each frame,
	 * which reads as a jolt rather than a wobble. */
	a = (ar__s.shake_amp * ar__s.shake_left + ar__s.shake_total - 1) /
		ar__s.shake_total;
	ar__s.view_dx = (int)ar_rand_below((uint32_t)(2 * a + 1)) - a;
	ar__s.view_dy = (int)ar_rand_below((uint32_t)(2 * a + 1)) - a;
	ar__s.shake_left--;
}

/* ===================================================================
 * players
 * =================================================================== */

static uint32_t kb_held[AR_PLAYERS], pad_held[AR_PLAYERS];
static uint32_t latch[AR_PLAYERS];

#define EVQ 64
static ar_event_t evq[EVQ];
static int evq_head, evq_tail;

void ar__queue(const ar_event_t *e) {
	int next = (evq_head + 1) % EVQ;
	if (next == evq_tail) return;      /* full: drop, never block */
	evq[evq_head] = *e;
	evq_head = next;
}

bool ar__dequeue(ar_event_t *e) {
	if (evq_tail == evq_head) return false;
	*e = evq[evq_tail];
	evq_tail = (evq_tail + 1) % EVQ;
	return true;
}

static void button_event(int p, uint32_t bit, bool pressed, int source) {
	ar_event_t e;
	memset(&e, 0, sizeof(e));
	e.type = AR_EV_BUTTON;
	e.player = (uint8_t)p;
	e.key = bit;
	e.pressed = pressed;
	e.source = (uint8_t)source;
	ar__queue(&e);
}

/* Keysym -> (player, button). Letters case-folded: Shift held while a
 * key goes down and released before it comes up must not leave the
 * button held. */
static bool key_button(uint32_t k, int *p, uint32_t *bit) {

	/* zkbd.h's named keys, spelled numerically here so this file needs
	 * no Zeitlos header: Z_KEY_NAMED_BASE + 0..3. */
	const uint32_t up = 0x110000u, down = 0x110001u;
	const uint32_t left = 0x110002u, right = 0x110003u;

	if (k >= 'A' && k <= 'Z') k += 'a' - 'A';

	*p = 0;
	if (k == left)  { *bit = AR_BTN_LEFT;  return true; }
	if (k == right) { *bit = AR_BTN_RIGHT; return true; }
	if (k == up)    { *bit = AR_BTN_UP;    return true; }
	if (k == down)  { *bit = AR_BTN_DOWN;  return true; }
	if (k == ' ')   { *bit = AR_BTN_A;     return true; }
	if (k == 8)     { *bit = AR_BTN_B;     return true; }
	if (k == '\r' || k == '\n') { *bit = AR_BTN_START; return true; }

	*p = 1;
	if (k == 'a')  { *bit = AR_BTN_LEFT;  return true; }
	if (k == 'd')  { *bit = AR_BTN_RIGHT; return true; }
	if (k == 'w')  { *bit = AR_BTN_UP;    return true; }
	if (k == 's')  { *bit = AR_BTN_DOWN;  return true; }
	if (k == 'q')  { *bit = AR_BTN_A;     return true; }
	if (k == 'e')  { *bit = AR_BTN_B;     return true; }
	if (k == '\t') { *bit = AR_BTN_START; return true; }

	return false;
}

void ar__key(uint32_t keysym, bool pressed, uint8_t mods) {

	ar_event_t e;
	int p;
	uint32_t bit;

	memset(&e, 0, sizeof(e));
	e.type = AR_EV_KEY;
	e.key = keysym;
	e.pressed = pressed;
	e.mods = mods;
	ar__queue(&e);

	if (!key_button(keysym, &p, &bit)) return;

	if (pressed) {
		if (!(kb_held[p] & bit)) {
			kb_held[p] |= bit;
			latch[p] |= bit;
			if (!(pad_held[p] & bit)) button_event(p, bit, true, 0);
		}
	} else if (kb_held[p] & bit) {
		kb_held[p] &= ~bit;
		if (!(pad_held[p] & bit)) button_event(p, bit, false, 0);
	}
}

void ar__frame_input(void) {
	int p;
	for (p = 0; p < AR_PLAYERS; p++) {
		uint32_t now = arh_pad(p);
		uint32_t was = pad_held[p];
		uint32_t b;
		latch[p] = 0;
		for (b = 1; b <= AR_BTN_SELECT; b <<= 1) {
			bool n = (now & b) != 0, o = (was & b) != 0;
			if (n == o) continue;
			if (n) latch[p] |= b;
			/* An event only when the combined state changes: a key
			 * and a pad both holding LEFT is one LEFT. */
			if (!(kb_held[p] & b)) button_event(p, b, n, 1);
		}
		pad_held[p] = now;
	}
}

uint32_t ar_held(int p) {
	if (p < 0 || p >= AR_PLAYERS) return 0;
	return kb_held[p] | pad_held[p];
}

uint32_t ar_pressed(int p) {
	if (p < 0 || p >= AR_PLAYERS) return 0;
	return latch[p];
}

uint32_t ar_any_held(void) { return ar_held(0) | ar_held(1); }
uint32_t ar_any_pressed(void) { return ar_pressed(0) | ar_pressed(1); }

bool ar_pad_present(int p) { return arh_pad_present(p); }

bool ar_repeat(ar_repeat_t *r, bool held, int delay, int rate) {
	if (!held) {
		r->was = false;
		r->count = 0;
		return false;
	}
	if (!r->was) {
		r->was = true;
		r->count = 0;
		return true;
	}
	r->count++;
	if (r->count < delay) return false;
	if (rate <= 1) return true;
	return ((r->count - delay) % rate) == 0;
}

/* ===================================================================
 * random numbers
 * =================================================================== */

static uint32_t rng = 0x2545f491u;

void ar_seed(uint32_t s) {
	rng = s ? s : 0x2545f491u;
}

uint32_t ar_rand(void) {
	uint32_t x = rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng = x;
	return x;
}

/* Multiply-shift rather than %: no divide on a core that may not have
 * one, and no bias worth the name for game-sized n. */
uint32_t ar_rand_below(uint32_t n) {
	if (n == 0) return 0;
	return (uint32_t)(((uint64_t)ar_rand() * n) >> 32);
}

void ar_utoa(char *out, uint32_t v, int width, char pad) {
	char tmp[12];
	int n = 0, i;
	do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 11);
	for (i = n; i < width; i++) *out++ = pad;
	while (n) *out++ = tmp[--n];
	*out = 0;
}

/* ===================================================================
 * scores
 * =================================================================== */

static const char *parse_uint(const char *s, uint32_t *v, bool *ok) {
	uint32_t n = 0;
	bool any = false;
	while (*s == ' ' || *s == '\t') s++;
	while (*s >= '0' && *s <= '9') {
		n = n * 10 + (uint32_t)(*s - '0');
		s++;
		any = true;
	}
	*v = n;
	*ok = any;
	return s;
}

int ar_scores_rank(const ar_table_t *t, uint32_t score) {
	int i;
	if (score == 0) return -1;
	for (i = 0; i < t->n; i++)
		if (score > t->e[i].score) return i;
	return t->n < AR_SCORES ? t->n : -1;
}

int ar_scores_insert(ar_table_t *t, uint32_t score, const char *who) {
	int r = ar_scores_rank(t, score), i;
	if (r < 0) return -1;
	if (t->n < AR_SCORES) t->n++;
	for (i = t->n - 1; i > r; i--) t->e[i] = t->e[i - 1];
	t->e[r].score = score;
	for (i = 0; i < 3; i++) {
		char ch = who && who[i] ? who[i] : '-';
		if (ch < ' ' || ch > '~' || ch == ' ') ch = '-';
		t->e[r].who[i] = ch;
		if (who && !who[i]) who = NULL;
	}
	t->e[r].who[3] = 0;
	return r;
}

int ar_scores_parse(const char *text, int mode, ar_table_t *t) {

	const char *s = text;

	t->n = 0;

	while (s && *s) {

		uint32_t m, sc;
		bool ok1, ok2;
		const char *eol = s;
		char who[4] = "---";
		int i;

		while (*eol && *eol != '\n') eol++;

		s = parse_uint(s, &m, &ok1);
		s = parse_uint(s, &sc, &ok2);
		while (*s == ' ' || *s == '\t') s++;
		for (i = 0; i < 3 && s < eol && *s > ' '; i++) who[i] = *s++;

		if (ok1 && ok2 && (int)m == mode) ar_scores_insert(t, sc, who);

		s = *eol ? eol + 1 : eol;
	}

	return t->n;
}

/* The other modes' lines from old_text, then this mode's table. */
int ar_scores_format(char *out, int cap, const char *old_text, int mode,
	const ar_table_t *t) {

	int n = 0, i;
	const char *s = old_text;

	while (s && *s) {
		const char *eol = s;
		uint32_t m;
		bool ok;
		while (*eol && *eol != '\n') eol++;
		parse_uint(s, &m, &ok);
		if (ok && (int)m != mode) {
			int len = (int)(eol - s);
			if (n + len + 1 >= cap) return -1;
			memcpy(out + n, s, (size_t)len);
			n += len;
			out[n++] = '\n';
		}
		s = *eol ? eol + 1 : eol;
	}

	for (i = 0; i < t->n; i++) {
		char a[12], b[12];
		int la, lb;
		ar_utoa(a, (uint32_t)mode, 0, ' ');
		ar_utoa(b, t->e[i].score, 0, ' ');
		la = (int)strlen(a);
		lb = (int)strlen(b);
		if (n + la + lb + 7 >= cap) return -1;
		memcpy(out + n, a, (size_t)la); n += la;
		out[n++] = ' ';
		memcpy(out + n, b, (size_t)lb); n += lb;
		out[n++] = ' ';
		memcpy(out + n, t->e[i].who, 3); n += 3;
		out[n++] = '\n';
	}

	out[n] = 0;
	return n;
}

#define SCORE_FILE_MAX 2048

void ar_scores_load(int mode, ar_table_t *t) {
	static char buf[SCORE_FILE_MAX];
	int n = arh_read_scores(buf, SCORE_FILE_MAX - 1);
	if (n < 0) n = 0;
	buf[n] = 0;
	ar_scores_parse(buf, mode, t);
}

bool ar_scores_save(int mode, const ar_table_t *t) {
	static char old[SCORE_FILE_MAX], out[SCORE_FILE_MAX];
	int n = arh_read_scores(old, SCORE_FILE_MAX - 1);
	if (n < 0) n = 0;
	old[n] = 0;
	n = ar_scores_format(out, SCORE_FILE_MAX, old, mode, t);
	if (n < 0) return false;
	return arh_write_scores(out, n);
}

/* ===================================================================
 * skill
 * =================================================================== */

static int skill = AR_SKILL_EASY;
static const char *const skill_words[AR_SKILLS] = { "easy", "normal", "hard" };
static const char *const skill_names[AR_SKILLS] = { "EASY", "NORMAL", "HARD" };

static const char *const skill_spoken[AR_SKILLS] = { "Easy", "Normal", "Hard" };

int ar_skill(void) { return skill; }

const char *ar_skill_spoken(int s) {
	return (s >= 0 && s < AR_SKILLS) ? skill_spoken[s] : "";
}

const char *ar_skill_name(int s) {
	return (s >= 0 && s < AR_SKILLS) ? skill_names[s] : "?";
}

int ar_skill_table(int mode) {
	mode &= 15;
	if (skill == AR_SKILL_EASY) return mode + 16;
	if (skill == AR_SKILL_HARD) return mode + 32;
	return mode;
}

/* A word, up to the first space, tab, CR or NL, ignoring case. */
static bool word_is(const char *s, const char *w) {
	while (*w) {
		char c = *s++;
		if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
		if (c != *w++) return false;
	}
	return *s == 0 || *s == ' ' || *s == '\t' || *s == '\r' || *s == '\n';
}

int ar_skill_word(const char *w) {
	int i;
	if (!w) return -1;
	while (*w == ' ' || *w == '\t') w++;
	for (i = 0; i < AR_SKILLS; i++)
		if (word_is(w, skill_words[i])) return i;
	return -1;
}

static bool line_is_skill(const char *s) {
	while (*s == ' ' || *s == '\t') s++;
	return word_is(s, "skill");
}

int ar_skill_parse(const char *text) {
	const char *s = text;
	int found = -1;
	while (s && *s) {
		const char *eol = s;
		while (*eol && *eol != '\n') eol++;
		if (line_is_skill(s)) {
			const char *v = s;
			while (*v == ' ' || *v == '\t') v++;
			v += 5;
			found = ar_skill_word(v);
		}
		s = *eol ? eol + 1 : eol;
	}
	return found;
}

int ar_skill_format(char *out, int cap, const char *old, int sk) {
	int n = 0;
	const char *s = old;
	const char *w;
	int lw;
	if (sk < 0 || sk >= AR_SKILLS) sk = AR_SKILL_EASY;
	while (s && *s) {
		const char *eol = s;
		while (*eol && *eol != '\n') eol++;
		if (!line_is_skill(s) && eol > s) {
			int len = (int)(eol - s);
			if (n + len + 1 >= cap) return -1;
			memcpy(out + n, s, (size_t)len);
			n += len;
			out[n++] = '\n';
		}
		s = *eol ? eol + 1 : eol;
	}
	w = skill_words[sk];
	lw = (int)strlen(w);
	if (n + 6 + lw + 1 >= cap) return -1;
	memcpy(out + n, "skill ", 6); n += 6;
	memcpy(out + n, w, (size_t)lw); n += lw;
	out[n++] = '\n';
	out[n] = 0;
	return n;
}

#define SETTINGS_MAX 512

void ar__skill_load(void) {
	static char buf[SETTINGS_MAX];
	int n = arh_read_settings(buf, SETTINGS_MAX - 1), s = -1;
	if (n > 0) {
		buf[n] = 0;
		s = ar_skill_parse(buf);
	}
	if (s < 0) s = arh_cfg_skill();
	skill = (s >= 0 && s < AR_SKILLS) ? s : AR_SKILL_EASY;
}

void ar_skill_set(int s) {
	static char old[SETTINGS_MAX], out[SETTINGS_MAX];
	int n;
	if (s < 0 || s >= AR_SKILLS) return;
	skill = s;
	n = arh_read_settings(old, SETTINGS_MAX - 1);
	if (n < 0) n = 0;
	old[n] = 0;
	n = ar_skill_format(out, SETTINGS_MAX, old, s);
	if (n > 0) arh_write_settings(out, n);
}

void ar_say(const char *text) {
	if (text && *text) arh_say(text);
}

/* ===================================================================
 * setup
 * =================================================================== */

void ar__core_init(const ar_desc_t *d) {
	ov_area.x = 0; ov_area.y = 0; ov_area.w = AR_W; ov_area.h = AR_H;
	int i;
	memset(&ar__s, 0, sizeof(ar__s));
	ar__s.d = d;
	ar__s.mode = AR_MODE_WINDOW;
	ar__s.want_color = !d->prefer_mono;
	for (i = 0; i < 16; i++) ar__s.pal[i] = d->palette[i] & 0xfff;
	ar__s.pal_dirty = 0xffff;
	n_cur = n_prev = 0;
	ov_forget = true;
	pool_used = 0;
	evq_head = evq_tail = 0;
	for (i = 0; i < AR_PLAYERS; i++) kb_held[i] = pad_held[i] = latch[i] = 0;
	ar_dirty_all();
	ar__skill_load();
}

uint32_t ar_ticks(void) { return ar__s.ticks; }
void ar_text_input(bool on) { ar__s.text_input = on; }
ar_mode_t ar_mode(void) { return ar__s.mode; }
bool ar_fullscreen(void) { return ar__s.mode != AR_MODE_WINDOW; }
bool ar_color(void) { return ar__s.mode == AR_MODE_FULL_COLOR; }

#ifdef AR_HOST_TESTS
/* tests/test_zarcade.c: submit again exactly the overlays of the frame
 * just drawn, so a full repaint can be compared with it. */
void ar__test_resubmit(void) {
	int i;
	n_cur = 0;
	for (i = 0; i < n_prev; i++) ov_cur[n_cur++] = ov_prev[i];
}
#endif
#ifdef AR_HOST_DEBUG
#include <stdio.h>
void ar__test_dump(void) {
	int i;
	for (i = 0; i < n_prev; i++)
		printf("  ov %d: type %d c %d at %d,%d %dx%d\n", i, ov_prev[i].type,
			ov_prev[i].c, ov_prev[i].x, ov_prev[i].y, ov_prev[i].w, ov_prev[i].h);
}
#endif
