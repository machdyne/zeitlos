/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zcanvas -- see zcanvas.h and docs/zcanvas.md.
 */

#include "zcanvas.h"

#include <string.h>

// -- trigonometry ------------------------------------------------------

// sin over a quarter turn (256 steps of the 1024-step turn), Q14.
static const int16_t sin_q[257] = {
	0, 101, 201, 302, 402, 503, 603, 704, 804, 904, 1005, 1105,
	1205, 1306, 1406, 1506, 1606, 1706, 1806, 1906, 2006, 2105, 2205, 2305,
	2404, 2503, 2603, 2702, 2801, 2900, 2999, 3098, 3196, 3295, 3393, 3492,
	3590, 3688, 3786, 3883, 3981, 4078, 4176, 4273, 4370, 4467, 4563, 4660,
	4756, 4852, 4948, 5044, 5139, 5235, 5330, 5425, 5520, 5614, 5708, 5803,
	5897, 5990, 6084, 6177, 6270, 6363, 6455, 6547, 6639, 6731, 6823, 6914,
	7005, 7096, 7186, 7276, 7366, 7456, 7545, 7635, 7723, 7812, 7900, 7988,
	8076, 8163, 8250, 8337, 8423, 8509, 8595, 8680, 8765, 8850, 8935, 9019,
	9102, 9186, 9269, 9352, 9434, 9516, 9598, 9679, 9760, 9841, 9921, 10001,
	10080, 10159, 10238, 10316, 10394, 10471, 10549, 10625, 10702, 10778, 10853, 10928,
	11003, 11077, 11151, 11224, 11297, 11370, 11442, 11514, 11585, 11656, 11727, 11797,
	11866, 11935, 12004, 12072, 12140, 12207, 12274, 12340, 12406, 12472, 12537, 12601,
	12665, 12729, 12792, 12854, 12916, 12978, 13039, 13100, 13160, 13219, 13279, 13337,
	13395, 13453, 13510, 13567, 13623, 13678, 13733, 13788, 13842, 13896, 13949, 14001,
	14053, 14104, 14155, 14206, 14256, 14305, 14354, 14402, 14449, 14497, 14543, 14589,
	14635, 14680, 14724, 14768, 14811, 14854, 14896, 14937, 14978, 15019, 15059, 15098,
	15137, 15175, 15213, 15250, 15286, 15322, 15357, 15392, 15426, 15460, 15493, 15525,
	15557, 15588, 15619, 15649, 15679, 15707, 15736, 15763, 15791, 15817, 15843, 15868,
	15893, 15917, 15941, 15964, 15986, 16008, 16029, 16049, 16069, 16088, 16107, 16125,
	16143, 16160, 16176, 16192, 16207, 16221, 16235, 16248, 16261, 16273, 16284, 16295,
	16305, 16315, 16324, 16332, 16340, 16347, 16353, 16359, 16364, 16369, 16373, 16376,
	16379, 16381, 16383, 16384, 16384
};

int zc_sin(int a) {
	a &= 1023;
	if (a < 256) return sin_q[a];
	if (a < 512) return sin_q[512 - a];
	if (a < 768) return -sin_q[a - 512];
	return -sin_q[1024 - a];
}

int zc_cos(int a) { return zc_sin(a + 256); }

static uint32_t isqrt32(uint32_t v) {
	uint32_t r = 0, bit = 1u << 30;
	while (bit > v) bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return r;
}

uint32_t zc_isqrt64(uint64_t v) {
	uint64_t r = 0, bit = (uint64_t)1 << 62;
	if (v <= 0xFFFFFFFFu) return isqrt32((uint32_t)v);
	while (bit > v) bit >>= 2;
	while (bit) {
		if (v >= r + bit) {
			v -= r + bit;
			r = (r >> 1) + bit;
		} else {
			r >>= 1;
		}
		bit >>= 2;
	}
	return (uint32_t)r;
}

// -- targets -----------------------------------------------------------

static void target_common(zc_target_t *t, int fmt, void *pix, int stride,
	int w, int h) {
	memset(t, 0, sizeof(*t));
	t->fmt = (uint8_t)fmt;
	t->pat_scale = 1;
	t->pix = pix;
	t->stride = stride;
	t->w = w;
	t->h = h;
	zc_clip_reset(t);
}

void zc_target_bits(zc_target_t *t, uint32_t *bits, int wpl, int w, int h) {
	target_common(t, ZC_BITS, bits, wpl, w, h);
}

void zc_target_gray(zc_target_t *t, uint8_t *pix, int stride, int w, int h) {
	target_common(t, ZC_GRAY, pix, stride, w, h);
}

void zc_target_rec(zc_target_t *t, const zc_rec_ops_t *ops, void *ctx, int w, int h) {
	target_common(t, ZC_REC, NULL, 0, w, h);
	t->rec = ops;
	t->rec_ctx = ctx;
}

void zc_target_origin(zc_target_t *t, int ox, int oy) {
	t->ox = ox;
	t->oy = oy;
	zc_clip_reset(t);
}

void zc_clip_reset(zc_target_t *t) {
	t->cx0 = t->ox;
	t->cy0 = t->oy;
	t->cx1 = t->ox + t->w;
	t->cy1 = t->oy + t->h;
}

void zc_clip(zc_target_t *t, int x0, int y0, int x1, int y1) {
	if (x0 > t->cx0) t->cx0 = x0;
	if (y0 > t->cy0) t->cy0 = y0;
	if (x1 < t->cx1) t->cx1 = x1;
	if (y1 < t->cy1) t->cy1 = y1;
	if (t->cx1 < t->cx0) t->cx1 = t->cx0;
	if (t->cy1 < t->cy0) t->cy1 = t->cy0;
}

int zc_get_bit(const zc_target_t *t, int x, int y) {
	int bx = x - t->ox, by = y - t->oy;
	if (t->fmt != ZC_BITS || bx < 0 || by < 0 || bx >= t->w || by >= t->h)
		return -1;
	const uint32_t *row = (const uint32_t *)t->pix + (size_t)by * (size_t)t->stride;
	return (int)((row[bx >> 5] >> (bx & 31)) & 1u);
}

// -- paint -------------------------------------------------------------

zc_paint_t zc_pattern(const uint8_t pat[8], int tone, bool opaque) {
	zc_paint_t p;
	p.kind = ZC_P_PATTERN;
	p.tone = (uint8_t)(tone < 0 ? 0 : tone > 16 ? 16 : tone);
	p.opaque = opaque ? 1 : 0;
	memcpy(p.pat, pat, 8);
	return p;
}

// The blitter's 4x4 Bayer matrix (rtl/gpu/gpu_blit.v): a pixel is dark
// where its threshold is below the tone.
static const uint8_t bayer4[4][4] = {
	{  0,  8,  2, 10 },
	{ 12,  4, 14,  6 },
	{  3, 11,  1,  9 },
	{ 15,  7, 13,  5 },
};

static inline int floordiv(int a, int b) {
	int q = a / b;
	if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
	return q;
}

static inline int mod_pos(int a, int m) {
	int r = a % m;
	return r < 0 ? r + m : r;
}

static inline uint8_t tone_gray(int tone) {
	return (uint8_t)(255 - (tone * 255 + 8) / 16);
}

// Is device pixel (x, y) dark for a tone, by the shade matrix?
static inline bool tone_dark(const zc_target_t *t, int tone, int x, int y) {
	int ps = t->pat_scale;
	int c, r;
	if (ps <= 1) { c = (x - t->px) & 3; r = (y - t->py) & 3; }
	else { c = mod_pos(floordiv(x - t->px, ps), 4); r = mod_pos(floordiv(y - t->py, ps), 4); }
	return bayer4[r][c] < tone;
}

static inline bool pat_ink(const zc_target_t *t, const uint8_t *pat, int x, int y) {
	int ps = t->pat_scale;
	int c, r;
	if (ps <= 1) { c = (x - t->px) & 7; r = (y - t->py) & 7; }
	else { c = mod_pos(floordiv(x - t->px, ps), 8); r = mod_pos(floordiv(y - t->py, ps), 8); }
	return (pat[r] >> (7 - c)) & 1;
}

static inline uint32_t rep8(uint32_t b) { b &= 0xFF; b |= b << 8; return b | (b << 16); }
static inline uint32_t rep4(uint32_t b) { b &= 0xF; b |= b << 4; return rep8(b); }

