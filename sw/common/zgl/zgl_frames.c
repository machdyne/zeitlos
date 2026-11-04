/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zgl_frames.c -- ZGL_FRAMES in software: whole frames, filled,
 * shaded and depth-tested, on every board. docs/zgl.md.
 *
 * -- Strips --
 *
 * A frame is the triangles and lines submitted since the last
 * zgl_frame(), kept in screen space (front end: zgl.c). At the end of
 * the frame the viewport is drawn in horizontal strips of 16 rows: for
 * each strip, a colour buffer and a 16-bit depth buffer of 320 x 16
 * are cleared, every primitive that touches the strip is drawn into
 * them, and the strip goes out to the picture at once. So there is no
 * frame-sized buffer anywhere -- the strip's 15 KB stay close in the
 * cache -- and the triangle engine (Phase 4) can draw the very same
 * strips on chip, this file being its pixel-exact reference.
 *
 * -- Rasterization --
 *
 * Pixel centres are at whole picture coordinates; vertices come with
 * 4 bits of fraction (28.4). A pixel is drawn when its centre is
 * inside, with the top-left rule on the edges (a centre exactly on a
 * top or left edge is in, on a bottom or right edge out), so triangles
 * that share an edge draw each pixel once. Depth (0..65535 with 15 bits
 * of fraction, 0 near) and
 * the colour index (8.8) are planes over the triangle: a gradient per
 * pixel across a span, the start of each span from the plane.
 *
 * -- Colour --
 *
 * Index mode, as the arcade runtime's palette: a fractional index from
 * smooth shading or lighting is dithered (4x4 ordered) between the two
 * entries either side, so a ramp of 8 or 16 palette entries shades
 * smoothly. In monochrome each index's luminance (from the palette)
 * is an ordered-dither level instead.
 *
 * -- Out to the picture --
 *
 * Colour (full screen, the colour window): the strip's bytes become n
 * bitplanes (a table spreads a byte's bits a byte apart, eight pixels
 * merge with shifts) and go out a plane at a time with ar_plane() --
 * one blitter pass each. Monochrome: one 1 bpp bitmap, ar_bitmap_opaque().
 */

#pragma GCC optimize ("O2")

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "zgl.h"
#include "zgl_int.h"
#include "../games/zarcade.h"

#define STRIP_H   16
#define ROW_BYTES (ZGL_W / 8)            /* 40: one plane row */

typedef struct {
	uint8_t kind;                        /* 0 triangle, 1 line */
	uint8_t depth;                       /* GL_DEPTH_TEST when submitted */
	bool short_right;                    /* triangle: v1 right of v0-v2 */
	zgl_svtx_t v[3];                     /* triangle: sorted by y */
	int32_t dzdx, dzdy, dcdx, dcdy;      /* triangle: the planes' gradients */
} zf_prim_t;

static zf_prim_t *zf_prims;
/* each primitive's rows (first, last) it can touch, apart from it: a
 * strip looks at every primitive's, and these stay in the data cache
 * where the primitives themselves (70 bytes each) would not */
static int16_t (*zf_yr)[2];
static int zf_n, zf_max;
static bool zf_open;
static uint8_t zf_bits;
static uint8_t zf_chunky;               /* 0, 1 software chunky, 2 the engine, 3 T2 */
static bool zf_engine;                  /* the engine, for a window's frames */

/* the runtime's, which a host test need not have */
#pragma weak ar_window_target
static zgl_info_t *zf_info;

static uint8_t  zf_col[STRIP_H * ZGL_W] __attribute__((aligned(4)));
static uint16_t zf_z[STRIP_H * ZGL_W];
static uint8_t  zf_out[8][STRIP_H * ROW_BYTES] __attribute__((aligned(4)));

/* what each row of the strip has had drawn, columns x0..x1 (x0 > x1:
 * nothing); outside that a row is the clear colour, and is neither
 * cleared again nor converted, only filled with the clear's bytes */
static int16_t zf_rx0[STRIP_H], zf_rx1[STRIP_H];
static int zf_clear = -1;                 /* the colour zf_col is clear to */
static uint8_t zf_lum[256];                              /* 0..16 */
/* monochrome: each index's dither byte for each row of the 8x8 matrix
 * (bit i the pixel at column i mod 8), made from zf_lum each frame */
static uint8_t zf_mpat[8][256];

/* 4x4 ordered dither, 8..248: added to an 8.8 index before dropping
 * the fraction */
static const uint8_t bayer4[4][4] = {
	{   8, 136,  40, 168 },
	{ 200,  72, 232, 104 },
	{  56, 184,  24, 152 },
	{ 248, 120, 216,  88 }
};

/* 8x8, 0..63: monochrome levels (a luminance of 0..16, times 4) */
static const uint8_t bayer8[8][8] = {
	{  0, 32,  8, 40,  2, 34, 10, 42 },
	{ 48, 16, 56, 24, 50, 18, 58, 26 },
	{ 12, 44,  4, 36, 14, 46,  6, 38 },
	{ 60, 28, 52, 20, 62, 30, 54, 22 },
	{  3, 35, 11, 43,  1, 33,  9, 41 },
	{ 51, 19, 59, 27, 49, 17, 57, 25 },
	{ 15, 47,  7, 39, 13, 45,  5, 37 },
	{ 63, 31, 55, 23, 61, 29, 53, 21 }
};

