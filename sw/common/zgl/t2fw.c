/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * t2fw.c -- T2's geometry unit: the program its small RISC-V core
 * (zeitlos32g) runs, and the host model runs as it is (tests/t2_model.c).
 * docs/zgl_t2.md.
 *
 * It carries out the stream zgl sends (zgl_t2.h): a chunk's vertices
 * are transformed and lit, and projected if inside the view, into the
 * vertex buffer; its triangles and lines, by vertex number, are culled,
 * clipped and set up into records for the raster (t2io_rec()). TRI and
 * LINE commands, set up by the CPU, become records directly.
 *
 * The arithmetic is zgl's own (zgl.c, zgl_frames.c), so T2 draws what
 * the software draws. No libc: integers only (64-bit division from
 * libgcc where zgl uses it).
 */

#include <stdint.h>
#include <stdbool.h>
#include "zgl_t2.h"

#define POS_ONE 256

/* What is rarely run -- clipping, lines, the CPU's own triangles,
 * positional lights -- is built for size, apart from the hot paths:
 * the code memory is 12 KB without textures (sw/t2fw/link.ld). */
#define T2_COLD __attribute__((noinline, cold, optimize("Os")))

typedef struct {
	int32_t x, y, z, w;      /* clip, 20.12 */
	int32_t ci;              /* 8.8 */
#ifndef T2_NO_TEX
	int32_t s, t;            /* texture, texels 12.4 (T2_C_TEX) */
#endif
	int32_t sx, sy, sz;      /* picture 24.8, depth 16.15 */
	uint8_t out, sv;
} vtx_t;

/* 28.4, as zgl_svtx_t; and for a textured triangle s, t and clip w */
typedef struct { int32_t x, y, z, ci, s, t, w; } svtx_t;

/* the state */
static int32_t fx0, fx1, fy0, fy1;               /* the frame's rectangle */
static int32_t vp_x, vp_y, vp_w, vp_h;
static int32_t m[16], near_w, far_w;
static bool persp;
static bool lighting, light_on[2], depth, smooth, front_ccw;
/* textures (STATE bit 5) on, and the chunk being drawn's texture word
 * (T2_C_TEX); tri_tex: the triangles being set up are textured. Built
 * with T2_NO_TEX (sw/t2fw's t2fw.hex, for an engine without textures)
 * a textured chunk is drawn in its colours. */
static bool tex_on, tri_tex;
/* positional lights (PLIGHT, zgl_t2.h): where each is (the model's
 * space, 24.8), the model's scale (16.16), the attenuation */
static bool pl_on[2], pl_unit[2], pl_any;
static int32_t pl_p[2][3], pl_s[2], pl_kc[2], pl_kl[2];
static uint32_t pl_kq[2];
static uint32_t tex_word;
static int cull;
static int32_t light_dir[2][3], light_d[2], mat_a, mat_d;
static vtx_t vb[T2_CHUNK_V];                     /* the vertex buffer */

t2_stats_t t2fw_stats;
#define ST t2fw_stats
/* a frame's counts, for the model's reports (tests/t2_model.c); on the
 * core nothing reads them, and each costs a load and a store */
#ifdef T2_CORE
#define STAT(f) do { } while (0)
#else
#define STAT(f) (ST.f++)
#endif

static void vcopy(vtx_t *d, const vtx_t *s) {
	d->x = s->x; d->y = s->y; d->z = s->z; d->w = s->w; d->ci = s->ci;
	d->sx = s->sx; d->sy = s->sy; d->sz = s->sz; d->out = s->out; d->sv = s->sv;
#ifndef T2_NO_TEX
	d->s = s->s; d->t = s->t;
#endif
}

/* -- zgl's arithmetic ------------------------------------------------------- */

/* n / d as C divides int64_t (toward zero), without libgcc's 2 KB: the
 * 32-bit divide when both fit (the common case), else bit by bit */
static T2_COLD int64_t sdiv64(int64_t n, int64_t d) {
	uint32_t nh, nl, dh, dl, rh = 0, rl = 0, qh = 0, ql = 0;
	uint64_t un, ud;
	int neg, i;
	if (n >= INT32_MIN && n <= INT32_MAX && d >= INT32_MIN && d <= INT32_MAX && d != 0 &&
	    !(n == INT32_MIN && d == -1))
		return (int32_t)n / (int32_t)d;
	if (d == 0) return 0;
	neg = (n < 0) != (d < 0);
	un = n < 0 ? (uint64_t)0 - (uint64_t)n : (uint64_t)n;
	ud = d < 0 ? (uint64_t)0 - (uint64_t)d : (uint64_t)d;
	nh = (uint32_t)(un >> 32); nl = (uint32_t)un;
	dh = (uint32_t)(ud >> 32); dl = (uint32_t)ud;
	/* bit by bit, in 32-bit halves (no libgcc shifts); a quotient that
	 * fits 32 bits (the usual: clipping's t, a line's steps) takes 32 */
	i = 63;
	if (dh == 0 && nh < dl) { rl = nh; i = 31; }   /* the top half is all remainder */
	for (; i >= 0; i--) {
		uint32_t bit = i >= 32 ? (nh >> (i - 32)) & 1 : (nl >> i) & 1;
		rh = rh << 1 | rl >> 31;
		rl = rl << 1 | bit;
		if (rh > dh || (rh == dh && rl >= dl)) {
			uint32_t b0 = rl < dl;
			rl -= dl;
			rh -= dh + b0;
			if (i >= 32) qh |= 1u << (i - 32); else ql |= 1u << i;
		}
	}
	un = (uint64_t)qh << 32 | ql;
	return neg ? -(int64_t)un : (int64_t)un;
}