// At one device pixel per cell: the 32-bit masks of a row, starting at
// device x0, without a loop over the bits. dark: pixels the tone darkens;
// ink: pixels the pattern sets.
static uint32_t dark_word(const zc_target_t *t, int tone, int x0, int y) {
	int r = (y - t->py) & 3, k = (x0 - t->px) & 3, c;
	uint32_t d4 = 0, rot;
	for (c = 0; c < 4; c++) if (bayer4[r][c] < tone) d4 |= 1u << c;
	// bit i of the word is column (k + i) & 3
	rot = ((d4 >> k) | (d4 << (4 - k))) & 0xF;
	return rep4(rot);
}

static uint32_t ink_word(const zc_target_t *t, const uint8_t *pat, int x0, int y) {
	uint32_t b = pat[(y - t->py) & 7], rev = 0, rot;
	int k = (x0 - t->px) & 7, i;
	for (i = 0; i < 8; i++) if (b & (0x80u >> i)) rev |= 1u << i;	// bit i = column i
	rot = ((rev >> k) | (rev << (8 - k))) & 0xFF;
	return rep8(rot);
}

// For a 1bpp row: V = the bits to write (1 white), M = which bits the
// paint touches, for the 32 pixels starting at device x0.
static void word_vm(const zc_target_t *t, const zc_paint_t *p, int x0, int y,
	uint32_t *V, uint32_t *M) {
	uint32_t v = 0, m = 0;
	int i;
	if (t->pat_scale <= 1 && p->kind == ZC_P_SHADE) {
		*M = 0xFFFFFFFFu;
		*V = p->tone == 0 ? 0xFFFFFFFFu : p->tone >= 16 ? 0 : ~dark_word(t, p->tone, x0, y);
		return;
	}
	if (t->pat_scale <= 1 && p->kind == ZC_P_PATTERN) {
		uint32_t ink = ink_word(t, p->pat, x0, y);
		uint32_t dark = p->tone >= 16 ? 0xFFFFFFFFu : p->tone == 0 ? 0 :
			dark_word(t, p->tone, x0, y);
		*M = p->opaque ? 0xFFFFFFFFu : ink;
		*V = ~(ink & dark);
		return;
	}
	switch (p->kind) {
	case ZC_P_SHADE:
		m = 0xFFFFFFFFu;
		if (p->tone == 0) { v = 0xFFFFFFFFu; break; }
		if (p->tone >= 16) { v = 0; break; }
		for (i = 0; i < 32; i++)
			if (!tone_dark(t, p->tone, x0 + i, y)) v |= 1u << i;
		break;
	case ZC_P_PATTERN:
		for (i = 0; i < 32; i++) {
			bool ink = pat_ink(t, p->pat, x0 + i, y);
			if (ink) {
				m |= 1u << i;
				if (!tone_dark(t, p->tone, x0 + i, y)) v |= 1u << i;
			} else if (p->opaque) {
				m |= 1u << i;
				v |= 1u << i;
			}
		}
		break;
	default:
		m = 0xFFFFFFFFu;
		break;
	}
	*V = v;
	*M = m;
}

// Patterns repeat every 8*ps pixels and shades every 4*ps; when that
// divides 32 every word of a row is the same and one computation
// serves the whole span.
static bool word_periodic(const zc_target_t *t, const zc_paint_t *p) {
	int ps = t->pat_scale ? t->pat_scale : 1;
	int period = (p->kind == ZC_P_PATTERN) ? 8 * ps : 4 * ps;
	if (p->kind == ZC_P_SHADE && (p->tone == 0 || p->tone >= 16)) return true;
	return (32 % period) == 0;
}

static void span_bits(zc_target_t *t, int y, int x0, int x1,
	const zc_paint_t *p) {
	uint32_t *row = (uint32_t *)t->pix + (size_t)(y - t->oy) * (size_t)t->stride;
	int b0 = x0 - t->ox, b1 = x1 - t->ox;	// buffer columns [b0, b1)
	int w0 = b0 >> 5, w1 = (b1 - 1) >> 5;
	int w;
	uint32_t V = 0, M = 0xFFFFFFFFu;
	bool per = (p->kind == ZC_P_XOR) || word_periodic(t, p);

	if (p->kind != ZC_P_XOR && per)
		word_vm(t, p, t->ox + w0 * 32, y, &V, &M);

	for (w = w0; w <= w1; w++) {
		uint32_t edge = 0xFFFFFFFFu;
		if (w == w0) edge &= 0xFFFFFFFFu << (b0 & 31);
		if (w == w1 && ((b1 & 31) != 0)) edge &= 0xFFFFFFFFu >> (32 - (b1 & 31));
		if (p->kind == ZC_P_XOR) {
			row[w] ^= edge;
			continue;
		}
		if (!per) word_vm(t, p, t->ox + w * 32, y, &V, &M);
		uint32_t m = M & edge;
		row[w] = (row[w] & ~m) | (V & m);
	}
}

// Bytes of one value, a word at a time where it can: a white band and
// a grey fill are most of what a printed sheet's raster costs to clear.
static void fill_bytes(uint8_t *d, uint8_t v, int n) {
	uint32_t w = v * 0x01010101u;
	while (n > 0 && ((uintptr_t)d & 3)) { *d++ = v; n--; }
	while (n >= 16) {
		uint32_t *q = (uint32_t *)(void *)d;
		q[0] = w; q[1] = w; q[2] = w; q[3] = w;
		d += 16;
		n -= 16;
	}
	while (n >= 4) { *(uint32_t *)(void *)d = w; d += 4; n -= 4; }
	while (n-- > 0) *d++ = v;
}

static void span_gray(zc_target_t *t, int y, int x0, int x1,
	const zc_paint_t *p) {
	uint8_t *row = (uint8_t *)t->pix + (size_t)(y - t->oy) * (size_t)t->stride
		- t->ox;
	int x;
	switch (p->kind) {
	case ZC_P_SHADE:
		fill_bytes(row + x0, tone_gray(p->tone), x1 - x0);
		break;
	case ZC_P_PATTERN: {
		// the row's pattern byte once, then cell by cell along it: a
		// printed pattern is pat_scale device pixels a bit
		uint8_t g = tone_gray(p->tone);
		int ps = t->pat_scale ? t->pat_scale : 1;
		int rr = mod_pos(floordiv(y - t->py, ps), 8);
		uint8_t bits = p->pat[rr];
		int cell = floordiv(x0 - t->px, ps);
		int c = mod_pos(cell, 8);
		int k = (x0 - t->px) - cell * ps;		// pixels into the cell
		x = x0;
		while (x < x1) {
			int run = ps - k;
			int ink = (bits >> (7 - c)) & 1;
			if (run > x1 - x) run = x1 - x;
			if (ink) fill_bytes(row + x, g, run);
			else if (p->opaque) fill_bytes(row + x, 255, run);
			x += run;
			k = 0;
			c = (c + 1) & 7;
		}
		break;
	}
	default:
		for (x = x0; x < x1; x++) row[x] = (uint8_t)(255 - row[x]);
		break;
	}
}

void zc_span(zc_target_t *t, int y, int x0, int x1, const zc_paint_t *p) {
	if (y < t->cy0 || y >= t->cy1) return;
	if (x0 < t->cx0) x0 = t->cx0;
	if (x1 > t->cx1) x1 = t->cx1;
	if (x0 >= x1) return;
	if (t->fmt == ZC_BITS) span_bits(t, y, x0, x1, p);
	else if (t->fmt == ZC_GRAY) span_gray(t, y, x0, x1, p);
	else if (t->rec->rect) t->rec->rect(t, x0, y, x1, y + 1, p);
}

void zc_fill_rect_dev(zc_target_t *t, int x, int y, int w, int h,
	const zc_paint_t *p) {
	int y0 = y, y1 = y + h, r;
	if (y0 < t->cy0) y0 = t->cy0;
	if (y1 > t->cy1) y1 = t->cy1;
	if (t->fmt == ZC_REC) {
		int x0 = x < t->cx0 ? t->cx0 : x, x1 = x + w > t->cx1 ? t->cx1 : x + w;
		if (x0 < x1 && y0 < y1 && t->rec->rect) t->rec->rect(t, x0, y0, x1, y1, p);
		return;
	}
	for (r = y0; r < y1; r++) zc_span(t, r, x, x + w, p);
}

void zc_clear(zc_target_t *t, const zc_paint_t *p) {
	zc_fill_rect_dev(t, t->cx0, t->cy0, t->cx1 - t->cx0, t->cy1 - t->cy0, p);
}