bool zglf_open(const zgl_desc_t *d, zgl_info_t *info) {
	int i;
	zf_max = d->max_prims ? d->max_prims : 2048;
	zf_prims = malloc((size_t)zf_max * sizeof(zf_prim_t));
	zf_yr = malloc((size_t)zf_max * sizeof(*zf_yr));
	if (!zf_prims || !zf_yr) { free(zf_prims); free(zf_yr); zf_prims = NULL; zf_yr = NULL; return false; }
	zf_n = 0;
	zf_bits = d->index_bits;
	zf_info = info;
	zf_chunky = 0;
	zf_engine = false;
	if ((d->flags & ZGL_CHUNKY) && zglc_open && zglc_strip) {
		int e = zglc_open(d->flags);
		zf_chunky = e == 2 && zgl_t2_open && zgl_t2_open(info) ? 3 : e ? 2 : 1;
		if (zf_chunky == 2) info->backend = ZGL_BACKEND_ENGINE;
		if (zf_chunky == 3) info->backend = ZGL_BACKEND_T2;
	} else if (zglc_probe && zglc_mono && zglc_put && zglc_finish)
		zf_engine = zglc_probe(d->flags);
	for (i = 0; i < STRIP_H; i++) { zf_rx0[i] = ZGL_W; zf_rx1[i] = -1; }
	zf_clear = -1;
	zf_open = true;
	return true;
}

void zglf_close(void) {
	if (zf_chunky == 3) zgl_t2_close();
	free(zf_prims);
	free(zf_yr);
	zf_prims = NULL;
	zf_yr = NULL;
	zf_open = false;
}

/* -- submission (from the front end) ------------------------------------- */

static zf_prim_t *new_prim(void) {
	if (zf_n >= zf_max) { zf_info->overflows++; return NULL; }
	return &zf_prims[zf_n++];
}

static int16_t row_lo(int32_t y) {      /* first row at or below 28.4 y */
	int32_t r = (y + 15) >> 4;
	return (int16_t)(r < -1 ? -1 : (r > ZGL_H ? ZGL_H : r));
}

/* num / den for the gradients (den != 0). libgcc's 64-bit divide is a
 * long call; when both fit 32 bits the hardware's divide does it
 * exactly, and otherwise (depth, mostly) a reciprocal of den to 16
 * bits does: a gradient a 2^-15 part out moves a triangle's depth or
 * colour by that part of its own range, well inside a step of either. */
ZGL_HOT static int32_t div_s64(int64_t num, int64_t den) {
	uint64_t un, ud;
	uint32_t inv;
	int s = 0, neg;
	int64_t q;
	if (num >= -0x7fffffffLL && num <= 0x7fffffffLL && den >= -0x7fffffffLL && den <= 0x7fffffffLL)
		return (int32_t)num / (int32_t)den;
	neg = (num < 0) != (den < 0);
	un = num < 0 ? (uint64_t)-num : (uint64_t)num;
	ud = den < 0 ? (uint64_t)-den : (uint64_t)den;
	while (ud >= 65536) { ud >>= 1; s++; }          /* ud in [2^15, 2^16) */
	while (ud < 32768) { ud <<= 1; s--; }
	inv = (uint32_t)((1u << 31) / (uint32_t)ud);    /* 2^31 / ud: 2^15..2^16 */
	while (un >= (1ULL << 46)) { un >>= 1; s--; }
	/* num / den = un * inv / 2^(31 + s) */
	if (31 + s <= 0) q = INT32_MAX;
	else if (31 + s >= 63) q = 0;
	else q = (int64_t)((un * inv) >> (31 + s));
	if (q > INT32_MAX) q = INT32_MAX;
	return (int32_t)(neg ? -q : q);
}

/* A triangle is set up once, here: sorted by y, its facing for the
 * walk, the gradients of depth and colour. A strip draws its rows.
 * The corners are already in the primitive, in y order; false if it
 * draws nothing (and the primitive is given back). */
ZGL_HOT static bool tri_setup(zf_prim_t *p) {
	const zgl_svtx_t *a = &p->v[0], *b = &p->v[1], *c = &p->v[2];
	int64_t area, a1, a2, dy1, dy2, dx1, dx2;
	area = (int64_t)(b->x - a->x) * (c->y - a->y) -
	       (int64_t)(c->x - a->x) * (b->y - a->y);
	if (area == 0 || ((a->y + 15) >> 4) >= ((c->y + 15) >> 4)) {   /* no row's centre */
		zf_n--;
		return false;
	}
	p->kind = 0;
	p->depth = zgl_s.depth_test;
	p->short_right = area > 0;
	zf_yr[p - zf_prims][0] = row_lo(a->y);
	zf_yr[p - zf_prims][1] = (int16_t)(row_lo(c->y) - 1);
	/* per pixel (16 sub-units): z (16.15), ci (8.8) */
	dy1 = b->y - a->y; dy2 = c->y - a->y;
	dx1 = b->x - a->x; dx2 = c->x - a->x;
	a1 = b->z - a->z; a2 = c->z - a->z;
	p->dzdx = div_s64((a1 * dy2 - a2 * dy1) * 16, area);
	p->dzdy = div_s64((a2 * dx1 - a1 * dx2) * 16, area);
	a1 = b->ci - a->ci; a2 = c->ci - a->ci;
	if (a1 == 0 && a2 == 0) { p->dcdx = 0; p->dcdy = 0; }
	else {
		p->dcdx = div_s64((a1 * dy2 - a2 * dy1) * 16, area);
		p->dcdy = div_s64((a2 * dx1 - a1 * dx2) * 16, area);
	}
	return true;
}