/* the highest set bit's number (v != 0); zgl's shifting loops below
 * become one shift by it, with the same results */
static inline int msb32(uint32_t v) {
	int n = 0;
	if (v >> 16) { v >>= 16; n += 16; }
	if (v >> 8) { v >>= 8; n += 8; }
	if (v >> 4) { v >>= 4; n += 4; }
	if (v >> 2) { v >>= 2; n += 2; }
	if (v >> 1) n += 1;
	return n;
}

static inline int msb64(uint64_t v) {
	uint32_t h = (uint32_t)(v >> 32);
	return h ? 32 + msb32(h) : msb32((uint32_t)v);
}

static int32_t ndc_px(int32_t x, int32_t w, int32_t half) {
	int32_t q;
	int sh = 0;
	if (w >= (1 << 19)) sh = msb32((uint32_t)w) - 18;   /* while (w >= 2^19) w >>= 1 */
	w >>= sh;
	x >>= sh;
	if (w <= 0) w = 1;
	if (x > 4 * w || x < -4 * w)
		return (int32_t)sdiv64((int64_t)x * half, w);
	q = (x * 1024) / w;
	return (q * half) / 1024;
}

static void project(const vtx_t *v, int32_t *sx, int32_t *sy) {
	int32_t hw = vp_w * (POS_ONE / 2), hh = vp_h * (POS_ONE / 2);
	int32_t w = v->w > 0 ? v->w : 1;
	*sx = vp_x * POS_ONE + hw + ndc_px(v->x, w, hw);
	*sy = vp_y * POS_ONE + hh - ndc_px(v->y, w, hh);
}

static void screen_of(vtx_t *v) {
	int32_t z = v->z, w = v->w > 0 ? v->w : 1, q;
	project(v, &v->sx, &v->sy);
	if (w >= (1 << 15)) {                    /* while (w >= 2^15) both >>= 1 */
		int sh = msb32((uint32_t)w) - 14;
		w >>= sh;
		z >>= sh;
	}
	if (z > w) z = w;
	if (z < -w) z = -w;
	q = (z * 32768) / w;
	q += 32768;
	if (q > 65535) q = 65535;
	if (q < 0) q = 0;
	v->sz = q << 15;
	v->sv = 1;
}

static T2_COLD int32_t plane_d1(const vtx_t *v, int p) {
	switch (p) {
	case 0: return v->w + v->x;
	case 1: return v->w - v->x;
	case 2: return v->w + v->y;
	case 3: return v->w - v->y;
	case 4: return persp ? v->w - near_w : v->w + v->z;
	default: return persp ? (far_w == INT32_MAX ? INT32_MAX / 2 : far_w - v->w) : v->w - v->z;
	}
}

static uint8_t out_code(const vtx_t *v) {
	int32_t w = v->w;
	uint8_t c = (uint8_t)((w + v->x < 0) | (w - v->x < 0) << 1 | (w + v->y < 0) << 2 | (w - v->y < 0) << 3);
	if (persp) {
		if (w - near_w < 0) c |= 16;
		if (far_w != INT32_MAX && far_w - w < 0) c |= 32;
	} else {
		if (w + v->z < 0) c |= 16;
		if (w - v->z < 0) c |= 32;
	}
	return c;
}

#define T_ONE ((int64_t)1 << 30)

static T2_COLD void lerp(vtx_t *o, const vtx_t *a, const vtx_t *b, int64_t t) {
	o->x = a->x + (int32_t)(((int64_t)(b->x - a->x) * t) >> 30);
	o->y = a->y + (int32_t)(((int64_t)(b->y - a->y) * t) >> 30);
	o->z = a->z + (int32_t)(((int64_t)(b->z - a->z) * t) >> 30);
	o->w = a->w + (int32_t)(((int64_t)(b->w - a->w) * t) >> 30);
	o->ci = a->ci + (int32_t)(((int64_t)(b->ci - a->ci) * t) >> 30);
#ifndef T2_NO_TEX
	/* in clip space a texture coordinate is linear, as x, y, z and w */
	o->s = a->s + (int32_t)(((int64_t)(b->s - a->s) * t) >> 30);
	o->t = a->t + (int32_t)(((int64_t)(b->t - a->t) * t) >> 30);
#endif
	o->out = 0;
	o->sv = 0;
}

