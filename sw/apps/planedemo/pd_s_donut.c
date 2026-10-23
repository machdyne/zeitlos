/*
 * planedemo -- bars 13-16: the donut. The mouse's leap ends in a white
 * flash, and there it is: a glazed, sprinkled donut turning in the
 * night sky, shaded in 3D, flaring on the drums.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * 64 frames of a 176x176 torus are rendered while the storm plays --
 * points on the surface, a depth buffer, a light and a highlight,
 * dough below and pink glaze above with sprinkles -- a slice of a frame
 * at a time, in the time the storm leaves over. Playing them is six
 * memory blits a frame. Planes 0-5: the donut (64 colours: 0 none,
 * 1-24 dough, 25-48 glaze, 49-63 sprinkles). Planes 6-7: the sky
 * behind it, drawn once, which shows wherever the donut is 0.
 */

#include <stdlib.h>
#include <string.h>

#include "pd.h"
#include "pd_gfx.h"
#include "pd_scenes.h"

#define DW 176
#define NF 64                   /* frames: one turn */
#define NT 128                  /* steps round the tube */
#define NP 288                  /* steps round the ring */
#define SLICE 8                 /* tube steps a job step does */

static pd_planar_t frames[NF];
static pd_planar_t bg;
static pd_chunky_t buf;
static uint16_t *zbuf;
static uint8_t tex[NT][NP];     /* the surface: 0 dough, 1 glaze, 2.. sprinkles */
static int f_at = -1, t_at;     /* the frame being made, and how far */

static uint32_t seed = 0x51d0f00du;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

static void make_tex(void) {
	int t, p, i;
	for (t = 0; t < NT; t++)
		for (p = 0; p < NP; p++) {
			/* glaze: the upper side of the tube (t round the tube, 0 =
			 * outside, NT/4 = top), with a wavy drip edge */
			int a = t * 4096 / NT;
			int up = pd_sin(a);                         /* the tube normal's height */
			int edge = -2400 + (pd_sin(p * 4096 * 7 / NP) >> 3) + (pd_sin(p * 4096 * 3 / NP + 900) >> 3);
			tex[t][p] = up > edge ? 1 : 0;
		}
	/* sprinkles: short dashes on the glaze, five colours */
	for (i = 0; i < 260; i++) {
		int t0 = (int)(rnd() % NT), p0 = (int)(rnd() % NP), c = 2 + (int)(rnd() % 5), k;
		int dt = (int)(rnd() % 3) - 1, dp = (rnd() & 1) ? 1 : -1;
		if (tex[t0][p0] != 1) continue;
		for (k = 0; k < 3; k++) {
			int tt = (t0 + dt * k + NT) % NT, pp = (p0 + dp * k + NP) % NP;
			if (tex[tt][pp] >= 1) tex[tt][pp] = (uint8_t)c;
		}
	}
}

static void make_bg(void) {
	pd_chunky_t c;
	int x, y;
	if (!pd_chunky_new(&c, PD_W, PD_H)) return;
	for (y = 0; y < PD_H; y++)
		for (x = 0; x < PD_W; x++) {
			int dx = x - 160, dy = y - 120;
			int d = (int)pd_isqrt((uint32_t)(dx * dx + dy * dy));
			/* a glow behind the donut, three tones, dithered */
			int v = (3 * 16 - d * 16 * 3 / 170 + pd_dither(x, y)) >> 4;
			c.px[y * PD_W + x] = (uint8_t)(v < 0 ? 0 : v > 2 ? 2 : v);
			if ((rnd() % 420) == 0) c.px[y * PD_W + x] = 3;   /* stars */
		}
	pd_planar_from(&bg, &c, 0, 2, -1);
	pd_chunky_free(&c);
}

/* The frame's turn as a matrix, by its columns: where the unit
 * vectors along x, y and z go (Q14). About the donut's own axis (y)
 * by C, so the sprinkles go round once a loop; tilted toward us about
 * x by A, so the glaze shows, wobbling; rocked about z by B. */
static int32_t mx[3][3];        /* mx[column][row] */

static void turn(int32_t *v, int sA, int cA, int sB, int cB, int sC, int cC) {
	int32_t x = v[0], y = v[1], z = v[2], t1;
	t1 = (x * cC + z * sC) >> 14; z = (-x * sC + z * cC) >> 14; x = t1;
	t1 = (y * cA - z * sA) >> 14; z = (y * sA + z * cA) >> 14; y = t1;
	t1 = (x * cB - y * sB) >> 14; y = (x * sB + y * cB) >> 14; x = t1;
	v[0] = x; v[1] = y; v[2] = z;
}

/* Per frame: the round of the ring, turned. A point of the surface is
 * then cx0 * ring[p] + cy0 * mx[1], and its normal ct * ring[p] +
 * st * mx[1]: six multiplies each, whatever the turn. */
