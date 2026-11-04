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

typedef struct {
	int32_t x, y, z, w;      /* clip, 20.12 */
	int32_t ci;              /* 8.8 */
	int32_t sx, sy, sz;      /* picture 24.8, depth 16.15 */
	uint8_t out, sv;
} vtx_t;

typedef struct { int32_t x, y, z, ci; } svtx_t;   /* 28.4, as zgl_svtx_t */

/* the state */
static int32_t fx0, fx1, fy0, fy1;               /* the frame's rectangle */
static int32_t vp_x, vp_y, vp_w, vp_h;
static int32_t m[16], near_w, far_w;
static bool persp;
static bool lighting, light_on[2], depth, smooth, front_ccw;
static int cull;
static int32_t light_dir[2][3], light_d[2], mat_a, mat_d;
static vtx_t vb[T2_CHUNK_V];                     /* the vertex buffer */

t2_stats_t t2fw_stats;
#define ST t2fw_stats

static void vcopy(vtx_t *d, const vtx_t *s) {
	d->x = s->x; d->y = s->y; d->z = s->z; d->w = s->w; d->ci = s->ci;
	d->sx = s->sx; d->sy = s->sy; d->sz = s->sz; d->out = s->out; d->sv = s->sv;
}

/* -- zgl's arithmetic ------------------------------------------------------- */

/* n / d as C divides int64_t (toward zero), without libgcc's 2 KB: the
 * 32-bit divide when both fit (the common case), else bit by bit */