/* a corner behind the eye: the clip-space determinant */
static int64_t facing_clip(vtx_t *a, vtx_t *b, vtx_t *c) {
	int64_t ax = a->x >> 12, ay = a->y >> 12, aw = a->w >> 12;
	int64_t bx = b->x >> 12, by = b->y >> 12, bw = b->w >> 12;
	int64_t cx = c->x >> 12, cy = c->y >> 12, cw = c->w >> 12;
	return ax * (by * cw - bw * cy) - ay * (bx * cw - bw * cx) + aw * (bx * cy - by * cx);
}

static int facing(vtx_t *a, vtx_t *b, vtx_t *c) {
	int64_t d;
	if (a->w > 0 && b->w > 0 && c->w > 0) {
		int32_t ax, ay, bx, by, cx, cy;
		if (a->sv) { ax = a->sx; ay = a->sy; } else project(a, &ax, &ay);
		if (b->sv) { bx = b->sx; by = b->sy; } else project(b, &bx, &by);
		if (c->sv) { cx = c->sx; cy = c->sy; } else project(c, &cx, &cy);
		d = -((int64_t)(bx - ax) * (cy - ay) - (int64_t)(by - ay) * (cx - ax));
	} else d = facing_clip(a, b, c);
	return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

static int32_t div_s64(int64_t num, int64_t den) {
	uint64_t un, ud;
	uint32_t inv;
	int s = 0, neg;
	int64_t q;
	if (num >= -0x7fffffffLL && num <= 0x7fffffffLL && den >= -0x7fffffffLL && den <= 0x7fffffffLL)
		return (int32_t)num / (int32_t)den;
	neg = (num < 0) != (den < 0);
	un = num < 0 ? (uint64_t)-num : (uint64_t)num;
	ud = den < 0 ? (uint64_t)-den : (uint64_t)den;
	/* ud to [2^15, 2^16), un below 2^46: zgl's loops, as one shift each */
	{
		int m = msb64(ud);
		if (m > 15) { s = m - 15; ud >>= s; }
		else if (m < 15) { s = m - 15; ud <<= 15 - m; }
	}
	inv = (uint32_t)((1u << 31) / (uint32_t)ud);
	if (un >> 46) {
		int k = msb64(un) - 45;
		un >>= k;
		s -= k;
	}
	if (31 + s <= 0) q = INT32_MAX;
	else if (31 + s >= 63) q = 0;
	else q = (int64_t)((un * inv) >> (31 + s));
	if (q > INT32_MAX) q = INT32_MAX;
	return (int32_t)(neg ? -q : q);
}

#ifndef T2_NO_TEX
/* div_s64() by one denominator many times (a triangle's area, for all
 * its planes): its reciprocal found once. The same steps in the same
 * order, so the same quotients. */
typedef struct {
	int64_t den;
	uint32_t inv;
	int s;
	bool fits;                     /* den fits 32 bits: the fast path may apply */
	bool ready;                    /* inv and s found (on the first slow one) */
} rdiv_t;

static inline void rdiv_init(rdiv_t *r, int64_t den) {
	r->den = den;
	r->fits = den >= -0x7fffffffLL && den <= 0x7fffffffLL;
	r->ready = false;
}

static __attribute__((noinline)) int32_t rdiv(rdiv_t *r, int64_t num) {
	uint64_t un;
	int s, neg;
	int64_t q;
	if (r->fits && num >= -0x7fffffffLL && num <= 0x7fffffffLL)
		return (int32_t)num / (int32_t)r->den;
	if (!r->ready) {
		uint64_t ud = r->den < 0 ? (uint64_t)-r->den : (uint64_t)r->den;
		int m = msb64(ud);
		r->s = 0;
		if (m > 15) { r->s = m - 15; ud >>= r->s; }
		else if (m < 15) { r->s = m - 15; ud <<= 15 - m; }
		r->inv = (uint32_t)((1u << 31) / (uint32_t)ud);
		r->ready = true;
	}
	s = r->s;
	neg = (num < 0) != (r->den < 0);
	un = num < 0 ? (uint64_t)-num : (uint64_t)num;
	if (un >> 46) {
		int k = msb64(un) - 45;
		un >>= k;
		s -= k;
	}
	if (31 + s <= 0) q = INT32_MAX;
	else if (31 + s >= 63) q = 0;
	else q = (int64_t)((un * r->inv) >> (31 + s));
	if (q > INT32_MAX) q = INT32_MAX;
	return (int32_t)(neg ? -q : q);
}
#endif

/* -- records for the raster ------------------------------------------------- */

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* zgl_frames.c's tri_setup(), as a record: the planes at (bx0, ylo) */
static void rec_tri(const svtx_t *pa, const svtx_t *pb, const svtx_t *pc, bool dep) {
	const svtx_t *v0 = pa, *v1 = pb, *v2 = pc, *t;
	int64_t area, a1, a2, dy1, dy2, dx1, dx2, k;
	int32_t dzdx, dzdy, dcdx, dcdy, ylo, yhi, ymid, bx0, bx1, mn, mx;
	volatile uint32_t *w;
	if (v1->y < v0->y) { t = v0; v0 = v1; v1 = t; }
	if (v2->y < v1->y) { t = v1; v1 = v2; v2 = t; }
	if (v1->y < v0->y) { t = v0; v0 = v1; v1 = t; }
	area = (int64_t)(v1->x - v0->x) * (v2->y - v0->y) - (int64_t)(v2->x - v0->x) * (v1->y - v0->y);
	if (area == 0 || ((v0->y + 15) >> 4) >= ((v2->y + 15) >> 4)) return;
	ylo = clampi((v0->y + 15) >> 4, fy0, fy1 + 1);
	yhi = clampi(((v2->y + 15) >> 4) - 1, fy0 - 1, fy1);
	if (ylo > yhi) return;
	ymid = clampi((v1->y + 15) >> 4, ylo, yhi + 1);
	mn = v0->x; mx = v0->x;
	if (v1->x < mn) mn = v1->x;
	if (v1->x > mx) mx = v1->x;
	if (v2->x < mn) mn = v2->x;
	if (v2->x > mx) mx = v2->x;
	bx0 = clampi((mn + 15) >> 4, fx0, fx1 + 1);
	bx1 = clampi((mx + 15) >> 4, fx0 - 1, fx1);
	if (bx0 > bx1) return;
	/* coordinates the raster's 16-bit fields can't hold: dropped (the
	 * front end clips to the view, so only a guard-band case) */
	if (v0->x < -32768 || v0->x > 32767 || v1->x < -32768 || v1->x > 32767 ||
	    v2->x < -32768 || v2->x > 32767 || v0->y < -32768 || v2->y > 32767) return;
	/* the record goes straight into the list (no copy on the way) */
	w = t2io_rec(tri_tex ? T2_REC_TWORDS : T2_REC_WORDS);
	if (!w) { STAT(overflows); return; }
	STAT(drawn);
	dy1 = v1->y - v0->y; dy2 = v2->y - v0->y;
	dx1 = v1->x - v0->x; dx2 = v2->x - v0->x;
	a1 = v1->z - v0->z; a2 = v2->z - v0->z;
	dzdx = div_s64((a1 * dy2 - a2 * dy1) * 16, area);
	dzdy = div_s64((a2 * dx1 - a1 * dx2) * 16, area);
	a1 = v1->ci - v0->ci; a2 = v2->ci - v0->ci;
	if (a1 == 0 && a2 == 0) { dcdx = 0; dcdy = 0; }
	else {
		dcdx = div_s64((a1 * dy2 - a2 * dy1) * 16, area);
		dcdy = div_s64((a2 * dx1 - a1 * dx2) * 16, area);
	}
	w[0] = (tri_tex ? T2_REC_TTRI : T2_REC_TRI) | (dep ? 4u : 0u) | (area > 0 ? 8u : 0u) |
	       (uint32_t)ylo << 8 | (uint32_t)yhi << 16 | (uint32_t)ymid << 24;
	w[1] = (uint32_t)bx0 | (uint32_t)bx1 << 16;
	w[2] = (uint32_t)(uint16_t)v0->x | (uint32_t)(uint16_t)v0->y << 16;
	w[3] = (uint32_t)(uint16_t)v1->x | (uint32_t)(uint16_t)v1->y << 16;
	w[4] = (uint32_t)(uint16_t)v2->x | (uint32_t)(uint16_t)v2->y << 16;
	/* Z(x, y) = z0 + ((-dzdx x0 + dzdy (16 y - y0)) >> 4) + dzdx x, mod 2^32 */
	k = -(int64_t)dzdx * v0->x + (int64_t)dzdy * (ylo * 16 - v0->y);
	w[5] = (uint32_t)v0->z + (uint32_t)(k >> 4) + (uint32_t)dzdx * (uint32_t)bx0;
	w[6] = (uint32_t)dzdx; w[7] = (uint32_t)dzdy;
	k = -(int64_t)dcdx * v0->x + (int64_t)dcdy * (ylo * 16 - v0->y);
	w[8] = (uint32_t)v0->ci + (uint32_t)(k >> 4) + (uint32_t)dcdx * (uint32_t)bx0;
	w[9] = (uint32_t)dcdx; w[10] = (uint32_t)dcdy;
#ifndef T2_NO_TEX
	if (tri_tex) {
		/* Perspective-correct: s/w, t/w and 1/w are linear on the
		 * screen, so they are the planes, and the raster divides
		 * (zgl_t2.h, "TTRI"). 1/w as q = 2^24 wmin / w, the nearest
		 * corner 2^24; s q and t q >> 8 so a coordinate of up to
		 * +-1,024 texels fits. */
		int32_t q0, q1, q2, wm = v0->w, n0, n1, n2;
		int j;
		/* six planes by one area: its reciprocal once */
		rdiv_t ra;
		rdiv_init(&ra, area);
		if (v1->w < wm) wm = v1->w;
		if (v2->w < wm) wm = v2->w;
		if (wm < 1) wm = 1;
		q0 = div_s64((int64_t)wm << 24, v0->w < 1 ? 1 : v0->w);
		q1 = div_s64((int64_t)wm << 24, v1->w < 1 ? 1 : v1->w);
		q2 = div_s64((int64_t)wm << 24, v2->w < 1 ? 1 : v2->w);
		w[11] = tex_word;
		for (j = 0; j < 3; j++) {
			int32_t pdx, pdy;
			if (j == 0) { n0 = (int32_t)(((int64_t)v0->s * q0) >> 8); n1 = (int32_t)(((int64_t)v1->s * q1) >> 8); n2 = (int32_t)(((int64_t)v2->s * q2) >> 8); }
			else if (j == 1) { n0 = (int32_t)(((int64_t)v0->t * q0) >> 8); n1 = (int32_t)(((int64_t)v1->t * q1) >> 8); n2 = (int32_t)(((int64_t)v2->t * q2) >> 8); }
			else { n0 = q0; n1 = q1; n2 = q2; }
			a1 = (int64_t)n1 - n0; a2 = (int64_t)n2 - n0;
			pdx = rdiv(&ra, (a1 * dy2 - a2 * dy1) * 16);
			pdy = rdiv(&ra, (a2 * dx1 - a1 * dx2) * 16);
			k = -(int64_t)pdx * v0->x + (int64_t)pdy * (ylo * 16 - v0->y);
			w[12 + j * 3] = (uint32_t)n0 + (uint32_t)(k >> 4) + (uint32_t)pdx * (uint32_t)bx0;
			w[13 + j * 3] = (uint32_t)pdx;
			w[14 + j * 3] = (uint32_t)pdy;
		}
	}
#endif
}

/* zgl_frames.c's draw_line(), as a record: every step's numbers */
static T2_COLD void rec_line(const svtx_t *a, const svtx_t *b, bool dep) {
	int32_t dx = b->x - a->x, dy = b->y - a->y;
	int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
	int32_t n = ((adx > ady ? adx : ady) + 15) >> 4;
	int32_t ymin = a->y < b->y ? a->y : b->y, ymax = a->y < b->y ? b->y : a->y;
	int32_t ylo = clampi((ymin + 8) >> 4, fy0, fy1 + 1), yhi = clampi((ymax + 8) >> 4, fy0 - 1, fy1);
	volatile uint32_t *w;
	if (ylo > yhi) return;
	if (n == 0) n = 1;
	w = t2io_rec(10);
	if (!w) { STAT(overflows); return; }
	STAT(drawn);
	w[0] = T2_REC_LINE | (dep ? 4u : 0u) | (uint32_t)ylo << 8 | (uint32_t)yhi << 16;
	w[1] = (uint32_t)n;
	w[2] = (uint32_t)a->x << 16;
	w[3] = (uint32_t)a->y << 16;
	w[4] = (uint32_t)sdiv64((int64_t)dx * 65536, n);
	w[5] = (uint32_t)sdiv64((int64_t)dy * 65536, n);
	w[6] = (uint32_t)a->z;
	w[7] = (uint32_t)sdiv64((int64_t)(b->z - a->z), n);
	w[8] = (uint32_t)a->ci;
	w[9] = (uint32_t)sdiv64((int64_t)(b->ci - a->ci), n);
}

static void to_s(const vtx_t *v, svtx_t *o, bool flat, int32_t ci) {
	o->x = v->sx >> 4;
	o->y = v->sy >> 4;
	o->z = v->sz;
	o->ci = flat ? ci : v->ci;
#ifndef T2_NO_TEX
	o->s = v->s;
	o->t = v->t;
	o->w = v->w;
#endif
}

/* -- geometry --------------------------------------------------------------- */

#define CLIP_MAX 12

/* a corner's place fits the record (28.4 in 16 bits), with a margin */
static inline bool in_band(const vtx_t *v) {
	return v->sx > -(32000 << 4) && v->sx < (32000 << 4) &&
	       v->sy > -(32000 << 4) && v->sy < (32000 << 4);
}

static void clip_tri(vtx_t *a, vtx_t *b, vtx_t *c, bool flat, int32_t flat_ci);

static void geom_tri(vtx_t *a, vtx_t *b, vtx_t *c, int32_t flat_ci) {
	int f;
	bool front, flat = !smooth;
	svtx_t s0, s1, s2;
	STAT(tris);
	if (a->out & b->out & c->out) { STAT(culled); STAT(outside); return; }
	f = facing(a, b, c);
	front = front_ccw ? (f >= 0) : (f <= 0);
	if (cull == 3 || (cull == 1 && !front) || (cull == 2 && front)) { STAT(culled); return; }
	if (!(a->out | b->out | c->out)) {
		if (!a->sv) screen_of(a);
		if (!b->sv) screen_of(b);
		if (!c->sv) screen_of(c);
		to_s(a, &s0, flat, flat_ci); to_s(b, &s1, flat, flat_ci); to_s(c, &s2, flat, flat_ci);
		rec_tri(&s0, &s1, &s2, depth);
		return;
	}
	/* The guard band: outside only the picture's sides (not the near or
	 * far plane), a triangle goes to the raster unclipped if its corners
	 * fit the record's 16-bit 12.4 fields (+-2,048 pixels): the raster
	 * draws only the bounding box clamped to the frame, and its edge
	 * functions don't care where the corners are (rtl/gpu/gpu_t2.v, the
	 * setup's widths). This is where T2 parts from zgl's arithmetic,
	 * which clips to the sides: a merged world face off the side of the
	 * picture costs a record, not a clip. */
	if (!((a->out | b->out | c->out) & 48)) {
		if (!a->sv) screen_of(a);
		if (!b->sv) screen_of(b);
		if (!c->sv) screen_of(c);
		if (in_band(a) && in_band(b) && in_band(c)) {
			STAT(guard);
			to_s(a, &s0, flat, flat_ci); to_s(b, &s1, flat, flat_ci); to_s(c, &s2, flat, flat_ci);
			rec_tri(&s0, &s1, &s2, depth);
			return;
		}
	}
	clip_tri(a, b, c, flat, flat_ci);
}

/* Sutherland-Hodgman, as zgl's clip_polygon() */
static T2_COLD void clip_tri(vtx_t *a, vtx_t *b, vtx_t *c, bool flat, int32_t flat_ci) {
	svtx_t s0, s1, s2;
	{
		static vtx_t buf[2][CLIP_MAX];
		vtx_t *in = buf[1], *out;
		uint8_t any = a->out | b->out | c->out;
		int n = 3, i, p, mm, cur = 0;
		STAT(clipped);
		vcopy(&in[0], a); vcopy(&in[1], b); vcopy(&in[2], c);
		for (p = 0; p < 6 && any; p++) {
			if (!(any & (1 << p))) continue;
			out = buf[cur];
			mm = 0;
			for (i = 0; i < n; i++) {
				vtx_t *va = &in[i], *vb2 = &in[i + 1 < n ? i + 1 : 0];
				int32_t da = plane_d1(va, p), db = plane_d1(vb2, p);
				if (da >= 0 && mm < CLIP_MAX) vcopy(&out[mm++], va);
				if ((da >= 0) != (db >= 0) && mm < CLIP_MAX) {
					int64_t t = sdiv64((int64_t)da * T_ONE, (int64_t)da - db);
					lerp(&out[mm++], va, vb2, t);
				}
			}
			in = out;
			n = mm;
			cur ^= 1;
			if (n < 3) return;
		}
		for (i = 0; i < n; i++) if (!in[i].sv) screen_of(&in[i]);
		for (i = 2; i < n; i++) {
			to_s(&in[0], &s0, flat, flat_ci); to_s(&in[i - 1], &s1, flat, flat_ci); to_s(&in[i], &s2, flat, flat_ci);
			rec_tri(&s0, &s1, &s2, depth);
		}
	}
}

/* zgl's zgl_line(): clipped (Liang-Barsky), the second end's colour if flat */
static T2_COLD void geom_line(vtx_t *a, vtx_t *b) {
	vtx_t p, q;
	svtx_t sa, sb;
	STAT(lines);
	if (a->out & b->out) { STAT(culled); return; }
	if (!(a->out | b->out)) { vcopy(&p, a); vcopy(&q, b); }
	else {
		int64_t t0 = 0, t1 = T_ONE;
		int i;
		STAT(clipped);
		for (i = 0; i < 6; i++) {
			int32_t da = plane_d1(a, i), db = plane_d1(b, i);
			if (da < 0 && db < 0) return;
			if (da < 0) {
				int64_t t = sdiv64((int64_t)da * T_ONE, (int64_t)da - db);
				if (t > t0) t0 = t;
			} else if (db < 0) {
				int64_t t = sdiv64((int64_t)da * T_ONE, (int64_t)da - db);
				if (t < t1) t1 = t;
			}
		}
		if (t0 > t1) return;
		if (t0 > 0) lerp(&p, a, b, t0); else vcopy(&p, a);
		if (t1 < T_ONE) lerp(&q, a, b, t1); else vcopy(&q, b);
	}
	if (!p.sv) screen_of(&p);
	if (!q.sv) screen_of(&q);
	to_s(&p, &sa, false, 0);
	to_s(&q, &sb, false, 0);
	if (!smooth) sa.ci = sb.ci;
	rec_line(&sa, &sb, depth);
}

/* zgl's vertex_ci() with the light in the model's space */
/* the square root of v, rounded down (zgl.c's zgl_isqrt64()) */
static uint32_t isqrt64(uint64_t v) {
	uint64_t r = 0, b = (uint64_t)1 << 62;
	while (b > v) b >>= 2;
	while (b) {
		if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
		else r >>= 1;
		b >>= 2;
	}
	return (uint32_t)r;
}

/* positional light l's dot at model position x, y, z (24.8), normal
 * n (16.16): zgl.c's point_dot() in the model's space */
static T2_COLD int32_t point_dot(int l, int32_t nx, int32_t ny, int32_t nz,
	int32_t x, int32_t y, int32_t z) {
	int64_t L0 = (int64_t)pl_p[l][0] - x, L1 = (int64_t)pl_p[l][1] - y, L2 = (int64_t)pl_p[l][2] - z;
	int64_t nl, de, den;
	uint32_t d = isqrt64((uint64_t)(L0 * L0 + L1 * L1 + L2 * L2));
	int32_t dot, att;
	if (!d) return 65536;
	nl = (int64_t)nx * L0 + (int64_t)ny * L1 + (int64_t)nz * L2;
	dot = div_s64(nl, d);
	if (!pl_unit[l]) dot = (int32_t)(((int64_t)dot * pl_s[l]) >> 16);
	/* the attenuation, at the eye's distance */
	de = ((int64_t)d * pl_s[l]) >> 8;                              /* 16.16 */
	if (de > 0x7fffffff) de = 0x7fffffff;
	den = (int64_t)pl_kc[l] + (((int64_t)pl_kl[l] * de) >> 24) +
	      (int64_t)(((((uint64_t)de * (uint64_t)de) >> 32) * pl_kq[l]) >> 16);
	att = den <= 65536 ? 65536 : div_s64((int64_t)1 << 32, den);
	return (int32_t)(((int64_t)dot * att) >> 16);
}

/* a vertex lit with a positional light on: each light its own way */
static T2_COLD int32_t lit_pt(int32_t nx, int32_t ny, int32_t nz, int32_t ma, int32_t md,
	int32_t x, int32_t y, int32_t z) {
	int32_t d = 0;
	int l;
	for (l = 0; l < 2; l++) {
		int32_t dot;
		if (!light_on[l]) continue;
		if (pl_on[l]) dot = point_dot(l, nx, ny, nz, x, y, z);
		else dot = (int32_t)(((int64_t)nx * light_dir[l][0] + (int64_t)ny * light_dir[l][1] +
		                      (int64_t)nz * light_dir[l][2]) >> 16);
		if (dot > 0) d += (int32_t)(((int64_t)dot * light_d[l]) >> 16);
	}
	if (d > 65536) d = 65536;
	return ma + (int32_t)(((int64_t)(md - ma) * d) >> 16);
}

static int32_t lit(int32_t nx, int32_t ny, int32_t nz, int32_t ma, int32_t md) {
	int32_t d = 0;
	int l;
	for (l = 0; l < 2; l++) {
		int32_t dot;
		if (!light_on[l]) continue;
		dot = (int32_t)(((int64_t)nx * light_dir[l][0] + (int64_t)ny * light_dir[l][1] +
		                 (int64_t)nz * light_dir[l][2]) >> 16);
		if (dot > 0) d += (int32_t)(((int64_t)dot * light_d[l]) >> 16);
	}
	if (d > 65536) d = 65536;
	return ma + (int32_t)(((int64_t)(md - ma) * d) >> 16);
}

/* a DRAW: its arguments and chunk, from the stream */
static int draw(void) {
	uint32_t nv, nt, nl, sh, fl, i, h0, h1;
	int32_t didx, dn[3], ma, md;
	int32_t lnx = 0, lny = 0, lnz = 0, lci = 0;      /* the last normal lit */
	didx = (int32_t)t2io_in();
	dn[0] = (int32_t)t2io_in(); dn[1] = (int32_t)t2io_in(); dn[2] = (int32_t)t2io_in();
	h0 = t2io_in(); h1 = t2io_in();
	nv = h0 & 0xffff; nt = h0 >> 16;
	sh = h1 & 0xff; fl = (h1 >> 8) & 0xff; nl = h1 >> 16;
	ma = (int32_t)t2io_in(); md = (int32_t)t2io_in();
	if (!(fl & T2_C_MAT)) { ma = mat_a; md = mat_d; }
	/* a textured chunk: its texture's word, and s | t a vertex */
	if (fl & T2_C_TEX) tex_word = t2io_in();
#ifndef T2_NO_TEX
	tri_tex = tex_on && (fl & T2_C_TEX);
#endif
	if (nv > T2_CHUNK_V) return -1;
	STAT(draws);
	for (i = 0; i < nv; i++) {
		vtx_t *v = &vb[i];
		uint32_t p0 = t2io_in(), p1 = t2io_in(), p2 = t2io_in(), p3 = t2io_in();
		if (fl & T2_C_TEX) {
			uint32_t p4 = t2io_in();
#ifndef T2_NO_TEX
			v->s = (int32_t)(int16_t)(p4 & 0xffff);
			v->t = (int32_t)(int16_t)(p4 >> 16);
#else
			(void)p4;     /* an engine without textures: drawn in colours */
#endif
		}
		int32_t x = (int32_t)(int16_t)(p0 & 0xffff) * (1 << sh);
		int32_t y = (int32_t)(int16_t)(p0 >> 16) * (1 << sh);
		int32_t z = (int32_t)(int16_t)(p1 & 0xffff) * (1 << sh);
		int32_t nx = dn[0], ny = dn[1], nz = dn[2];
		if (fl & T2_C_NORMAL) {
			nx = (int32_t)(int16_t)(p2 & 0xffff) * 4;
			ny = (int32_t)(int16_t)(p2 >> 16) * 4;
			nz = (int32_t)(int16_t)(p3 & 0xffff) * 4;
		}
#define ROW(r) (int32_t)((((int64_t)m[r] * x + (int64_t)m[4 + r] * y + \
	(int64_t)m[8 + r] * z) + (1 << 15)) >> 16) + m[12 + r] * 16
		v->x = ROW(0);
		v->y = ROW(1);
		v->z = (persp && !depth) ? 0 : ROW(2);
		v->w = ROW(3);