void zglf_tri(const zgl_svtx_t *a, const zgl_svtx_t *b, const zgl_svtx_t *c) {
	zf_prim_t *p;
	const zgl_svtx_t *t;
	if (zf_chunky == 3) { zgl_t2_tri(a, b, c); return; }
	if (b->y < a->y) { t = a; a = b; b = t; }
	if (c->y < b->y) { t = b; b = c; c = t; }
	if (b->y < a->y) { t = a; a = b; b = t; }
	p = new_prim();
	if (!p) return;
	p->v[0] = *a; p->v[1] = *b; p->v[2] = *c;
	tri_setup(p);
}

/* From projected vertices: each corner written once, into the
 * primitive (a store costs on a board that writes through) */
static inline void put_corner(zgl_svtx_t *o, const zgl_vtx_t *v, bool flat, int32_t ci) {
	o->x = v->sx >> 4;
	o->y = v->sy >> 4;
	o->z = v->sz;
	o->ci = flat ? ci : v->ci;
}

ZGL_HOT void zglf_tri_v(const zgl_vtx_t *a, const zgl_vtx_t *b, const zgl_vtx_t *c, bool flat, int32_t ci) {
	zf_prim_t *p;
	const zgl_vtx_t *t;
	if (zf_chunky == 3) {
		zgl_svtx_t s[3];
		put_corner(&s[0], a, flat, ci); put_corner(&s[1], b, flat, ci); put_corner(&s[2], c, flat, ci);
		zgl_t2_tri(&s[0], &s[1], &s[2]);
		return;
	}
	if ((b->sy >> 4) < (a->sy >> 4)) { t = a; a = b; b = t; }
	if ((c->sy >> 4) < (b->sy >> 4)) { t = b; b = c; c = t; }
	if ((b->sy >> 4) < (a->sy >> 4)) { t = a; a = b; b = t; }
	p = new_prim();
	if (!p) return;
	put_corner(&p->v[0], a, flat, ci);
	put_corner(&p->v[1], b, flat, ci);
	put_corner(&p->v[2], c, flat, ci);
	tri_setup(p);
}

void zglf_line(const zgl_svtx_t *a, const zgl_svtx_t *b) {
	zf_prim_t *p;
	int32_t ymin = a->y < b->y ? a->y : b->y, ymax = a->y < b->y ? b->y : a->y;
	if (zf_chunky == 3) { zgl_t2_line(a, b); return; }
	p = new_prim();
	if (!p) return;
	p->kind = 1;
	p->depth = zgl_s.depth_test;
	zf_yr[p - zf_prims][0] = (int16_t)((ymin + 8) >> 4);
	zf_yr[p - zf_prims][1] = (int16_t)((ymax + 8) >> 4);
	p->v[0] = *a; p->v[1] = *b;
}

/* -- one strip --------------------------------------------------------------- */

typedef struct {
	int32_t sy0, sy1;          /* rows of the strip, inclusive */
	int32_t cx0, cx1;          /* columns: the viewport and scissor */
} zf_strip_t;

/* An edge, walked a row at a time, exactly: for each row the first
 * pixel whose centre is at or right of the edge (ceil of the crossing,
 * in pixels), kept as a quotient and remainder so that no row divides.
 * num = xa * dy + (Y - ya) * dx is the crossing times dy; q = ceil(num /
 * D), D = 16 dy, and num grows by S = 16 dx a row. */
typedef struct {
	int32_t q, r;          /* ceil(num / D), q * D - num (0 <= r < D) */
	int32_t qs, rs, D;     /* S = qs * D + rs, 0 <= rs < D */
} zf_edge_t;

__attribute__((noinline)) static void edge_init64(zf_edge_t *e, const zgl_svtx_t *a, const zgl_svtx_t *b, int32_t row) {
	int64_t dy = b->y - a->y, dx = b->x - a->x;
	int64_t D = 16 * dy, S = 16 * dx;
	int64_t num = (int64_t)a->x * dy + ((int64_t)row * 16 - a->y) * dx;
	int64_t q, qs;
	q = num >= 0 ? (num + D - 1) / D : -((-num) / D);          /* ceil */
	qs = S >= 0 ? S / D : -((-S + D - 1) / D);                 /* floor */
	e->q = (int32_t)q;
	e->r = (int32_t)(q * D - num);
	e->qs = (int32_t)qs;
	e->rs = (int32_t)(S - qs * D);
	e->D = (int32_t)D;
}

/* inlined, so that a caller's edges can live in registers (the 64-bit
 * way's result is copied in) */
static inline __attribute__((always_inline)) void edge_init(zf_edge_t *e, const zgl_svtx_t *a, const zgl_svtx_t *b, int32_t row) {
	int32_t dy32 = b->y - a->y, dx32 = b->x - a->x;
	/* on the screen (the front end clips to the view volume) every term
	 * fits 32 bits: x, dx < 2^14, dy < 2^12 */
	if (dx32 > -8192 && dx32 < 8192 && dy32 < 8192 && a->x > -8192 && a->x < 8192 &&
	    row * 16 - a->y > -8192 && row * 16 - a->y < 8192) {
		int32_t D = 16 * dy32, S = 16 * dx32, q, qs;
		int32_t num = a->x * dy32 + (row * 16 - a->y) * dx32;
		q = num >= 0 ? (num + D - 1) / D : -((-num) / D);
		qs = S >= 0 ? S / D : -((-S + D - 1) / D);
		e->q = q; e->r = q * D - num;
		e->qs = qs; e->rs = S - qs * D; e->D = D;
		return;
	}
	{
		zf_edge_t t;
		edge_init64(&t, a, b, row);
		*e = t;
	}
}

