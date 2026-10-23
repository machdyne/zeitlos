/*
 * planedemo -- bars 21-24: vectors. Out of the tunnel into black
 * space: a wireframe Z, the machine's initial, turns slowly while a
 * dodecahedron orbits it; stars stream past; copper bars sweep behind.
 * In the last bar everything speeds up, toward the light.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Lines are the line rasterizer's (ar_plane_line), into the planes of
 * their colour. Planes 2-4: the two solids (7 colours, by depth).
 * Planes 0-1: the stars (3 brightnesses, streaks). Planes 5-7: three
 * copper bars, each a 7-step gradient blitted from one row of bitmaps.
 * The palette puts the solids in front, then the stars, then the bars.
 */

#include <stdlib.h>
#include <string.h>

#include "pd.h"
#include "pd_gfx.h"
#include "pd_scenes.h"

/* -- the copper bar: 14 rows, values 1..7..1, three planes -- */

#define BAR_H 14
static uint32_t barbm[3][BAR_H * (PD_W / 32) + 1];

static void make_bar(void) {
	int y, k;
	static const int v[BAR_H] = { 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1 };
	memset(barbm, 0, sizeof(barbm));
	for (y = 0; y < BAR_H; y++)
		for (k = 0; k < 3; k++)
			if ((v[y] >> k) & 1) memset(&barbm[k][y * (PD_W / 32)], 0xff, PD_W / 8);
}

/* -- the solids -- */

typedef struct { int x, y, z; } v3;

/* the Z: an outline of ten points, extruded front (z+) and back (z-) */
static const int zpt[10][2] = {
	{ -60, -50 }, { 60, -50 }, { 60, -32 }, { -26, 32 }, { 60, 32 },
	{ 60, 50 }, { -60, 50 }, { -60, 32 }, { 26, -32 }, { -60, -32 },
};
static v3 zv[20];
static int ze[30][2];

/* the dodecahedron: 20 points, 30 edges, found by distance */
static v3 dv[20];
static int de[30][2], nde;

static void make_solids(void) {
	int i, j, n = 0;
	for (i = 0; i < 10; i++) {
		zv[i].x = zpt[i][0] * 256; zv[i].y = zpt[i][1] * 256; zv[i].z = 14 * 256;
		zv[10 + i] = zv[i]; zv[10 + i].z = -14 * 256;
	}
	for (i = 0; i < 10; i++) {
		ze[n][0] = i; ze[n][1] = (i + 1) % 10; n++;
		ze[n][0] = 10 + i; ze[n][1] = 10 + (i + 1) % 10; n++;
		ze[n][0] = i; ze[n][1] = 10 + i; n++;
	}
	/* (+-1, +-1, +-1), (0, +-1/p, +-p), (+-1/p, +-p, 0), (+-p, 0, +-1/p):
	 * p = 1.618, in 8.8 */
	{
		static const int a = 256, p = 414, q = 158;   /* 1, phi, 1/phi */
		int s;
		n = 0;
		for (s = 0; s < 8; s++) {
			dv[n].x = (s & 1) ? a : -a; dv[n].y = (s & 2) ? a : -a; dv[n].z = (s & 4) ? a : -a; n++;
		}
		for (s = 0; s < 4; s++) {
			int b1 = (s & 1) ? 1 : -1, b2 = (s & 2) ? 1 : -1;
			dv[n].x = 0; dv[n].y = b1 * q; dv[n].z = b2 * p; n++;
			dv[n].x = b1 * q; dv[n].y = b2 * p; dv[n].z = 0; n++;
			dv[n].x = b1 * p; dv[n].y = 0; dv[n].z = b2 * q; n++;
		}
	}
	/* an edge joins two points 2/phi apart (about 316 in 8.8) */
	nde = 0;
	for (i = 0; i < 20; i++)
		for (j = i + 1; j < 20; j++) {
			int dx = dv[i].x - dv[j].x, dy = dv[i].y - dv[j].y, dz = dv[i].z - dv[j].z;
			int d2 = dx * dx + dy * dy + dz * dz;
			if (d2 > 300 * 300 && d2 < 335 * 335 && nde < 30) { de[nde][0] = i; de[nde][1] = j; nde++; }
		}
}

/* rotate by angles about y then x, scale, place, project */
static void project(const v3 *in, int n, int ay, int ax, int scale, int ox, int oy, int oz,
	int *sx, int *sy, int *sz) {
	int i, sY = pd_sin(ay), cY = pd_cos(ay), sX = pd_sin(ax), cX = pd_cos(ax);
	for (i = 0; i < n; i++) {
		int x = in[i].x * scale >> 8, y = in[i].y * scale >> 8, z = in[i].z * scale >> 8, t;
		t = (x * cY + z * sY) >> 14; z = (-x * sY + z * cY) >> 14; x = t;
		t = (y * cX - z * sX) >> 14; z = (y * sX + z * cX) >> 14; y = t;
		x += ox; y += oy; z += oz;
		/* x, y, z in 8.8 pixels; the eye 300 pixels back */
		{
			int zz = (z >> 8) + 300;
			if (zz < 40) zz = 40;
			sx[i] = 160 + (x >> 8) * 300 / zz;
			sy[i] = 112 + (y >> 8) * 300 / zz;
			sz[i] = z >> 8;
		}
	}
}