#undef ROW
		v->out = out_code(v);
		v->sv = 0;
		/* a chunk's vertices often share their normal (a world's face
		 * set has one): lit once */
		if (lighting) {
			/* (a positional light: every vertex, it depends on where) */
			if (pl_any) lci = lit_pt(nx, ny, nz, ma, md, x, y, z);
			else if (i == 0 || nx != lnx || ny != lny || nz != lnz) {
				lci = lit(nx, ny, nz, ma, md);
				lnx = nx; lny = ny; lnz = nz;
			}
			v->ci = lci;
		}
		else v->ci = (fl & T2_C_INDEX) ? (int32_t)(p1 >> 16) : didx;
		if (!v->out) screen_of(v);
		STAT(verts);
	}
	for (i = 0; i < nt; i++) {
		uint32_t p0 = t2io_in(), p1 = t2io_in();
		uint32_t a = p0 & 0xffff, b = p0 >> 16, c = p1 & 0xffff, pv = p1 >> 16;
		if (a >= nv || b >= nv || c >= nv || pv >= nv) return -1;
		geom_tri(&vb[a], &vb[b], &vb[c], vb[pv].ci);
	}
	for (i = 0; i < nl; i++) {
		uint32_t p0 = t2io_in();
		uint32_t a = p0 & 0xffff, b = p0 >> 16;
		if (a >= nv || b >= nv) return -1;
		geom_line(&vb[a], &vb[b]);
	}
	return 0;
}