// Plots one device pixel, clipped.
static void plot(zc_target_t *t, int x, int y, const zc_paint_t *p) {
	if (x < t->cx0 || x >= t->cx1 || y < t->cy0 || y >= t->cy1) return;
	if (t->fmt == ZC_REC) {
		if (t->rec->rect) t->rec->rect(t, x, y, x + 1, y + 1, p);
	} else if (t->fmt == ZC_BITS) {
		uint32_t *row = (uint32_t *)t->pix + (size_t)(y - t->oy) * (size_t)t->stride;
		int bx = x - t->ox;
		uint32_t bit = 1u << (bx & 31);
		uint32_t *w = &row[bx >> 5];
		switch (p->kind) {
		case ZC_P_SHADE:
			if (tone_dark(t, p->tone, x, y)) *w &= ~bit; else *w |= bit;
			break;
		case ZC_P_PATTERN:
			if (pat_ink(t, p->pat, x, y)) {
				if (tone_dark(t, p->tone, x, y)) *w &= ~bit; else *w |= bit;
			} else if (p->opaque) {
				*w |= bit;
			}
			break;
		default:
			*w ^= bit;
			break;
		}
	} else {
		span_gray(t, y, x, x + 1, p);
	}
}

// -- transforms --------------------------------------------------------

zc_xf_t zc_xf_identity(void) {
	zc_xf_t m = { ZC_XF_ONE, 0, 0, ZC_XF_ONE, 0, 0 };
	return m;
}

zc_xf_t zc_xf_scale(int32_t s16, zc_fix ox, zc_fix oy) {
	zc_xf_t m = { s16, 0, 0, s16, ox, oy };
	return m;
}

zc_xf_t zc_xf_rot90(int32_t s16, zc_fix ox, zc_fix oy) {
	// (x, y) -> (-y, x) on screen is a quarter turn clockwise
	zc_xf_t m = { 0, s16, -s16, 0, ox, oy };
	return m;
}

static inline int32_t mul16(int32_t a, int32_t b) {
	return (int32_t)(((int64_t)a * (int64_t)b) >> 16);
}

zc_xf_t zc_xf_mul(const zc_xf_t *m, const zc_xf_t *n) {
	zc_xf_t r;
	r.a = mul16(m->a, n->a) + mul16(m->c, n->b);
	r.b = mul16(m->b, n->a) + mul16(m->d, n->b);
	r.c = mul16(m->a, n->c) + mul16(m->c, n->d);
	r.d = mul16(m->b, n->c) + mul16(m->d, n->d);
	r.e = mul16(m->a, n->e) + mul16(m->c, n->f) + m->e;
	r.f = mul16(m->b, n->e) + mul16(m->d, n->f) + m->f;
	return r;
}

void zc_xf_apply(const zc_xf_t *m, zc_fix x, zc_fix y, zc_fix *ox, zc_fix *oy) {
	*ox = (zc_fix)((((int64_t)m->a * x) + ((int64_t)m->c * y)) >> 16) + m->e;
	*oy = (zc_fix)((((int64_t)m->b * x) + ((int64_t)m->d * y)) >> 16) + m->f;
}

int32_t zc_xf_scale_of(const zc_xf_t *m) {
	// the same transform is asked about over and over (every text run,
	// every stroke of a page): remember the last answer
	static int32_t la, lb, lc, ld, lr;
	int64_t det;
	if (m->a == la && m->b == lb && m->c == lc && m->d == ld && lr) return lr;
	if (m->b == 0 && m->c == 0 && m->a == m->d) {
		la = m->a; lb = 0; lc = 0; ld = m->d;
		lr = m->a < 0 ? -m->a : m->a;
		return lr;
	}
	la = m->a; lb = m->b; lc = m->c; ld = m->d;
	det = ((int64_t)m->a * m->d - (int64_t)m->b * m->c);
	if (det < 0) det = -det;
	// det is 32.32; its square root is 16.16
	lr = (int32_t)zc_isqrt64((uint64_t)det);
	return lr;
}

bool zc_xf_is_axis(const zc_xf_t *m) {
	return m->b == 0 && m->c == 0 && m->a > 0 && m->d > 0;
}

// -- paths -------------------------------------------------------------

void zc_path_init(zc_path_t *p, zc_pt_t *pt, int cap, uint16_t *sub, int subcap) {
	memset(p, 0, sizeof(*p));
	p->pt = pt;
	p->cap = cap;
	p->sub = sub;
	p->subcap = subcap;
	p->xf = zc_xf_identity();
}

void zc_path_reset(zc_path_t *p) {
	p->n = 0;
	p->nsub = 0;
	p->overflow = false;
}

void zc_path_set_xf(zc_path_t *p, const zc_xf_t *xf) {
	p->xf = *xf;
}

static void add_dev(zc_path_t *p, zc_fix x, zc_fix y, bool start) {
	if (start) {
		if (p->nsub >= p->subcap || p->n >= 0xFFFF) { p->overflow = true; return; }
		// an empty subpath (two moves in a row) is replaced
		if (p->nsub > 0 && p->sub[p->nsub - 1] == p->n) p->nsub--;
		p->sub[p->nsub++] = (uint16_t)p->n;
	} else if (p->nsub == 0) {
		add_dev(p, x, y, true);
		return;
	}
	if (p->n >= p->cap) { p->overflow = true; return; }
	if (p->n == 0) {
		p->bx0 = p->bx1 = x;
		p->by0 = p->by1 = y;
	} else {
		if (x < p->bx0) p->bx0 = x;
		if (x > p->bx1) p->bx1 = x;
		if (y < p->by0) p->by0 = y;
		if (y > p->by1) p->by1 = y;
	}
	p->pt[p->n].x = x;
	p->pt[p->n].y = y;
	p->n++;
}

void zc_path_move_dev(zc_path_t *p, zc_fix x, zc_fix y) { add_dev(p, x, y, true); }
void zc_path_line_dev(zc_path_t *p, zc_fix x, zc_fix y) { add_dev(p, x, y, false); }

void zc_path_move(zc_path_t *p, zc_fix x, zc_fix y) {
	zc_fix dx, dy;
	zc_xf_apply(&p->xf, x, y, &dx, &dy);
	add_dev(p, dx, dy, true);
	p->cur_x = x;
	p->cur_y = y;
}

void zc_path_line(zc_path_t *p, zc_fix x, zc_fix y) {
	zc_fix dx, dy;
	zc_xf_apply(&p->xf, x, y, &dx, &dy);
	add_dev(p, dx, dy, false);
	p->cur_x = x;
	p->cur_y = y;
}

void zc_path_close(zc_path_t *p) {
	// Fills close every subpath implicitly and a stroke closes them when
	// asked (zc_stroke's `closed`). Kept so path-building code reads
	// like the shape it draws.
	(void)p;
}

// Number of segments for a curve of this device length, 24.8.
static int curve_steps(int64_t len_fix) {
	int px = (int)(len_fix >> ZC_FRAC);
	int n = (int)zc_isqrt64((uint64_t)(px > 0 ? px : 1) * 8u);
	if (n < 2) n = 2;
	if (n > 128) n = 128;
	return n;
}

static int64_t dist_fix(zc_fix ax, zc_fix ay, zc_fix bx, zc_fix by) {
	int64_t dx = (int64_t)bx - ax, dy = (int64_t)by - ay;
	return (int64_t)zc_isqrt64((uint64_t)(dx * dx + dy * dy));
}

void zc_path_cubic(zc_path_t *p, zc_fix x1, zc_fix y1, zc_fix x2, zc_fix y2,
	zc_fix x3, zc_fix y3) {
	zc_fix X[4], Y[4];
	int n, i;
	zc_xf_apply(&p->xf, p->cur_x, p->cur_y, &X[0], &Y[0]);
	zc_xf_apply(&p->xf, x1, y1, &X[1], &Y[1]);
	zc_xf_apply(&p->xf, x2, y2, &X[2], &Y[2]);
	zc_xf_apply(&p->xf, x3, y3, &X[3], &Y[3]);
	n = curve_steps(dist_fix(X[0], Y[0], X[1], Y[1]) +
		dist_fix(X[1], Y[1], X[2], Y[2]) + dist_fix(X[2], Y[2], X[3], Y[3]));
	for (i = 1; i <= n; i++) {
		// t in Q16
		int64_t t = ((int64_t)i << 16) / n, u = 65536 - t;
		int64_t b0 = (u * u >> 16) * u >> 16;
		int64_t b1 = 3 * ((u * u >> 16) * t >> 16);
		int64_t b2 = 3 * ((u * t >> 16) * t >> 16);
		int64_t b3 = (t * t >> 16) * t >> 16;
		zc_fix x = (zc_fix)((b0 * X[0] + b1 * X[1] + b2 * X[2] + b3 * X[3]) >> 16);
		zc_fix y = (zc_fix)((b0 * Y[0] + b1 * Y[1] + b2 * Y[2] + b3 * Y[3]) >> 16);
		add_dev(p, x, y, false);
	}
	p->cur_x = x3;
	p->cur_y = y3;
}