static inline void edge_step(zf_edge_t *e) {
	e->q += e->qs;
	e->r -= e->rs;
	if (e->r < 0) { e->r += e->D; e->q++; }
}

/* a triangle's rows inside the strip */
__attribute__((noinline)) static void draw_tri(const zf_strip_t *st, const zf_prim_t *p) {
	const zgl_svtx_t *v0 = &p->v[0], *v1 = &p->v[1], *v2 = &p->v[2];
	int32_t dzdx = p->dzdx, dzdy = p->dzdy, dcdx = p->dcdx, dcdy = p->dcdy;
	int32_t y, ylo, yhi, ymid;
	zf_edge_t el, es;              /* the long edge, the short one in use */
	bool short_right = p->short_right;
	ylo = (v0->y + 15) >> 4;                /* top: a centre on the edge is in */
	yhi = ((v2->y + 15) >> 4) - 1;          /* bottom: out */
	ymid = (v1->y + 15) >> 4;               /* first row of the lower half */
	if (ylo < st->sy0) ylo = st->sy0;
	if (yhi > st->sy1) yhi = st->sy1;
	if (ylo > yhi) return;
	edge_init(&el, v0, v2, ylo);
	if (ylo < ymid) edge_init(&es, v0, v1, ylo);
	else edge_init(&es, v1, v2, ylo);
	for (y = ylo; y <= yhi; y++) {
		int32_t Y = y * 16, x0, x1, x;
		int64_t z, ci;
		uint8_t *cb;
		uint16_t *zb;
		const uint8_t *dth;
		if (y == ymid && y != ylo) edge_init(&es, v1, v2, y);
		/* left edge: its first pixel is in; right edge: its first is out */
		if (short_right) { x0 = el.q; x1 = es.q - 1; }
		else { x0 = es.q; x1 = el.q - 1; }
		edge_step(&el);
		edge_step(&es);
		if (x0 < st->cx0) x0 = st->cx0;
		if (x1 > st->cx1) x1 = st->cx1;
		if (x0 > x1) continue;
		z = v0->z + (((int64_t)dzdx * (x0 * 16 - v0->x) + (int64_t)dzdy * (Y - v0->y)) >> 4);
		ci = v0->ci + (((int64_t)dcdx * (x0 * 16 - v0->x) + (int64_t)dcdy * (Y - v0->y)) >> 4);
		if (x0 < zf_rx0[y - st->sy0]) zf_rx0[y - st->sy0] = (int16_t)x0;
		if (x1 > zf_rx1[y - st->sy0]) zf_rx1[y - st->sy0] = (int16_t)x1;
		cb = &zf_col[(y - st->sy0) * ZGL_W];
		zb = &zf_z[(y - st->sy0) * ZGL_W];
		dth = bayer4[y & 3];
		{
			/* mod 2^32, as the engine (and the plane can wrap far off a
			 * thin triangle; the depth test then sees what it sees) */
			uint32_t zi = (uint32_t)z, cii = (uint32_t)ci;
			uint32_t dz = (uint32_t)dzdx, dc = (uint32_t)dcdx;
			if (p->depth) {
				int64_t zend = (int64_t)(int32_t)zi + (int64_t)dzdx * (x1 - x0);
				if ((int32_t)zi < 0 || zend < 0 || zend > INT32_MAX) {
					for (x = x0; x <= x1; x++) {
						uint32_t zv = (int32_t)zi < 0 ? 0 : zi >> 15;
						if (zv < zb[x]) {
							zb[x] = (uint16_t)zv;
							cb[x] = (uint8_t)((cii + dth[x & 3]) >> 8);
						}
						zi += dz;
						cii += dc;
					}
				} else if (dc == 0 && (cii & 0xff) == 0) {
					/* one index (flat, unlit): no dither */
					uint8_t c = (uint8_t)(cii >> 8);
					uint16_t *zp = &zb[x0], *ze = &zb[x1];
					uint8_t *cp = &cb[x0];
					for (; zp <= ze; zp++, cp++) {
						uint32_t zv = zi >> 15;
						if (zv < *zp) { *zp = (uint16_t)zv; *cp = c; }
						zi += dz;
					}
				} else {
					uint16_t *zp = &zb[x0], *ze = &zb[x1];
					uint8_t *cp = &cb[x0];
					for (x = x0; zp <= ze; zp++, cp++, x++) {
						uint32_t zv = zi >> 15;
						if (zv < *zp) {
							*zp = (uint16_t)zv;
							*cp = (uint8_t)((cii + dth[x & 3]) >> 8);
						}
						zi += dz;
						cii += dc;
					}
				}
			} else if (dc == 0 && (cii & 0xff) == 0) {
				memset(&cb[x0], (cii >> 8) & 0xff, (size_t)(x1 - x0 + 1));
			} else {
				for (x = x0; x <= x1; x++) {
					cb[x] = (uint8_t)((cii + dth[x & 3]) >> 8);
					cii += dc;
				}
			}
		}
	}
}