static void get_s(svtx_t *o) {
	o->x = (int32_t)t2io_in(); o->y = (int32_t)t2io_in(); o->z = (int32_t)t2io_in(); o->ci = (int32_t)t2io_in();
}

int t2fw_step(void) {
	uint32_t h = t2io_in(), k;
	switch (T2_OP(h)) {
	case T2_FRAME: {
		uint32_t a = t2io_in(), b = t2io_in();
		fx0 = (int32_t)(a & 0xffff); fx1 = (int32_t)(a >> 16);
		fy0 = (int32_t)(b & 0xffff); fy1 = (int32_t)(b >> 16);
		if (fx1 > 319) fx1 = 319;
		if (fy1 > 239) fy1 = 239;
		t2io_frame(fx0, fx1, fy0, fy1, (uint8_t)(h & 0xff));
		return 0;
	}
	case T2_VIEW: {
		uint32_t a = t2io_in(), b = t2io_in();
		vp_x = (int32_t)(int16_t)(a & 0xffff); vp_y = (int32_t)(int16_t)(a >> 16);
		vp_w = (int32_t)(b & 0xffff); vp_h = (int32_t)(b >> 16);
		return 0;
	}
	case T2_XFORM:
		persp = h & 1;
		for (k = 0; k < 16; k++) m[k] = (int32_t)t2io_in();
		near_w = (int32_t)t2io_in(); far_w = (int32_t)t2io_in();
		return 0;
	case T2_LIGHT:
		lighting = h & 1; light_on[0] = (h >> 1) & 1; light_on[1] = (h >> 2) & 1;
		for (k = 0; k < 2; k++) {
			light_dir[k][0] = (int32_t)t2io_in();
			light_dir[k][1] = (int32_t)t2io_in();
			light_dir[k][2] = (int32_t)t2io_in();
			light_d[k] = (int32_t)t2io_in();
		}
		mat_a = (int32_t)t2io_in(); mat_d = (int32_t)t2io_in();
		return 0;
	case T2_PLIGHT:
		k = h & 1;
		pl_on[k] = (h >> 1) & 1;
		pl_unit[k] = (h >> 2) & 1;
		pl_p[k][0] = (int32_t)t2io_in(); pl_p[k][1] = (int32_t)t2io_in(); pl_p[k][2] = (int32_t)t2io_in();
		pl_s[k] = (int32_t)t2io_in();
		pl_kc[k] = (int32_t)t2io_in(); pl_kl[k] = (int32_t)t2io_in(); pl_kq[k] = t2io_in();
		pl_any = pl_on[0] || pl_on[1];
		return 0;
	case T2_STATE:
		depth = h & 1; smooth = (h >> 1) & 1; cull = (int)((h >> 2) & 3); front_ccw = (h >> 4) & 1;
		tex_on = (h >> 5) & 1;
		return 0;
	case T2_DRAW:
		return draw();
	case T2_TRI: {
		svtx_t a, b, c;
		get_s(&a); get_s(&b); get_s(&c);
		STAT(cpu_tris);
		tri_tex = false;
		rec_tri(&a, &b, &c, h & 1);
		return 0;
	}
	case T2_LINE: {
		svtx_t a, b;
		get_s(&a); get_s(&b);
		STAT(lines);
		rec_line(&a, &b, h & 1);
		return 0;
	}
	case T2_END:
		t2io_end();
		return 0;
	default:
		return -1;
	}
}