void zc_path_quad(zc_path_t *p, zc_fix x1, zc_fix y1, zc_fix x2, zc_fix y2) {
	// a quadratic is the cubic with control points 2/3 of the way
	zc_fix x0 = p->cur_x, y0 = p->cur_y;
	zc_path_cubic(p,
		x0 + (zc_fix)(((int64_t)(x1 - x0) * 2) / 3),
		y0 + (zc_fix)(((int64_t)(y1 - y0) * 2) / 3),
		x2 + (zc_fix)(((int64_t)(x1 - x2) * 2) / 3),
		y2 + (zc_fix)(((int64_t)(y1 - y2) * 2) / 3),
		x2, y2);
}

// Segments in a full turn of an ellipse whose larger radius is r (user
// units), at this path's scale: about one per 1/8 sqrt(pixels), so the
// error stays under a tenth of a pixel from a 2-pixel dot to a page.
static int ellipse_steps(const zc_path_t *p, zc_fix r) {
	int64_t rdev = ((int64_t)r * zc_xf_scale_of(&p->xf)) >> 16;
	int px = (int)(rdev >> ZC_FRAC);
	int n = 8 * (int)zc_isqrt64((uint64_t)(px > 0 ? px : 1));
	if (n < 16) n = 16;
	if (n > 256) n = 256;
	return (n + 3) & ~3;
}

void zc_path_arc(zc_path_t *p, zc_fix cx, zc_fix cy, zc_fix rx, zc_fix ry,
	int a0, int a1) {
	int full = ellipse_steps(p, rx > ry ? rx : ry);
	int span = a1 - a0;
	int n = (int)(((int64_t)(span < 0 ? -span : span) * full) / 1024);
	int i;
	if (n < 1) n = 1;
	for (i = 0; i <= n; i++) {
		// angle in 1/16384 turn for smoothness, through the 1024 table
		int64_t a = (int64_t)a0 * 16 + ((int64_t)span * 16 * i) / n;
		int ai = (int)(a >> 4), af = (int)(a & 15);
		int c = zc_cos(ai) + (((zc_cos(ai + 1) - zc_cos(ai)) * af) >> 4);
		int s = zc_sin(ai) + (((zc_sin(ai + 1) - zc_sin(ai)) * af) >> 4);
		zc_fix x = cx + (zc_fix)(((int64_t)rx * c) >> 14);
		zc_fix y = cy + (zc_fix)(((int64_t)ry * s) >> 14);
		if (i == 0 && p->nsub == 0) zc_path_move(p, x, y);
		else zc_path_line(p, x, y);
	}
}

void zc_path_rect(zc_path_t *p, zc_fix x, zc_fix y, zc_fix w, zc_fix h) {
	zc_path_move(p, x, y);
	zc_path_line(p, x + w, y);
	zc_path_line(p, x + w, y + h);
	zc_path_line(p, x, y + h);
}

void zc_path_ellipse(zc_path_t *p, zc_fix cx, zc_fix cy, zc_fix rx, zc_fix ry) {
	int n = ellipse_steps(p, rx > ry ? rx : ry);
	int i;
	for (i = 0; i < n; i++) {
		int a = (int)(((int64_t)i * 1024 * 16) / n);
		int ai = a >> 4, af = a & 15;
		int c = zc_cos(ai) + (((zc_cos(ai + 1) - zc_cos(ai)) * af) >> 4);
		int s = zc_sin(ai) + (((zc_sin(ai + 1) - zc_sin(ai)) * af) >> 4);
		zc_fix x = cx + (zc_fix)(((int64_t)rx * c) >> 14);
		zc_fix y = cy + (zc_fix)(((int64_t)ry * s) >> 14);
		if (i == 0) zc_path_move(p, x, y);
		else zc_path_line(p, x, y);
	}
}

void zc_path_rrect(zc_path_t *p, zc_fix x, zc_fix y, zc_fix w, zc_fix h,
	zc_fix r) {
	if (r <= 0) { zc_path_rect(p, x, y, w, h); return; }
	if (r > w / 2) r = w / 2;
	if (r > h / 2) r = h / 2;
	// clockwise on screen: top edge left to right first
	zc_path_move(p, x + r, y);
	zc_path_line(p, x + w - r, y);
	zc_path_arc(p, x + w - r, y + r, r, r, 768, 1024);
	zc_path_line(p, x + w, y + h - r);
	zc_path_arc(p, x + w - r, y + h - r, r, r, 0, 256);
	zc_path_line(p, x + r, y + h);
	zc_path_arc(p, x + r, y + h - r, r, r, 256, 512);
	zc_path_line(p, x, y + r);
	zc_path_arc(p, x + r, y + r, r, r, 512, 768);
}

// -- fill --------------------------------------------------------------

typedef struct {
	int32_t		x;		// 16.16 at the current row's centre
	int32_t		dx;		// 16.16 per row
	int16_t		r0, r1;	// rows [r0, r1)
	int8_t		dir;
	uint8_t		pad[3];
} edge_t;

static edge_t *g_edges;
static uint16_t *g_active;		// edges crossing the current row, by x
static uint16_t *g_order;		// all edges, by first row
static int g_max_edges;

int zc_scratch(void *mem, size_t bytes) {
	size_t per = sizeof(edge_t) + 2 * sizeof(uint16_t);
	g_max_edges = (int)(bytes / per);
	if (g_max_edges > 65535) g_max_edges = 65535;
	g_edges = (edge_t *)mem;
	g_active = (uint16_t *)((uint8_t *)mem + (size_t)g_max_edges * sizeof(edge_t));
	g_order = g_active + g_max_edges;
	return g_max_edges;
}

// First row whose centre is at or below y (24.8).
static inline int row_at(zc_fix y) {
	return (int)((y - ZC_HALF + ZC_ONE - 1) >> ZC_FRAC);
}

static bool add_edge(int *ne, zc_fix x0, zc_fix y0, zc_fix x1, zc_fix y1,
	int ylo, int yhi) {
	int dir = 1, r0, r1;
	edge_t *e;
	if (y0 == y1) return true;
	if (y0 > y1) {
		zc_fix t;
		t = x0; x0 = x1; x1 = t;
		t = y0; y0 = y1; y1 = t;
		dir = -1;
	}
	r0 = row_at(y0);
	r1 = row_at(y1);
	if (r0 >= r1) return true;
	if (r1 <= ylo || r0 >= yhi) return true;	// no row of the clip
	if (*ne >= g_max_edges) return false;
	e = &g_edges[(*ne)++];
	{
		int32_t ddx = x1 - x0, ddy = y1 - y0;
		int64_t dxdy;
		int start = r0 < ylo ? ylo : r0;
		zc_fix yc = (zc_fix)start * ZC_ONE + ZC_HALF;
		// the slope, 16.16: in 32 bits for an edge under 128 pixels wide
		// (nearly all of them: glyph strokes, curves), else in 64
		if (ddx > -32768 && ddx < 32768) dxdy = (ddx * 65536) / ddy;
		else dxdy = ((int64_t)ddx * 65536) / ddy;
		// an edge this flat crosses one row and never steps; keep the
		// step representable anyway
		if (dxdy > 0x7FFFFFFF) dxdy = 0x7FFFFFFF;
		if (dxdy < -0x7FFFFFFF) dxdy = -0x7FFFFFFF;
		e->x = (int32_t)((int64_t)x0 * 256 + (((int64_t)(yc - y0) * dxdy) >> 8));
		e->dx = (int32_t)dxdy;
		e->r0 = (int16_t)start;
		e->r1 = (int16_t)(r1 > yhi ? yhi : r1);
		e->dir = (int8_t)dir;
	}
	return true;
}

// g_order = the edges by first row. Shell sort over 16-bit indices (the
// edges themselves never move).
static void sort_edges(int n) {
	static const int gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
	int gi, i, j;
	for (i = 0; i < n; i++) g_order[i] = (uint16_t)i;
	for (gi = 0; gi < (int)(sizeof(gaps) / sizeof(gaps[0])); gi++) {
		int gap = gaps[gi];
		for (i = gap; i < n; i++) {
			uint16_t v = g_order[i];
			int16_t r = g_edges[v].r0;
			for (j = i; j >= gap && g_edges[g_order[j - gap]].r0 > r; j -= gap)
				g_order[j] = g_order[j - gap];
			g_order[j] = v;
		}
	}
}