/* a line's pixels inside the strip: one a step along the longer axis */
__attribute__((noinline)) static void draw_line(const zf_strip_t *st, const zf_prim_t *p) {
	const zgl_svtx_t *a = &p->v[0], *b = &p->v[1];
	int32_t dx = b->x - a->x, dy = b->y - a->y;
	int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
	int32_t n = ((adx > ady ? adx : ady) + 15) >> 4, i;
	int32_t i0 = 0, i1;
	int64_t sx, sy, sz, sc, x, y, z, c;
	if (n == 0) n = 1;
	i1 = n;
	/* only the steps whose row can be in the strip (two steps' margin
	 * either side; the test below is exact): a long line is walked a
	 * strip's worth at a time, not whole for every strip */
	if (ady > 16) {
		int32_t ra = st->sy0 * 16 - 8 - a->y, rb = (st->sy1 + 1) * 16 - 8 - a->y;
		int32_t ta = ra * n / dy, tb = rb * n / dy;
		i0 = (ta < tb ? ta : tb) - 2;
		i1 = (ta < tb ? tb : ta) + 2;
		if (i0 < 0) i0 = 0;
		if (i1 > n) i1 = n;
	}
	sx = ((int64_t)dx * 65536) / n; sy = ((int64_t)dy * 65536) / n;
	sz = ((int64_t)(b->z - a->z)) / n; sc = ((int64_t)(b->ci - a->ci)) / n;
	x = (int64_t)a->x * 65536 + sx * i0; y = (int64_t)a->y * 65536 + sy * i0;
	z = a->z + sz * i0; c = a->ci + sc * i0;
	for (i = i0; i <= i1; i++) {
		int32_t px = (int32_t)((x + (8 << 16)) >> 20), py = (int32_t)((y + (8 << 16)) >> 20);
		if (py >= st->sy0 && py <= st->sy1 && px >= st->cx0 && px <= st->cx1) {
			int o = (py - st->sy0) * ZGL_W + px;
			uint32_t zv = z < 0 ? 0 : (uint32_t)(z >> 15);
			if (!p->depth || zv <= zf_z[o]) {
				if (p->depth) zf_z[o] = (uint16_t)zv;
				zf_col[o] = (uint8_t)((c + 128) >> 8);
				if (px < zf_rx0[py - st->sy0]) zf_rx0[py - st->sy0] = (int16_t)px;
				if (px > zf_rx1[py - st->sy0]) zf_rx1[py - st->sy0] = (int16_t)px;
			}
		}
		x += sx; y += sy; z += sz; c += sc;
	}
}

/* -- out to the picture ------------------------------------------------------ */

static int planes_now(void) {
	int n = 0, shown;
	if (!ar_color()) return 0;
	shown = ar_colours();
	while ((1 << n) < shown) n++;
	if (zf_bits && zf_bits < n) n = zf_bits;
	return n ? n : 1;
}

/* Eight pixels' bytes to eight plane bytes: an 8x8 bit transpose
 * (pixel i's bit k to plane k's bit i), in three exchanges of 32-bit
 * words, about 30 instructions for all eight planes. */
static inline void c2p8(uint32_t *plo, uint32_t *phi) {
	uint32_t lo = *plo, hi = *phi, t;
	t = (lo ^ (lo >> 7)) & 0x00AA00AAu; lo ^= t ^ (t << 7);
	t = (hi ^ (hi >> 7)) & 0x00AA00AAu; hi ^= t ^ (t << 7);
	t = (lo ^ (lo >> 14)) & 0x0000CCCCu; lo ^= t ^ (t << 14);
	t = (hi ^ (hi >> 14)) & 0x0000CCCCu; hi ^= t ^ (t << 14);
	t = (lo ^ (hi << 4)) & 0xF0F0F0F0u; lo ^= t; hi ^= t >> 4;
	*plo = lo; *phi = hi;
}

__attribute__((noinline)) static void out_planes(const zf_strip_t *st, int rows, int np) {
	int r, g, k, g0 = st->cx0 >> 3, g1 = st->cx1 >> 3;
	uint8_t fill[8];
	for (k = 0; k < 8; k++) fill[k] = (zgl_s.clear_index >> k & 1) ? 0xff : 0;
	for (r = 0; r < rows; r++) {
		const uint32_t *src = (const uint32_t *)&zf_col[r * ZGL_W];
		int d0 = zf_rx0[r] >> 3, d1 = zf_rx1[r] >> 3;
		if (zf_rx0[r] > zf_rx1[r]) { d0 = g1 + 1; d1 = g1; }
		if (d0 < g0) d0 = g0;
		if (d1 > g1) d1 = g1;
		if (d0 > g0 || d1 < g1)
			for (k = 0; k < np; k++) {
				uint8_t *o = &zf_out[k][r * ROW_BYTES], f = fill[k];
				for (g = g0; g < d0; g++) o[g] = f;
				for (g = d1 + 1 > g0 ? d1 + 1 : g0; g <= g1; g++) o[g] = f;
			}
		for (g = d0; g <= d1; g++) {
			uint32_t lo = src[g * 2], hi = src[g * 2 + 1];
			int o = r * ROW_BYTES + g;
			c2p8(&lo, &hi);
			switch (np) {
			case 8: zf_out[7][o] = (uint8_t)(hi >> 24); /* fall through */
			case 7: zf_out[6][o] = (uint8_t)(hi >> 16); /* fall through */
			case 6: zf_out[5][o] = (uint8_t)(hi >> 8);  /* fall through */
			case 5: zf_out[4][o] = (uint8_t)hi;         /* fall through */
			case 4: zf_out[3][o] = (uint8_t)(lo >> 24); /* fall through */
			case 3: zf_out[2][o] = (uint8_t)(lo >> 16); /* fall through */
			case 2: zf_out[1][o] = (uint8_t)(lo >> 8);  /* fall through */
			default: zf_out[0][o] = (uint8_t)lo;
			}
		}
	}
	for (k = 0; k < np; k++)
		ar_plane(k, zf_out[k], ROW_BYTES, st->cx0, 0, st->cx0, st->sy0,
			st->cx1 - st->cx0 + 1, rows);
}