static int32_t ring[NP][3];
static int ring_f = -1;

static void make_ring(int f) {
	int ph = f * 4096 / NF;
	int C = ph;
	int A = -640 + (pd_sin(ph) * 190 >> 14);
	int B = pd_sin(ph * 2 + 700) * 260 >> 14;
	int sA = pd_sin(A), cA = pd_cos(A), sB = pd_sin(B), cB = pd_cos(B);
	int sC = pd_sin(C), cC = pd_cos(C);
	int i, p;
	for (i = 0; i < 3; i++) {
		mx[i][0] = i == 0 ? 16384 : 0; mx[i][1] = i == 1 ? 16384 : 0; mx[i][2] = i == 2 ? 16384 : 0;
		turn(mx[i], sA, cA, sB, cB, sC, cC);
	}
	for (p = 0; p < NP; p++) {
		int a = p * 4096 / NP;
		ring[p][0] = pd_cos(a); ring[p][1] = 0; ring[p][2] = pd_sin(a);
		turn(ring[p], sA, cA, sB, cB, sC, cC);
	}
	ring_f = f;
}

/* One slice of frame f: tube steps t0 .. t0+SLICE. Multiplies and
 * shifts, one divide a point (the perspective); no 64-bit arithmetic,
 * which costs three times as much on the board. */
static void render_slice(int f, int t0) {
	int t, p;
	if (ring_f != f) make_ring(f);
	for (t = t0; t < t0 + SLICE && t < NT; t++) {
		int th = t * 4096 / NT;
		int32_t st = pd_sin(th), ct = pd_cos(th);
		/* the tube's circle, radius 0.45, at distance 1 (Q14) */
		int32_t cx0 = 16384 + (ct * 7373 >> 14), cy0 = st * 7373 >> 14;
		/* the parts that are the same all the way round */
		int32_t yx = cy0 * mx[1][0] >> 14, yy = cy0 * mx[1][1] >> 14, yz = cy0 * mx[1][2] >> 14;
		int32_t nyx = st * mx[1][0] >> 14, nyy = st * mx[1][1] >> 14, nyz = st * mx[1][2] >> 14;
		const uint8_t *tx = tex[t];
		for (p = 0; p < NP; p++) {
			const int32_t *r = ring[p];
			int32_t x, y, z, nx, ny, nz, rz;
			int sx, sy, idx, lum, m, k;
			/* facing away: never seen (and never let through a gap
			 * between the samples in front) */
			nz = (ct * r[2] >> 14) + nyz;
			if (nz > 2000) continue;
			x = (cx0 * r[0] >> 14) + yx;
			y = (cx0 * r[1] >> 14) + yy;
			z = (cx0 * r[2] >> 14) + yz;
			/* the camera: 4 units away, a little perspective: 230/zz
			 * as a 16.16 reciprocal, one divide */
			rz = (230 << 16) / (z + 4 * 16384);
			sx = DW / 2 + (int)((x * rz) >> 16);
			sy = DW / 2 - (int)((y * rz) >> 16);
			if ((unsigned)sx >= DW - 1 || (unsigned)sy >= DW - 1) continue;
			nx = (ct * r[0] >> 14) + nyx;
			ny = (ct * r[1] >> 14) + nyy;
			{
				/* depth: nearer is bigger z-buffer value */
				uint16_t dz = (uint16_t)(65535 - ((z + 4 * 16384) >> 3));
				/* light from the top left, toward the viewer; the
				 * scales below are n/14800 etc. as multiply-shifts */
				lum = (int)((-nx * 7000 + ny * 9000 - nz * 9500) >> 14);
				m = tx[p];
				if (m == 0) {
					int l = 4 + ((lum < 0 ? 0 : lum) * 354 >> 18);
					idx = 1 + (l > 23 ? 23 : l);
				} else if (m == 1) {
					/* glaze: shinier, a highlight where it faces the light */
					int l = 3 + ((lum < 0 ? 0 : lum) * 319 >> 18);
					if (lum > 12500) l += (lum - 12500) * 874 >> 18;
					idx = 25 + (l > 23 ? 23 : l);
				} else {
					idx = 49 + (m - 2) * 3 + (lum > 9000 ? 2 : lum > 2000 ? 1 : 0);
				}
				/* a 2x2 splat: no holes between the samples */
				for (k = 0; k < 4; k++) {
					int o = (sy + (k >> 1)) * DW + sx + (k & 1);
					if (dz > zbuf[o]) { zbuf[o] = dz; buf.px[o] = (uint8_t)idx; }
				}
			}
		}
	}
}

static bool stop;               /* the scene is over: what is not made, never will be */

static void done(void) {
	pd_chunky_free(&buf);
	free(zbuf);
	zbuf = NULL;
}