bool zc_fill(zc_target_t *t, const zc_path_t *path, int rule,
	const zc_paint_t *p) {
	int ne = 0, s, i;
	int ylo = t->cy0, yhi = t->cy1;
	int na = 0, next = 0, y, ymin;

	if (t->fmt == ZC_REC) {
		if (path->n >= 2 && !zc_clip_empty(t) && t->rec->fill) t->rec->fill(t, path, rule, p);
		return true;
	}
	if (!g_edges || path->n < 2 || zc_clip_empty(t)) return g_edges != NULL;
	if (row_at(path->by1) <= ylo || row_at(path->by0) >= yhi) return true;
	if ((path->bx1 >> ZC_FRAC) < t->cx0 - 1 || (path->bx0 >> ZC_FRAC) > t->cx1 + 1) {
		// entirely left or right of the clip: nothing visible
		return true;
	}

	for (s = 0; s < path->nsub; s++) {
		int a = path->sub[s];
		int b = (s + 1 < path->nsub) ? path->sub[s + 1] : path->n;
		if (b - a < 2) continue;
		for (i = a; i < b; i++) {
			const zc_pt_t *p0 = &path->pt[i];
			const zc_pt_t *p1 = &path->pt[(i + 1 < b) ? i + 1 : a];
			if (!add_edge(&ne, p0->x, p0->y, p1->x, p1->y, ylo, yhi))
				return false;
		}
	}
	if (ne == 0) return true;
	sort_edges(ne);

	ymin = g_edges[g_order[0]].r0;
	for (y = ymin; y < yhi && (next < ne || na > 0); y++) {
		int k, w;

		while (next < ne && g_edges[g_order[next]].r0 == y)
			g_active[na++] = g_order[next++];

		// drop finished edges
		for (k = 0, i = 0; i < na; i++) {
			if (g_edges[g_active[i]].r1 > y) g_active[k++] = g_active[i];
		}
		na = k;
		if (na == 0) {
			if (next < ne) y = g_edges[g_order[next]].r0 - 1;
			continue;
		}

		// insertion sort by x: nearly sorted from the row before
		for (i = 1; i < na; i++) {
			uint16_t v = g_active[i];
			int32_t xv = g_edges[v].x;
			int j = i - 1;
			while (j >= 0 && g_edges[g_active[j]].x > xv) {
				g_active[j + 1] = g_active[j];
				j--;
			}
			g_active[j + 1] = v;
		}

		// spans: walk the crossings, remembering where "inside" began
		w = 0;
		{
			int32_t start = 0;
			for (i = 0; i < na; i++) {
				edge_t *e = &g_edges[g_active[i]];
				int was = w;
				bool in_was, in_now;
				w += (rule == ZC_EVENODD) ? 1 : e->dir;
				in_was = (rule == ZC_EVENODD) ? (was & 1) : (was != 0);
				in_now = (rule == ZC_EVENODD) ? (w & 1) : (w != 0);
				if (!in_was && in_now) start = e->x;
				else if (in_was && !in_now) {
					// pixels whose centre lies in [start, x)
					int c0 = (int)((start - 32768 + 65535) >> 16);
					int c1 = (int)((e->x - 32768 + 65535) >> 16);
					if (c1 > c0) zc_span(t, y, c0, c1, p);
				}
			}
		}

		for (i = 0; i < na; i++) g_edges[g_active[i]].x += g_edges[g_active[i]].dx;
	}
	return true;
}

// -- strokes -----------------------------------------------------------

void zc_line_dev(zc_target_t *t, zc_fix x0, zc_fix y0, zc_fix x1, zc_fix y1,
	const zc_paint_t *p) {
	if (t->fmt == ZC_REC) {
		// a two-point path, stroked as thinly as the device goes
		zc_pt_t pt[2];
		uint16_t sub[1];
		zc_path_t lp;
		if (!t->rec->stroke || zc_clip_empty(t)) return;
		zc_path_init(&lp, pt, 2, sub, 1);
		zc_path_move_dev(&lp, x0, y0);
		zc_path_line_dev(&lp, x1, y1);
		t->rec->stroke(t, &lp, 0, false, p);
		return;
	}
	int ax = x0 >> ZC_FRAC, ay = y0 >> ZC_FRAC;
	int bx = x1 >> ZC_FRAC, by = y1 >> ZC_FRAC;
	int dx = bx > ax ? bx - ax : ax - bx;
	int dy = by > ay ? by - ay : ay - by;
	int sx = ax < bx ? 1 : -1, sy = ay < by ? 1 : -1;
	int err = dx - dy;

	// wholly outside the clip
	if ((ax < t->cx0 && bx < t->cx0) || (ax >= t->cx1 && bx >= t->cx1) ||
		(ay < t->cy0 && by < t->cy0) || (ay >= t->cy1 && by >= t->cy1))
		return;

	if (dy == 0 && p->kind != ZC_P_XOR) {
		zc_span(t, ay, ax < bx ? ax : bx, (ax < bx ? bx : ax) + 1, p);
		return;
	}
	for (;;) {
		plot(t, ax, ay, p);
		if (ax == bx && ay == by) break;
		{
			int e2 = 2 * err;
			if (e2 > -dy) { err -= dy; ax += sx; }
			if (e2 < dx) { err += dx; ay += sy; }
		}
	}
}

// Points on a semicircle: K steps per half turn for a radius of r px.
static int cap_steps(int r_px) {
	// the chord's sag, r(1 - cos(pi/2K)), stays near a tenth of a pixel
	if (r_px <= 2) return 4;
	if (r_px <= 3) return 8;
	if (r_px <= 8) return 12;
	if (r_px <= 20) return 16;
	if (r_px <= 60) return 24;
	return 32;
}

// A capsule (a segment swept by a disc) as one closed subpath, always
// turning the same way, so a set of them filled with nonzero winding is
// their union.
static void add_capsule(zc_path_t *w, zc_fix x0, zc_fix y0, zc_fix x1,
	zc_fix y1, zc_fix r) {
	int64_t dx = (int64_t)x1 - x0, dy = (int64_t)y1 - y0;
	int64_t len = (int64_t)zc_isqrt64((uint64_t)(dx * dx + dy * dy));
	int K = cap_steps(r >> ZC_FRAC);
	int32_t ux, uy, nx, ny;
	int i;

	if (len == 0) {
		ux = 16384; uy = 0;
	} else {
		ux = (int32_t)((dx * 16384) / len);
		uy = (int32_t)((dy * 16384) / len);
	}
	nx = -uy;
	ny = ux;

	// around the end: from +n through +u to -n
	for (i = 0; i <= K; i++) {
		int a = (512 * i) / K;		// half a turn in 1/1024 units
		int c = zc_cos(a), s = zc_sin(a);
		int32_t vx = (int32_t)(((int64_t)nx * c + (int64_t)ux * s) >> 14);
		int32_t vy = (int32_t)(((int64_t)ny * c + (int64_t)uy * s) >> 14);
		zc_fix px = x1 + (zc_fix)(((int64_t)vx * r) >> 14);
		zc_fix py = y1 + (zc_fix)(((int64_t)vy * r) >> 14);
		if (i == 0) zc_path_move_dev(w, px, py);
		else zc_path_line_dev(w, px, py);
	}
	// around the start: from -n through -u to +n
	for (i = 0; i <= K; i++) {
		int a = (512 * i) / K;
		int c = zc_cos(a), s = zc_sin(a);
		int32_t vx = (int32_t)((-(int64_t)nx * c - (int64_t)ux * s) >> 14);
		int32_t vy = (int32_t)((-(int64_t)ny * c - (int64_t)uy * s) >> 14);
		zc_fix px = x0 + (zc_fix)(((int64_t)vx * r) >> 14);
		zc_fix py = y0 + (zc_fix)(((int64_t)vy * r) >> 14);
		zc_path_line_dev(w, px, py);
	}
}