__attribute__((noinline)) static void out_mono(const zf_strip_t *st, int rows) {
	int r, x;
	for (r = 0; r < rows; r++) {
		const uint8_t *src = &zf_col[r * ZGL_W];
		const uint8_t *pat = zf_mpat[(st->sy0 + r) & 7];
		uint8_t *dst = &zf_out[0][r * ROW_BYTES];
		uint8_t fill = pat[zgl_s.clear_index];
		int d0 = zf_rx0[r] & ~7, d1 = zf_rx1[r];
		for (x = st->cx0 & ~7; x <= st->cx1; x += 8) {
			const uint8_t *p8 = &src[x];
			if (x < d0 || x > d1) { dst[x >> 3] = fill; continue; }
			dst[x >> 3] = (uint8_t)((pat[p8[0]] & 0x01) | (pat[p8[1]] & 0x02) |
				(pat[p8[2]] & 0x04) | (pat[p8[3]] & 0x08) | (pat[p8[4]] & 0x10) |
				(pat[p8[5]] & 0x20) | (pat[p8[6]] & 0x40) | (pat[p8[7]] & 0x80));
		}
	}
	ar_bitmap_opaque(zf_out[0], ROW_BYTES, st->cx0, 0, st->cx0, st->sy0,
		st->cx1 - st->cx0 + 1, rows);
}

/* each index's grey level, 0..16, from the palette (monochrome) */
static void make_lum(void) {
	int i, r8;
	for (i = 0; i < 256; i++) {
		uint16_t c = ar_pal_get(i);
		int r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
		zf_lum[i] = (uint8_t)((r * 5 + g * 9 + b * 2) * 16 / (16 * 15));
	}
	for (r8 = 0; r8 < 8; r8++) {
		uint8_t lv[17];
		int l, k;
		for (l = 0; l <= 16; l++) {
			uint8_t bits = 0;
			for (k = 0; k < 8; k++)
				if (l * 4 > bayer8[r8][k]) bits |= (uint8_t)(1 << k);
			lv[l] = bits;
		}
		for (i = 0; i < 256; i++) zf_mpat[r8][i] = lv[zf_lum[i]];
	}
}

/* the part of the picture a frame covers: the viewport, and the scissor */
void zglf_rect(int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1) {
	int32_t vx0 = zgl_s.vp_x, vy0 = zgl_s.vp_y;
	int32_t vx1 = zgl_s.vp_x + zgl_s.vp_w - 1, vy1 = zgl_s.vp_y + zgl_s.vp_h - 1;
	if (vx0 < 0) vx0 = 0;
	if (vy0 < 0) vy0 = 0;
	if (vx1 > ZGL_W - 1) vx1 = ZGL_W - 1;
	if (vy1 > ZGL_H - 1) vy1 = ZGL_H - 1;
	if (zgl_s.scissor) {
		if (zgl_s.sc_x0 > vx0) vx0 = zgl_s.sc_x0;
		if (zgl_s.sc_y0 > vy0) vy0 = zgl_s.sc_y0;
		if (zgl_s.sc_x1 < vx1) vx1 = zgl_s.sc_x1;
		if (zgl_s.sc_y1 < vy1) vy1 = zgl_s.sc_y1;
	}
	*x0 = vx0; *y0 = vy0; *x1 = vx1; *y1 = vy1;
}