static void lines(const int (*e)[2], int ne, const int *sx, const int *sy, const int *sz, int near_v, int far_v) {
	int i, k;
	for (i = 0; i < ne; i++) {
		int a = e[i][0], b = e[i][1];
		int v = (sz[a] + sz[b]) / 2 < 0 ? near_v : far_v;
		for (k = 0; k < 3; k++)
			if ((v >> k) & 1) pdp_line(2 + k, sx[a], sy[a], sx[b], sy[b], 1);
	}
}

/* -- stars -- */

#define NS 150
static int st_x[NS], st_y[NS], st_z[NS];

static void star_reset(int i, bool far_) {
	st_x[i] = (int)(pd_rand() % 640) - 320;
	st_y[i] = (int)(pd_rand() % 480) - 240;
	st_z[i] = far_ ? 900 : 100 + (int)(pd_rand() % 800);
}

static bool made;

static void begin(void) {
	int i;
	if (!made) { make_bar(); make_solids(); made = true; }
	/* fewer planes: stars in one brightness, bars in fewer steps (6);
	 * with 4, the solids in two planes (values 1 and 4: every edge
	 * still shows) and flat bars */
	pd_keep(0xde, 0x96);
	for (i = 0; i < NS; i++) star_reset(i, false);
	pd_planes_fill(0, 8, 0, 0, PD_W, PD_H, 0);
}

static void palette(int hue, int white) {
	int i;
	for (i = 0; i < 256; i++) {
		int st = i & 3, ln = (i >> 2) & 7, br = i >> 5;
		uint16_t c;
		if (ln) {
			static const uint16_t lc[8] = { 0, 0x0aa, 0x4f8, 0x8ff, 0xf6c, 0xfa4, 0x48f, 0xfff };
			c = lc[ln];
		} else if (st) {
			static const uint16_t sc[4] = { 0, 0x445, 0x99a, 0xeef };
			c = sc[st];
		} else if (br) {
			/* the copper: a hue that drifts, dark to bright to dark */
			static const uint16_t hues[6] = { 0xf20, 0xfa0, 0x2c3, 0x0bf, 0x52f, 0xf2a };
			int h = (hue >> 8) % 6, f = hue & 255;
			uint16_t base = pd_mix(hues[h], hues[(h + 1) % 6], f);
			c = pd_mix(0x000, base, br * 256 / 7);
			if (br == 7) c = pd_mix(c, 0xfff, 96);
		} else c = 0x000;
		pd_pal(i, pd_mix(c, 0xfff, white));
	}
}

static void frame(uint32_t t, uint32_t lt) {
	int i, k;
	uint32_t len = pd_frame_of_bar(24) - pd_frame_of_bar(20), build = pd_frame_of_bar(23) - pd_frame_of_bar(20);
	int speed = lt < build ? 4 : 4 + (int)((lt - build) * 40 / (len - build));   /* stars: faster in the build */
	int sx[20], sy[20], sz[20];
	int white = lt + 16 > len ? (int)((lt + 16 - len) * 256 / 16) : 0;
	int spin = lt < build ? (int)lt * 9 : (int)build * 9 + (int)((lt - build) * (lt - build)) / 3;
	(void)t;

	/* behind: the bars */
	pd_planes_fill(5, 3, 0, 0, PD_W, PD_H, 0);
	for (k = 0; k < 3; k++) {
		int y = 113 + (pd_sin((int)lt * 22 + k * 1100) * 96 >> 14) - BAR_H / 2;
		for (i = 0; i < 3; i++) pdp_or(5 + i, barbm[i], PD_W / 8, 0, 0, 0, y, PD_W, BAR_H);
	}
	/* the stars, streaking toward us */
	pd_planes_fill(0, 2, 0, 0, PD_W, PD_H, 0);
	for (i = 0; i < (pd_late ? NS / 2 : NS); i++) {
		int z0 = st_z[i], z1 = st_z[i] - speed, x0, y0, x1, y1, v;
		if (z1 < 20) { star_reset(i, true); continue; }
		x0 = 160 + st_x[i] * 120 / z0; y0 = 112 + st_y[i] * 120 / z0;
		x1 = 160 + st_x[i] * 120 / z1; y1 = 112 + st_y[i] * 120 / z1;
		st_z[i] = z1;
		if (x1 < 0 || x1 >= PD_W || y1 < 0 || y1 >= PD_H) { star_reset(i, true); continue; }
		v = z1 < 250 ? 3 : z1 < 550 ? 2 : 1;
		for (k = 0; k < 2; k++) if ((v >> k) & 1) pdp_line(k, x0, y0, x1, y1, 1);
	}
	/* in front: the Z and the dodecahedron round it */
	pd_planes_fill(2, 3, 0, 0, PD_W, PD_H, 0);
	project(zv, 20, spin * 2, 300 + (pd_sin(spin) >> 6), 330, 0, 0, 0, sx, sy, sz);
	lines(ze, 30, sx, sy, sz, 7, 1);
	{
		int orbit = (int)lt * 20;
		project(dv, 20, spin * 3, spin * 2, 34 * 256,
			pd_cos(orbit) * 110 >> 6, pd_sin(orbit * 2) * 20 >> 6, pd_sin(orbit) * 110 >> 6, sx, sy, sz);
		/* 34 px a unit: a radius of about 59 px */
		lines(de, nde, sx, sy, sz, 4, 6);
	}
	palette((int)lt * 6, white);
}

const pd_scene_t pd_scene_vectors = { "vectors", 20, begin, frame };