bool zc_stroke(zc_target_t *t, const zc_path_t *path, zc_fix width,
	bool closed, zc_path_t *work, const zc_paint_t *p) {
	int64_t wdev = ((int64_t)width * zc_xf_scale_of(&path->xf)) >> 16;
	zc_fix r = (zc_fix)(wdev / 2);
	int s, i;
	bool ok = true;
	zc_fix pad = r + ZC_ONE;

	if (path->n < 1 || zc_clip_empty(t)) return true;
	if (t->fmt == ZC_REC) {
		if (t->rec->stroke) t->rec->stroke(t, path, (zc_fix)wdev, closed, p);
		return true;
	}
	// nothing of it near the clip
	if ((path->bx1 + pad) < (zc_fix)t->cx0 * ZC_ONE ||
		(path->bx0 - pad) > (zc_fix)t->cx1 * ZC_ONE ||
		(path->by1 + pad) < (zc_fix)t->cy0 * ZC_ONE ||
		(path->by0 - pad) > (zc_fix)t->cy1 * ZC_ONE)
		return true;

	if (wdev <= (ZC_ONE * 5) / 4) {
		for (s = 0; s < path->nsub; s++) {
			int a = path->sub[s];
			int b = (s + 1 < path->nsub) ? path->sub[s + 1] : path->n;
			if (b - a == 1) {
				plot(t, path->pt[a].x >> ZC_FRAC, path->pt[a].y >> ZC_FRAC, p);
				continue;
			}
			for (i = a; i + 1 < b; i++)
				zc_line_dev(t, path->pt[i].x, path->pt[i].y,
					path->pt[i + 1].x, path->pt[i + 1].y, p);
			if (closed && b - a > 2)
				zc_line_dev(t, path->pt[b - 1].x, path->pt[b - 1].y,
					path->pt[a].x, path->pt[a].y, p);
		}
		return true;
	}

	{
		int K = cap_steps(r >> ZC_FRAC);
		int per = 2 * (K + 1);
		zc_path_reset(work);
		for (s = 0; s < path->nsub; s++) {
			int a = path->sub[s];
			int b = (s + 1 < path->nsub) ? path->sub[s + 1] : path->n;
			int last = closed && b - a > 2 ? b : b - 1;
			if (b - a == 1) last = a + 1;
			for (i = a; i < last; i++) {
				const zc_pt_t *p0 = &path->pt[i];
				const zc_pt_t *p1 = (b - a == 1) ? p0 :
					&path->pt[(i + 1 < b) ? i + 1 : a];
				// cull a capsule nowhere near the clip
				zc_fix lx = p0->x < p1->x ? p0->x : p1->x;
				zc_fix hx = p0->x < p1->x ? p1->x : p0->x;
				zc_fix ly = p0->y < p1->y ? p0->y : p1->y;
				zc_fix hy = p0->y < p1->y ? p1->y : p0->y;
				if (hx + pad < (zc_fix)t->cx0 * ZC_ONE ||
					lx - pad > (zc_fix)t->cx1 * ZC_ONE ||
					hy + pad < (zc_fix)t->cy0 * ZC_ONE ||
					ly - pad > (zc_fix)t->cy1 * ZC_ONE)
					continue;
				if (work->n + per > work->cap || work->nsub + 1 > work->subcap) {
					if (!zc_fill(t, work, ZC_NONZERO, p)) ok = false;
					zc_path_reset(work);
				}
				add_capsule(work, p0->x, p0->y, p1->x, p1->y, r);
			}
		}
		if (work->n > 0 && !zc_fill(t, work, ZC_NONZERO, p)) ok = false;
		zc_path_reset(work);
	}
	return ok;
}

// -- frames ------------------------------------------------------------

bool zc_frame_rect(zc_target_t *t, zc_path_t *p, zc_fix x, zc_fix y, zc_fix w,
	zc_fix h, zc_fix width, const zc_paint_t *paint) {
	zc_path_reset(p);
	zc_path_rect(p, x, y, w, h);
	if (2 * width < w && 2 * width < h)
		zc_path_rect(p, x + width, y + width, w - 2 * width, h - 2 * width);
	return zc_fill(t, p, ZC_EVENODD, paint);
}

bool zc_frame_ellipse(zc_target_t *t, zc_path_t *p, zc_fix x, zc_fix y,
	zc_fix w, zc_fix h, zc_fix width, const zc_paint_t *paint) {
	zc_path_reset(p);
	zc_path_ellipse(p, x + w / 2, y + h / 2, w / 2, h / 2);
	if (2 * width < w && 2 * width < h)
		zc_path_ellipse(p, x + w / 2, y + h / 2, w / 2 - width, h / 2 - width);
	return zc_fill(t, p, ZC_EVENODD, paint);
}

bool zc_frame_rrect(zc_target_t *t, zc_path_t *p, zc_fix x, zc_fix y,
	zc_fix w, zc_fix h, zc_fix r, zc_fix width, const zc_paint_t *paint) {
	zc_path_reset(p);
	zc_path_rrect(p, x, y, w, h, r);
	if (2 * width < w && 2 * width < h)
		zc_path_rrect(p, x + width, y + width, w - 2 * width, h - 2 * width,
			r > width ? r - width : 0);
	return zc_fill(t, p, ZC_EVENODD, paint);
}

// -- images ------------------------------------------------------------

static inline int src_bit(const uint32_t *src, int swpl, int x, int y) {
	return (int)((src[(size_t)y * (size_t)swpl + (size_t)(x >> 5)] >> (x & 31)) & 1u);
}

// The device rectangle a blit covers, clipped: rows [*y0, *y1), cols
// [*x0, *x1). False when nothing is visible.
static bool blit_box(const zc_target_t *t, zc_fix dx, zc_fix dy, zc_fix dw,
	zc_fix dh, int *x0, int *y0, int *x1, int *y1) {
	if (dw <= 0 || dh <= 0) return false;
	*x0 = row_at(dx);
	*x1 = row_at(dx + dw);
	*y0 = row_at(dy);
	*y1 = row_at(dy + dh);
	if (*x0 < t->cx0) *x0 = t->cx0;
	if (*y0 < t->cy0) *y0 = t->cy0;
	if (*x1 > t->cx1) *x1 = t->cx1;
	if (*y1 > t->cy1) *y1 = t->cy1;
	return *x0 < *x1 && *y0 < *y1;
}

// Source coordinate (16.16) of device pixel centre c, for a source of
// n pixels drawn over [d, d + dlen).
static inline int64_t src_of(int c, zc_fix d, zc_fix dlen, int n) {
	return (((int64_t)c * ZC_ONE + ZC_HALF - d) * n * 65536) / dlen;
}

// Is this blit 1:1 and on whole pixels? Then rows move as words.
static bool blit_aligned(zc_fix dx, zc_fix dy, zc_fix dw, zc_fix dh, int sw,
	int sh) {
	return (dx & (ZC_ONE - 1)) == 0 && (dy & (ZC_ONE - 1)) == 0 &&
		dw == (zc_fix)sw * ZC_ONE && dh == (zc_fix)sh * ZC_ONE;
}

// 32 source bits starting at source column sx (may be negative or past
// the end; those bits read as 1, white).
static inline uint32_t src_word(const uint32_t *srow, int swpl, int sw, int sx) {
	uint32_t v;
	int wi = sx >> 5, sh = sx & 31;
	uint32_t lo = (wi >= 0 && wi < swpl) ? srow[wi] : 0xFFFFFFFFu;
	uint32_t hi = (wi + 1 >= 0 && wi + 1 < swpl) ? srow[wi + 1] : 0xFFFFFFFFu;
	v = sh ? (lo >> sh) | (hi << (32 - sh)) : lo;
	// bits at or past sw are outside the image
	if (sx + 32 > sw) {
		int keep = sw - sx;
		if (keep <= 0) v = 0xFFFFFFFFu;
		else if (keep < 32) v |= 0xFFFFFFFFu << keep;
	}
	if (sx < 0) {
		int skip = -sx;
		if (skip >= 32) v = 0xFFFFFFFFu;
		else v |= 0xFFFFFFFFu >> (32 - skip);
	}
	return v;
}

static void blit_bits_aligned(zc_target_t *t, const uint32_t *src, int swpl,
	int sw, int ddx, int ddy, int x0, int y0, int x1, int y1, bool mask,
	const zc_paint_t *p) {
	// [x0, x1) is inside the source's own columns (blit_box clipped to
	// the destination rectangle, which is the source's size), so the
	// only bits outside it are in the two end words, and the edge masks
	// remove those.
	int b0 = x0 - t->ox, b1 = x1 - t->ox;
	int w0 = b0 >> 5, w1 = (b1 - 1) >> 5;
	uint32_t e0 = 0xFFFFFFFFu << (b0 & 31);
	uint32_t e1 = (b1 & 31) ? 0xFFFFFFFFu >> (32 - (b1 & 31)) : 0xFFFFFFFFu;
	int sx0 = t->ox + w0 * 32 - ddx;		// source column of the first word's bit 0
	int sh = sx0 & 31, wi0 = sx0 >> 5;
	bool solid = !mask || (p->kind == ZC_P_SHADE && (p->tone == 0 || p->tone >= 16));
	bool xr = mask && p->kind == ZC_P_XOR;
	bool per = !solid && !xr && word_periodic(t, p);
	uint32_t V = 0, M = 0xFFFFFFFFu;
	int y, w;
	(void)sw;
	if (mask && solid) V = p->tone == 0 ? 0xFFFFFFFFu : 0;

	for (y = y0; y < y1; y++) {
		const uint32_t *srow = src + (size_t)(y - ddy) * (size_t)swpl;
		uint32_t *drow = (uint32_t *)t->pix + (size_t)(y - t->oy) * (size_t)t->stride;
		int wi = wi0;
		if (per) word_vm(t, p, t->ox + w0 * 32, y, &V, &M);
		for (w = w0; w <= w1; w++, wi++) {
			uint32_t lo = (wi >= 0) ? srow[wi] : 0xFFFFFFFFu;
			uint32_t s, edge = 0xFFFFFFFFu;
			if (sh) {
				uint32_t hi = (wi + 1 < swpl) ? srow[wi + 1] : 0xFFFFFFFFu;
				s = (lo >> sh) | (hi << (32 - sh));
			} else {
				s = lo;
			}
			if (w == w0) edge &= e0;
			if (w == w1) edge &= e1;
			if (!mask) {
				drow[w] = (drow[w] & ~edge) | (s & edge);
				continue;
			}
			{
				uint32_t m = ~s & edge;
				if (!m) continue;
				if (xr) { drow[w] ^= m; continue; }
				if (!solid && !per) word_vm(t, p, t->ox + w * 32, y, &V, &M);
				m &= M;
				drow[w] = (drow[w] & ~m) | (V & m);
			}
		}
	}
}