static int64_t sdiv64(int64_t n, int64_t d) {
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

static int32_t plane_d1(const vtx_t *v, int p) {
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

static void lerp(vtx_t *o, const vtx_t *a, const vtx_t *b, int64_t t) {
	o->x = a->x + (int32_t)(((int64_t)(b->x - a->x) * t) >> 30);
	o->y = a->y + (int32_t)(((int64_t)(b->y - a->y) * t) >> 30);
	o->z = a->z + (int32_t)(((int64_t)(b->z - a->z) * t) >> 30);
	o->w = a->w + (int32_t)(((int64_t)(b->w - a->w) * t) >> 30);
	o->ci = a->ci + (int32_t)(((int64_t)(b->ci - a->ci) * t) >> 30);
	o->out = 0;
	o->sv = 0;
}

static int facing(vtx_t *a, vtx_t *b, vtx_t *c) {
	int64_t d;
	if (a->w > 0 && b->w > 0 && c->w > 0) {
		int32_t ax, ay, bx, by, cx, cy;
		if (a->sv) { ax = a->sx; ay = a->sy; } else project(a, &ax, &ay);
		if (b->sv) { bx = b->sx; by = b->sy; } else project(b, &bx, &by);
		if (c->sv) { cx = c->sx; cy = c->sy; } else project(c, &cx, &cy);
		d = -((int64_t)(bx - ax) * (cy - ay) - (int64_t)(by - ay) * (cx - ax));
	} else {
		int64_t ax = a->x >> 12, ay = a->y >> 12, aw = a->w >> 12;
		int64_t bx = b->x >> 12, by = b->y >> 12, bw = b->w >> 12;
		int64_t cx = c->x >> 12, cy = c->y >> 12, cw = c->w >> 12;
		d = ax * (by * cw - bw * cy) - ay * (bx * cw - bw * cx) + aw * (bx * cy - by * cx);
	}
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
	w = t2io_rec(T2_REC_WORDS);
	if (!w) { ST.overflows++; return; }
	ST.drawn++;
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
	w[0] = T2_REC_TRI | (dep ? 4u : 0u) | (area > 0 ? 8u : 0u) |
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
}

/* zgl_frames.c's draw_line(), as a record: every step's numbers */
static void rec_line(const svtx_t *a, const svtx_t *b, bool dep) {
	int32_t dx = b->x - a->x, dy = b->y - a->y;
	int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
	int32_t n = ((adx > ady ? adx : ady) + 15) >> 4;
	int32_t ymin = a->y < b->y ? a->y : b->y, ymax = a->y < b->y ? b->y : a->y;
	int32_t ylo = clampi((ymin + 8) >> 4, fy0, fy1 + 1), yhi = clampi((ymax + 8) >> 4, fy0 - 1, fy1);
	volatile uint32_t *w;
	if (ylo > yhi) return;
	if (n == 0) n = 1;
	w = t2io_rec(10);
	if (!w) { ST.overflows++; return; }
	ST.drawn++;
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
}

/* -- geometry --------------------------------------------------------------- */

#define CLIP_MAX 12

static void geom_tri(vtx_t *a, vtx_t *b, vtx_t *c, int32_t flat_ci) {
	int f;
	bool front, flat = !smooth;
	svtx_t s0, s1, s2;
	ST.tris++;
	if (a->out & b->out & c->out) { ST.culled++; return; }
	f = facing(a, b, c);
	front = front_ccw ? (f >= 0) : (f <= 0);
	if (cull == 3 || (cull == 1 && !front) || (cull == 2 && front)) { ST.culled++; return; }
	if (!(a->out | b->out | c->out)) {
		if (!a->sv) screen_of(a);
		if (!b->sv) screen_of(b);
		if (!c->sv) screen_of(c);
		to_s(a, &s0, flat, flat_ci); to_s(b, &s1, flat, flat_ci); to_s(c, &s2, flat, flat_ci);
		rec_tri(&s0, &s1, &s2, depth);
		return;
	}
	/* Sutherland-Hodgman, as zgl's clip_polygon() */
	{
		static vtx_t buf[2][CLIP_MAX];
		vtx_t *in = buf[1], *out;
		uint8_t any = a->out | b->out | c->out;
		int n = 3, i, p, mm, cur = 0;
		ST.clipped++;
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
static void geom_line(vtx_t *a, vtx_t *b) {
	vtx_t p, q;
	svtx_t sa, sb;
	ST.lines++;
	if (a->out & b->out) { ST.culled++; return; }
	if (!(a->out | b->out)) { vcopy(&p, a); vcopy(&q, b); }
	else {
		int64_t t0 = 0, t1 = T_ONE;
		int i;
		ST.clipped++;
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
	didx = (int32_t)t2io_in();
	dn[0] = (int32_t)t2io_in(); dn[1] = (int32_t)t2io_in(); dn[2] = (int32_t)t2io_in();
	h0 = t2io_in(); h1 = t2io_in();
	nv = h0 & 0xffff; nt = h0 >> 16;
	sh = h1 & 0xff; fl = (h1 >> 8) & 0xff; nl = h1 >> 16;
	ma = (int32_t)t2io_in(); md = (int32_t)t2io_in();
	if (!(fl & T2_C_MAT)) { ma = mat_a; md = mat_d; }
	if (nv > T2_CHUNK_V) return -1;
	ST.draws++;
	for (i = 0; i < nv; i++) {
		vtx_t *v = &vb[i];
		uint32_t p0 = t2io_in(), p1 = t2io_in(), p2 = t2io_in(), p3 = t2io_in();
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
		if (lighting) v->ci = lit(nx, ny, nz, ma, md);
		else v->ci = (fl & T2_C_INDEX) ? (int32_t)(p1 >> 16) : didx;
		if (!v->out) screen_of(v);
		ST.verts++;
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
	case T2_STATE:
		depth = h & 1; smooth = (h >> 1) & 1; cull = (int)((h >> 2) & 3); front_ccw = (h >> 4) & 1;
		return 0;
	case T2_DRAW:
		return draw();
	case T2_TRI: {
		svtx_t a, b, c;
		get_s(&a); get_s(&b); get_s(&c);
		ST.cpu_tris++;
		rec_tri(&a, &b, &c, h & 1);
		return 0;
	}
	case T2_LINE: {
		svtx_t a, b;
		get_s(&a); get_s(&b);
		ST.lines++;
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