void zglf_frame(void) {
	zf_strip_t st;
	int np = planes_now(), y0, i;
	int32_t vx0, vy0, vx1, vy1;
	if (!zf_open) return;
	if (zf_chunky == 2) { zglf_t_frame(zglc_put); return; }
	if (zf_chunky == 3) { zgl_t2_frame(); return; }
	zglf_rect(&vx0, &vy0, &vx1, &vy1);
	if (zf_chunky) np = 8;
	else if (!np) {
		int ox, oy;
		ar_rect_t v;
		make_lum();
		if (zf_engine && ar_window_target && ar_window_target(&ox, &oy, &v)) {
			/* a black-and-white window, one rectangle of it showing:
			 * the engine draws it there; done when this returns, so the
			 * program's text goes on top as before */
			int x0 = v.x - ox, y0 = v.y - oy, x1 = x0 + v.w - 1, y1 = y0 + v.h - 1;
			if (x0 < 0) x0 = 0;
			if (y0 < 0) y0 = 0;
			if (x1 > ZGL_W - 1) x1 = ZGL_W - 1;
			if (y1 > ZGL_H - 1) y1 = ZGL_H - 1;
			if (x0 <= x1 && y0 <= y1) {
				zglc_mono(ox, oy, x0, x1, y0, y1, zf_lum);
				zf_info->backend = ZGL_BACKEND_ENGINE;
				zglf_t_frame(zglc_put);
				zglc_finish();
				return;
			}
		}
		zf_info->backend = ZGL_BACKEND_SOFTWARE;
	}
	st.cx0 = vx0; st.cx1 = vx1;
	for (y0 = vy0; y0 <= vy1; y0 += STRIP_H) {
		int rows = (vy1 - y0 + 1) < STRIP_H ? (vy1 - y0 + 1) : STRIP_H;
		st.sy0 = y0;
		st.sy1 = y0 + rows - 1;
		if (zf_clear != zgl_s.clear_index) {
			memset(zf_col, zgl_s.clear_index, sizeof(zf_col));
			memset(zf_z, 0xff, sizeof(zf_z));
			zf_clear = zgl_s.clear_index;
		}
		for (i = 0; i < zf_n; i++) {
			const zf_prim_t *p;
			if (zf_yr[i][1] < st.sy0 || zf_yr[i][0] > st.sy1) continue;
			p = &zf_prims[i];
			if (p->kind == 0) draw_tri(&st, p);
			else draw_line(&st, p);
		}
		if (zf_chunky) zglc_strip(st.sy0, rows, st.cx0, st.cx1, zf_col);
		else if (np) out_planes(&st, rows, np);
		else out_mono(&st, rows);
		/* back to clear, where drawn */
		for (i = 0; i < rows; i++) {
			int x0 = zf_rx0[i], x1 = zf_rx1[i];
			if (x0 > x1) continue;
			memset(&zf_col[i * ZGL_W + x0], zf_clear, (size_t)(x1 - x0 + 1));
			memset(&zf_z[i * ZGL_W + x0], 0xff, (size_t)(x1 - x0 + 1) * 2);
			zf_rx0[i] = ZGL_W; zf_rx1[i] = -1;
		}
	}
	zf_info->lines = 0;
	zf_info->prims = (uint32_t)zf_n;
	zf_info->frames++;
	zf_n = 0;
}

/* -- the triangle engine's commands (rtl/gpu/gpu_tri.v, docs/gpu_tri.md) ---
 *
 * The engine draws the same strips as above, pixel for pixel, from
 * commands the CPU sets up for each strip: a triangle becomes one or
 * two trapezoids (its rows above and below the middle vertex), each
 * with its two edges already at its first row, and a line its steps
 * inside the strip. What is left to the engine is stepping and
 * pixels: no division, and one multiply a row (a plane at the span's
 * first pixel).
 *
 *   BEGIN  op 3 | first << 27 | y0 << 12 | clear << 4 | (rows - 1)
 *                      the strip, the frame's first or not, its
 *                      background index
 *          cx0 | cx1 << 16                        its columns
 *   TRAP   op 1 | depth << 27 | ya << 4 | yb      rows ya..yb of the strip
 *          left edge, right edge: 3 words each:
 *            q | qs << 16   r | rs << 16   D
 *          z at column 0 of row ya, dz/dx, dz/dy   (16.15, mod 2^32)
 *          ci at column 0 of row ya, dci/dx, dci/dy (8.8)
 *   LINE   op 2 | depth << 27 | (steps - 1)
 *          x, y (28.4 << 16, + 8 << 16), dx, dy a step,
 *          z, dz, ci, dci
 *   END    op 4                                   the strip is done
 *
 * A row's pixels run from the left edge's q to the right edge's q - 1,
 * inside cx0..cx1. The edges step as edge_step() does. A span's first
 * z is z0 + dz/dx * x (mod 2^32: the same low 32 bits as the 64-bit sum
 * above), and each row adds dz/dy; colour likewise. */

#define ZT_TRAP  1u
#define ZT_LINE  2u
#define ZT_BEGIN 3u
#define ZT_END   4u

/* where the commands go: the engine's command register, or put() */
static volatile uint32_t *zt_reg;
static void (*zt_put)(uint32_t);
#define ZT_PUT(w) do { uint32_t w_ = (w); if (zt_reg) *zt_reg = w_; else zt_put(w_); } while (0)

static inline __attribute__((always_inline)) void t_edge(const zf_edge_t *e) {
	ZT_PUT((uint32_t)(uint16_t)e->q | (uint32_t)(uint16_t)e->qs << 16);
	ZT_PUT((uint32_t)(uint16_t)e->r | (uint32_t)(uint16_t)e->rs << 16);
	ZT_PUT((uint32_t)e->D);
}

/* fits the engine's 16-bit edge fields (the front end clips to the
 * view, so a triangle's edges always do) */
static inline bool t_edge_ok(const zf_edge_t *e) {
	return e->q >= -32768 && e->q <= 32767 && e->qs >= -32768 && e->qs <= 32767 &&
	       e->D > 0 && e->D <= 65535 && e->r >= 0 && e->rs >= 0;
}

/* the plane a at column 0 of picture row y */
static uint32_t t_plane0(int32_t a0, int32_t dadx, int32_t dady, const zgl_svtx_t *v0, int32_t y) {
	int64_t k = -(int64_t)dadx * v0->x + (int64_t)dady * (y * 16 - v0->y);
	return (uint32_t)a0 + (uint32_t)(k >> 4);
}