static void blit_bits_any(zc_target_t *t, const uint32_t *src, int swpl, int sw,
	int sh, zc_fix dx, zc_fix dy, zc_fix dw, zc_fix dh, bool mask,
	const zc_paint_t *p) {
	int x0, y0, x1, y1, x, y;
	if (t->fmt == ZC_REC) {
		if (t->rec->image && !zc_clip_empty(t) && dw > 0 && dh > 0)
			t->rec->image(t, mask ? ZC_IMG_MASK : ZC_IMG_BITS, src, swpl, sw, sh,
				dx, dy, dw, dh, p);
		return;
	}
	if (!blit_box(t, dx, dy, dw, dh, &x0, &y0, &x1, &y1)) return;
	if (t->fmt == ZC_BITS && blit_aligned(dx, dy, dw, dh, sw, sh)) {
		blit_bits_aligned(t, src, swpl, sw, dx >> ZC_FRAC, dy >> ZC_FRAC,
			x0, y0, x1, y1, mask, p);
		return;
	}
	{
		int64_t sx0 = src_of(x0, dx, dw, sw);
		int64_t sxs = (((int64_t)ZC_ONE * sw) << 16) / dw;
		zc_paint_t white = zc_white(), black = zc_black();
		for (y = y0; y < y1; y++) {
			int sy = (int)(src_of(y, dy, dh, sh) >> 16);
			int64_t sx = sx0;
			if (sy < 0) sy = 0;
			if (sy >= sh) sy = sh - 1;
			for (x = x0; x < x1; x++, sx += sxs) {
				int si = (int)(sx >> 16);
				int b;
				if (si < 0) si = 0;
				if (si >= sw) si = sw - 1;
				b = src_bit(src, swpl, si, sy);
				if (mask) {
					if (!b) plot(t, x, y, p);
				} else {
					plot(t, x, y, b ? &white : &black);
				}
			}
		}
	}
}

void zc_blit_bits(zc_target_t *t, const uint32_t *src, int swpl, int sw,
	int sh, zc_fix dx, zc_fix dy, zc_fix dw, zc_fix dh) {
	blit_bits_any(t, src, swpl, sw, sh, dx, dy, dw, dh, false, NULL);
}

void zc_blit_mask(zc_target_t *t, const uint32_t *src, int swpl, int sw,
	int sh, zc_fix dx, zc_fix dy, zc_fix dw, zc_fix dh, const zc_paint_t *p) {
	blit_bits_any(t, src, swpl, sw, sh, dx, dy, dw, dh, true, p);
}

// Grey of a source at (fx, fy), 16.16 source coordinates of a device
// pixel's centre; `kx`, `ky` the source extent of one device pixel
// (16.16). Bilinear on an axis that is enlarged, a box on one reduced.
static int sample_gray(const uint8_t *src, int sstride, int sw, int sh,
	int64_t fx, int64_t fy, int64_t kx, int64_t ky) {
	int x0, x1, y0, y1, x, y;
	if (kx <= 65536 && ky <= 65536) {
		int64_t ax = fx - 32768, ay = fy - 32768;
		int ix = (int)(ax >> 16), iy = (int)(ay >> 16);
		int wx = (int)((ax & 0xFFFF) >> 8), wy = (int)((ay & 0xFFFF) >> 8);
		int ix1 = ix + 1, iy1 = iy + 1;
		int a, b, c, d;
		if (ix < 0) { ix = 0; wx = 0; }
		if (iy < 0) { iy = 0; wy = 0; }
		if (ix1 >= sw) ix1 = sw - 1;
		if (iy1 >= sh) iy1 = sh - 1;
		if (ix >= sw) ix = sw - 1;
		if (iy >= sh) iy = sh - 1;
		a = src[(size_t)iy * sstride + ix];
		b = src[(size_t)iy * sstride + ix1];
		c = src[(size_t)iy1 * sstride + ix];
		d = src[(size_t)iy1 * sstride + ix1];
		a = a * (256 - wx) + b * wx;
		c = c * (256 - wx) + d * wx;
		return (a * (256 - wy) + c * wy + 32768) >> 16;
	}
	x0 = (int)((fx - kx / 2) >> 16);
	x1 = (int)((fx + kx / 2 + 65535) >> 16);
	y0 = (int)((fy - ky / 2) >> 16);
	y1 = (int)((fy + ky / 2 + 65535) >> 16);
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > sw) x1 = sw;
	if (y1 > sh) y1 = sh;
	if (x1 <= x0) x1 = x0 + 1;
	if (y1 <= y0) y1 = y0 + 1;
	if (x1 > sw) { x1 = sw; x0 = sw - 1; }
	if (y1 > sh) { y1 = sh; y0 = sh - 1; }
	{
		uint32_t sum = 0, n = 0;
		for (y = y0; y < y1; y++) {
			const uint8_t *r = src + (size_t)y * sstride;
			for (x = x0; x < x1; x++) sum += r[x];
			n += (uint32_t)(x1 - x0);
		}
		return (int)((sum + n / 2) / n);
	}
}

void zc_blit_gray(zc_target_t *t, const uint8_t *src, int sstride, int sw,
	int sh, zc_fix dx, zc_fix dy, zc_fix dw, zc_fix dh) {
	int x0, y0, x1, y1, x, y;
	int64_t kx, ky;
	if (t->fmt == ZC_REC) {
		if (t->rec->image && !zc_clip_empty(t) && dw > 0 && dh > 0)
			t->rec->image(t, ZC_IMG_GRAY, src, sstride, sw, sh, dx, dy, dw, dh, NULL);
		return;
	}
	if (!blit_box(t, dx, dy, dw, dh, &x0, &y0, &x1, &y1)) return;
	kx = (((int64_t)ZC_ONE * sw) << 16) / dw;
	ky = (((int64_t)ZC_ONE * sh) << 16) / dh;
	for (y = y0; y < y1; y++) {
		int64_t fy = src_of(y, dy, dh, sh);
		int64_t fx = src_of(x0, dx, dw, sw);
		for (x = x0; x < x1; x++, fx += kx) {
			int g = sample_gray(src, sstride, sw, sh, fx, fy, kx, ky);
			if (t->fmt == ZC_GRAY) {
				((uint8_t *)t->pix)[(size_t)(y - t->oy) * t->stride + (x - t->ox)] =
					(uint8_t)g;
			} else {
				int tone = 16 - (g * 17) / 256;
				zc_paint_t sp = zc_shade(tone);
				plot(t, x, y, &sp);
			}
		}
	}
}

// -- a source through any transform ----------------------------------------