bool pd_donut_job(void) {
	if (stop) { if (zbuf) done(); return true; }
	if (f_at < 0) {
		if (!pd_chunky_new(&buf, DW, DW)) return true;
		zbuf = (uint16_t *)calloc(DW * DW, sizeof(uint16_t));
		if (!zbuf) return true;
		make_tex();
		make_bg();
		f_at = 0; t_at = 0;
		memset(buf.px, 0, DW * DW);
		return false;
	}
	if (f_at >= NF) return true;
	render_slice(f_at, t_at);
	t_at += SLICE;
	if (t_at < NT) return false;
	/* close the pinholes the sampling leaves: a pixel nothing hit,
	 * between two that something did, takes its neighbour */
	{
		int x, y;
		for (y = 1; y < DW - 1; y++)
			for (x = 1; x < DW - 1; x++) {
				uint8_t *q = &buf.px[y * DW + x];
				if (*q) continue;
				if (q[-1] && q[1]) *q = q[-1];
				else if (q[-DW] && q[DW]) *q = q[-DW];
			}
	}
	pd_planar_from(&frames[f_at], &buf, 0, 6, -1);
	memset(buf.px, 0, DW * DW);
	memset(zbuf, 0, DW * DW * sizeof(uint16_t));
	f_at++;
	t_at = 0;
	if (f_at < NF) return false;
	done();
	return true;
}

/* -- the palette -- */

static void palette(int fade_white, int flare, int dark) {
	static const uint16_t spr[5][3] = {
		{ 0x612, 0xc35, 0xf8a }, { 0x116, 0x35c, 0x8af }, { 0x541, 0xca3, 0xfe8 },
		{ 0x151, 0x3b4, 0x9f9 }, { 0x555, 0xccc, 0xfff },
	};
	static const uint16_t bgc[4] = { 0x102, 0x214, 0x326, 0xffe };
	int i;
	for (i = 0; i < 256; i++) {
		int d = i & 63, b = i >> 6;
		uint16_t c;
		if (pd_planes < 6 && d) d += 2;         /* the middle of the run of 4 it stands for */
		if (d == 0) c = bgc[b];
		else if (d < 25) c = pd_mix(0x310, 0xfb6, (d - 1) * 256 / 23);       /* dough */
		else if (d < 49) {
			int k = d - 25;
			c = k < 18 ? pd_mix(0x402, 0xf6b, k * 256 / 17) : pd_mix(0xf6b, 0xfff, (k - 17) * 256 / 6);
		} else {
			int s = (d - 49) / 3, l = (d - 49) % 3;
			c = s < 5 ? spr[s][l] : 0xfff;
		}
		c = pd_mix(c, 0xfff, flare);
		c = pd_mix(c, 0xfff, fade_white);
		pd_pal(i, dark ? pd_mix(c, 0x000, dark) : c);
	}
}

/* The frames made so far. On a board where the storm left too little
 * time for all 64, the donut turns through the ones there are while
 * the rest are made (the scene itself costs next to nothing) -- it
 * does not wait for them. */
static int ready(void) { return f_at < 0 ? 0 : f_at; }

static void begin(void) {
	/* at least the sky and one frame */
	while (ready() < 1 && !pd_donut_job()) { }
	/* fewer planes: no sky behind it (6); with 4, the donut in steps of 4 */
	pd_keep(0x3f, 0x3c);
	palette(256, 0, 0);
	if (bg.pl[0]) pd_put(&bg, 6, 0, 0, 0, 0, PD_W, PD_H);
	pd_planes_fill(0, 6, 0, 0, PD_W, PD_H, 0);
}

static void frame(uint32_t t, uint32_t lt) {
	/* a donut frame every two of ours: a turn in two seconds */
	int n = ready(), f = n ? (int)(lt / 2) % n : 0;
	int white = lt < 24 ? (int)(256 - lt * 256 / 24) : 0;
	int flare = pdm_level(PD_CH_SNARE) / 5;
	/* the last beat: to black; the tunnel comes out of the dark */
	uint32_t len = pd_frame_of_bar(16) - pd_frame_of_bar(12);
	int dark = lt + 28 > len ? (int)((lt + 28 - len) * 256 / 28) : 0;
	if (lt + 1 >= len) stop = true;
	if (n && frames[f].pl[0]) pd_put(&frames[f], 0, 0, 0, (PD_W - DW) / 2, (PD_H - DW) / 2 - 4, DW, DW);
	palette(white, flare, dark);
	(void)t;
}

const pd_scene_t pd_scene_donut = { "the donut", 12, begin, frame };

void pd_donut_store(pd_store_t *st) {
	int k;
	if (st->writing) while (!pd_donut_job()) { }
	for (k = 0; k < NF; k++) pd_store_planar(st, &frames[k]);
	pd_store_planar(st, &bg);
	if (!st->writing && !st->bad) f_at = NF;
}