static void t_trap(const zf_strip_t *st, const zf_prim_t *p,
		int32_t ya, int32_t yb, const zgl_svtx_t *sa, const zgl_svtx_t *sb) {
	const zgl_svtx_t *v0 = &p->v[0], *v2 = &p->v[2];
	zf_edge_t el, es;
	edge_init(&el, v0, v2, ya);
	edge_init(&es, sa, sb, ya);
	if (!t_edge_ok(&el) || !t_edge_ok(&es)) { zf_info->overflows++; return; }
	ZT_PUT(ZT_TRAP << 28 | (uint32_t)p->depth << 27 |
	    (uint32_t)(ya - st->sy0) << 4 | (uint32_t)(yb - st->sy0));
	if (p->short_right) { t_edge(&el); t_edge(&es); }
	else { t_edge(&es); t_edge(&el); }
	ZT_PUT(t_plane0(v0->z, p->dzdx, p->dzdy, v0, ya));
	ZT_PUT((uint32_t)p->dzdx);
	ZT_PUT((uint32_t)p->dzdy);
	ZT_PUT(t_plane0(v0->ci, p->dcdx, p->dcdy, v0, ya));
	ZT_PUT((uint32_t)p->dcdx);
	ZT_PUT((uint32_t)p->dcdy);
}

static void t_tri(const zf_strip_t *st, const zf_prim_t *p) {
	const zgl_svtx_t *v0 = &p->v[0], *v1 = &p->v[1], *v2 = &p->v[2];
	int32_t ylo = (v0->y + 15) >> 4, yhi = ((v2->y + 15) >> 4) - 1, ymid = (v1->y + 15) >> 4;
	if (ylo < st->sy0) ylo = st->sy0;
	if (yhi > st->sy1) yhi = st->sy1;
	if (ylo > yhi) return;
	if (ylo < ymid) t_trap(st, p, ylo, ymid - 1 < yhi ? ymid - 1 : yhi, v0, v1);
	if ((ymid > ylo ? ymid : ylo) <= yhi) t_trap(st, p, ymid > ylo ? ymid : ylo, yhi, v1, v2);
}

static void t_line(const zf_strip_t *st, const zf_prim_t *p) {
	/* draw_line()'s walk, as numbers */
	const zgl_svtx_t *a = &p->v[0], *b = &p->v[1];
	int32_t dx = b->x - a->x, dy = b->y - a->y;
	int32_t adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
	int32_t n = ((adx > ady ? adx : ady) + 15) >> 4;
	int32_t i0 = 0, i1;
	int64_t sx, sy, sz, sc;
	if (n == 0) n = 1;
	i1 = n;
	if (ady > 16) {
		int32_t ra = st->sy0 * 16 - 8 - a->y, rb = (st->sy1 + 1) * 16 - 8 - a->y;
		int32_t ta = ra * n / dy, tb = rb * n / dy;
		i0 = (ta < tb ? ta : tb) - 2;
		i1 = (ta < tb ? tb : ta) + 2;
		if (i0 < 0) i0 = 0;
		if (i1 > n) i1 = n;
	}
	if (i1 < i0 || i1 - i0 > 65535) return;
	sx = ((int64_t)dx * 65536) / n; sy = ((int64_t)dy * 65536) / n;
	sz = ((int64_t)(b->z - a->z)) / n; sc = ((int64_t)(b->ci - a->ci)) / n;
	ZT_PUT(ZT_LINE << 28 | (uint32_t)p->depth << 27 | (uint32_t)(i1 - i0));
	ZT_PUT((uint32_t)((int64_t)a->x * 65536 + sx * i0 + (8 << 16)));
	ZT_PUT((uint32_t)((int64_t)a->y * 65536 + sy * i0 + (8 << 16)));
	ZT_PUT((uint32_t)sx);
	ZT_PUT((uint32_t)sy);
	ZT_PUT((uint32_t)(a->z + sz * i0));
	ZT_PUT((uint32_t)sz);
	ZT_PUT((uint32_t)(a->ci + sc * i0));
	ZT_PUT((uint32_t)sc);
}

/* This frame as the engine's commands, strip by strip, to put(): what
 * zglf_frame() would draw. The submitted primitives are used up. */
void zglf_t_frame(void (*put)(uint32_t w)) {
	zf_strip_t st;
	int32_t vx0, vy0, vx1, vy1, y0;
	int i;
	if (!zf_open) return;
	/* to the engine: a store each, no call */
	zt_reg = put == zglc_put && zglc_cmd ? zglc_cmd() : NULL;
	zt_put = put;
	zglf_rect(&vx0, &vy0, &vx1, &vy1);
	st.cx0 = vx0; st.cx1 = vx1;
	for (y0 = vy0; y0 <= vy1; y0 += STRIP_H) {
		int rows = (vy1 - y0 + 1) < STRIP_H ? (vy1 - y0 + 1) : STRIP_H;
		st.sy0 = y0;
		st.sy1 = y0 + rows - 1;
		ZT_PUT(ZT_BEGIN << 28 | (uint32_t)(y0 == vy0) << 27 | (uint32_t)y0 << 12 |
		    (uint32_t)zgl_s.clear_index << 4 |
		    (uint32_t)(rows - 1));
		ZT_PUT((uint32_t)st.cx0 | (uint32_t)st.cx1 << 16);
		for (i = 0; i < zf_n; i++) {
			const zf_prim_t *p;
			if (zf_yr[i][1] < st.sy0 || zf_yr[i][0] > st.sy1) continue;
			p = &zf_prims[i];
			if (p->kind == 0) t_tri(&st, p);
			else t_line(&st, p);
		}
		ZT_PUT(ZT_END << 28);
	}
	zf_info->lines = 0;
	zf_info->prims = (uint32_t)zf_n;
	zf_info->frames++;
	zf_n = 0;
}