void zc_blit_xf(zc_target_t *t, int kind, const void *src, int stride, int sw,
	int sh, const zc_xf_t *m, const zc_paint_t *p) {
	zc_fix cx[4], cy[4];
	zc_fix lx, hx, ly, hy;
	int x0, y0, x1, y1, x, y, i;
	int64_t det, ia, ib, ic, id;
	zc_paint_t white = zc_white(), black = zc_black();

	if (sw <= 0 || sh <= 0 || zc_clip_empty(t)) return;
	if (zc_xf_is_axis(m) && m->a > 0 && m->d > 0) {
		// no turn: the ordinary blits, which the recorder also sees
		zc_fix dx = m->e, dy = m->f;
		zc_fix dw = (zc_fix)(((int64_t)sw * m->a) >> 8);
		zc_fix dh = (zc_fix)(((int64_t)sh * m->d) >> 8);
		if (kind == ZC_IMG_GRAY) zc_blit_gray(t, (const uint8_t *)src, stride, sw, sh, dx, dy, dw, dh);
		else if (kind == ZC_IMG_MASK) zc_blit_mask(t, (const uint32_t *)src, stride, sw, sh, dx, dy, dw, dh, p);
		else zc_blit_bits(t, (const uint32_t *)src, stride, sw, sh, dx, dy, dw, dh);
		return;
	}
	if (t->fmt == ZC_REC) return;		// a turned source is not described

	zc_xf_apply(m, 0, 0, &cx[0], &cy[0]);
	zc_xf_apply(m, ZC_I(sw), 0, &cx[1], &cy[1]);
	zc_xf_apply(m, 0, ZC_I(sh), &cx[2], &cy[2]);
	zc_xf_apply(m, ZC_I(sw), ZC_I(sh), &cx[3], &cy[3]);
	lx = hx = cx[0];
	ly = hy = cy[0];
	for (i = 1; i < 4; i++) {
		if (cx[i] < lx) lx = cx[i];
		if (cx[i] > hx) hx = cx[i];
		if (cy[i] < ly) ly = cy[i];
		if (cy[i] > hy) hy = cy[i];
	}
	x0 = ZC_TOINT(lx); y0 = ZC_TOINT(ly);
	x1 = ZC_TOINT(hx) + 1; y1 = ZC_TOINT(hy) + 1;
	if (x0 < t->cx0) x0 = t->cx0;
	if (y0 < t->cy0) y0 = t->cy0;
	if (x1 > t->cx1) x1 = t->cx1;
	if (y1 > t->cy1) y1 = t->cy1;
	if (x0 >= x1 || y0 >= y1) return;

	// the inverse of the 2x2 part, so each device pixel finds its source
	// point: 16.16 in, source pixels 16.16 out per device pixel
	det = (int64_t)m->a * m->d - (int64_t)m->b * m->c;		// 32.32
	if (det == 0) return;
	ia = ((int64_t)m->d << 32) / det;		// 16.16
	ib = -((int64_t)m->b << 32) / det;
	ic = -((int64_t)m->c << 32) / det;
	id = ((int64_t)m->a << 32) / det;

	for (y = y0; y < y1; y++) {
		// device centre (x0 + .5, y + .5) relative to the origin, 24.8
		int64_t rx = ((int64_t)x0 << ZC_FRAC) + ZC_HALF - m->e;
		int64_t ry = ((int64_t)y << ZC_FRAC) + ZC_HALF - m->f;
		// source position, 16.16 source pixels
		int64_t u = (ia * rx + ic * ry) >> ZC_FRAC;
		int64_t v = (ib * rx + id * ry) >> ZC_FRAC;
		if (kind == ZC_IMG_GRAY && t->fmt == ZC_GRAY && sw >= 2 && sh >= 2 &&
			sw < 32768 && sh < 32768) {
			// the common case, a picture turned onto a grey band: bilinear
			// in 32 bits, no call a pixel
			const uint8_t *sp = (const uint8_t *)src;
			uint8_t *drow = (uint8_t *)t->pix + (size_t)(y - t->oy) * t->stride - t->ox;
			int32_t uu = (int32_t)u, vv = (int32_t)v, du = (int32_t)ia, dv = (int32_t)ib;
			uint32_t uw = (uint32_t)sw << 16, vh = (uint32_t)sh << 16;
			for (x = x0; x < x1; x++, uu += du, vv += dv) {
				int32_t ax, ay;
				int ix, iy, wx, wy, a, b, c, d;
				const uint8_t *q;
				if ((uint32_t)uu >= uw || (uint32_t)vv >= vh) continue;
				ax = uu - 32768;
				ay = vv - 32768;
				ix = ax >> 16;
				iy = ay >> 16;
				wx = (ax >> 8) & 255;
				wy = (ay >> 8) & 255;
				if (ix < 0) { ix = 0; wx = 0; } else if (ix > sw - 2) { ix = sw - 2; wx = 255; }
				if (iy < 0) { iy = 0; wy = 0; } else if (iy > sh - 2) { iy = sh - 2; wy = 255; }
				q = sp + iy * stride + ix;
				a = q[0]; b = q[1]; c = q[stride]; d = q[stride + 1];
				a = (a << 8) + (b - a) * wx;
				c = (c << 8) + (d - c) * wx;
				drow[x] = (uint8_t)(((a << 8) + (c - a) * wy + 32768) >> 16);
			}
			continue;
		}
		for (x = x0; x < x1; x++, u += ia, v += ib) {
			int sx = (int)(u >> 16), sy = (int)(v >> 16);
			if (u < 0 || v < 0 || sx >= sw || sy >= sh) continue;
			if (kind == ZC_IMG_GRAY) {
				int g = sample_gray((const uint8_t *)src, stride, sw, sh, u, v, 32768, 32768);
				if (t->fmt == ZC_GRAY) {
					((uint8_t *)t->pix)[(size_t)(y - t->oy) * t->stride + (x - t->ox)] = (uint8_t)g;
				} else {
					zc_paint_t sp = zc_shade(16 - (g * 17) / 256);
					plot(t, x, y, &sp);
				}
			} else {
				const uint32_t *row = (const uint32_t *)src + (size_t)sy * (size_t)stride;
				int bit = (int)((row[sx >> 5] >> (sx & 31)) & 1);
				if (kind == ZC_IMG_MASK) {
					if (!bit) plot(t, x, y, p);
				} else {
					plot(t, x, y, bit ? &white : &black);
				}
			}
		}
	}
}

void zc_resample(const uint8_t *src, int sstride, int sw, int sh,
	uint8_t *dst, int dstride, int dw, int dh, uint32_t *row) {
	int x, y;
	int64_t kx = ((int64_t)sw << 16) / dw, ky = ((int64_t)sh << 16) / dh;
	(void)row;
	for (y = 0; y < dh; y++) {
		int64_t fy = (((int64_t)y * 2 + 1) * ((int64_t)sh << 16)) / (2 * dh);
		for (x = 0; x < dw; x++) {
			int64_t fx = (((int64_t)x * 2 + 1) * ((int64_t)sw << 16)) / (2 * dw);
			dst[(size_t)y * dstride + x] =
				(uint8_t)sample_gray(src, sstride, sw, sh, fx, fy, kx, ky);
		}
	}
}

void zc_dither(const uint8_t *src, int sstride, int sw, int sh,
	uint32_t *dst, int dwpl, int dw, int dh, int16_t *row,
	int32_t contrast, int bright) {
	int16_t *e_cur = row + 1;
	int16_t *e_nxt = row + (dw + 2) + 1;
	int16_t *g = row + 2 * (dw + 2);
	int64_t kx = ((int64_t)sw << 16) / dw, ky = ((int64_t)sh << 16) / dh;
	int x, y;

	memset(row, 0, (size_t)(2 * (dw + 2)) * sizeof(int16_t));
	for (y = 0; y < dh; y++) {
		int64_t fy = (((int64_t)y * 2 + 1) * ((int64_t)sh << 16)) / (2 * dh);
		uint32_t *drow = dst + (size_t)y * (size_t)dwpl;
		bool rev = (y & 1) != 0;
		int16_t *tmp;

		for (x = 0; x < dw; x++) {
			int64_t fx = (((int64_t)x * 2 + 1) * ((int64_t)sw << 16)) / (2 * dw);
			int v = sample_gray(src, sstride, sw, sh, fx, fy, kx, ky);
			if (contrast) v = 128 + (int)((((int64_t)(v - 128)) * contrast) >> 16);
			v += bright;
			g[x] = (int16_t)v;
		}
		memset(e_nxt - 1, 0, (size_t)(dw + 2) * sizeof(int16_t));
		for (x = 0; x < (dw + 31) / 32; x++) drow[x] = 0;

		for (x = 0; x < dw; x++) {
			int xi = rev ? dw - 1 - x : x;
			int dir = rev ? -1 : 1;
			int v = g[xi] + e_cur[xi];
			int out = v >= 128 ? 255 : 0;
			int err = v - out;
			if (out) drow[xi >> 5] |= 1u << (xi & 31);
			e_cur[xi + dir] += (int16_t)((err * 7) / 16);
			e_nxt[xi - dir] += (int16_t)((err * 3) / 16);
			e_nxt[xi] += (int16_t)((err * 5) / 16);
			e_nxt[xi + dir] += (int16_t)(err / 16);
		}
		tmp = e_cur; e_cur = e_nxt; e_nxt = tmp;
	}
}
