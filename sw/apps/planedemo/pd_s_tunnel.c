/*
 * planedemo -- bars 17-20: into the machine. The donut's hole opens
 * into a tunnel of circuitry, rushing past and twisting with the beat;
 * the mouse, a silhouette against the light at the end, flies away
 * down it.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The oldest trick: every pixel's index says where on the tunnel's
 * wall it is -- one of 15 slices round, one of 8 rings deep -- and is
 * drawn once. The texture is in the palette: each frame the 120
 * entries are rewritten with the texture moved a ring deeper and
 * turned a little, and the whole tunnel moves, with nothing redrawn.
 * Plane 7 is fog, dithered thicker toward the far end: an index with
 * it set is the same colour darker. The mouse is a hole in the tunnel
 * -- index 0, black -- put back where it was each frame.
 */

#include <stdlib.h>
#include <string.h>

#include "pd.h"
#include "pd_gfx.h"
#include "pd_scenes.h"
#include "../mausrun/sprites.h"

#define NA 15                   /* slices round */
#define ND 8                    /* rings deep (the texture repeats) */
#define NSZ 14                  /* sizes of the mouse */

static pd_planar_t pic, fog;
static uint32_t *mbits[NSZ];
static int msz[NSZ], mstride[NSZ];
static int step;

static void make_pic(void) {
	pd_chunky_t c;
	int x, y;
	if (!pd_chunky_new(&c, PD_W, PD_H)) return;
	for (y = 0; y < PD_H; y++)
		for (x = 0; x < PD_W; x++) {
			int dx = x - 160, dy = (y - 120) * 5 / 4;
			int d = (int)pd_isqrt((uint32_t)(dx * dx + dy * dy)), a, u;
			if (d < 4) d = 4;
			a = pd_atan2(dy, dx) * NA / 4096;
			u = (int)(2400 / d);                    /* depth: far is big */
			/* the far end: the light (index 121) */
			c.px[y * PD_W + x] = (uint8_t)(d < 13 ? 121 : 1 + a * ND + (u % ND));
		}
	pd_planar_from(&pic, &c, 0, 7, -1);
	/* the fog: dithered, thicker the further in */
	for (y = 0; y < PD_H; y++)
		for (x = 0; x < PD_W; x++) {
			int dx = x - 160, dy = (y - 120) * 5 / 4;
			int d = (int)pd_isqrt((uint32_t)(dx * dx + dy * dy));
			int k = 20 - d * 20 / 90;                /* solid near the centre, none past 90 */
			c.px[y * PD_W + x] = (uint8_t)(k > pd_dither(x, y) ? 1 : 0);
		}
	pd_planar_from(&fog, &c, 0, 1, -1);
	pd_chunky_free(&c);
}

/* The mouse's jump frame, outlined, at size s (square), as a mask. */
static void make_mouse(int i) {
	pd_chunky_t c;
	pd_planar_t p;
	int s = 12 + i * 8, x, y;
	if (!pd_chunky_new(&c, s, s)) return;
	for (y = 0; y < s; y++)
		for (x = 0; x < s; x++)
			if ((mr_msk_mouse_jump[y * 16 / s] >> (x * 16 / s)) & 1) c.px[y * s + x] = 1;
	pd_planar_from(&p, &c, 0, 1, -1);
	mbits[i] = p.pl[0];
	msz[i] = s;
	mstride[i] = p.stride;
	pd_chunky_free(&c);
}

bool pd_tunnel_job(void) {
	if (step == 0) { make_pic(); step++; return false; }
	if (step <= NSZ) { make_mouse(step - 1); step++; return step > NSZ; }
	return true;
}

/* -- the texture: a chip's insides -- */

static uint16_t tex(int a, int d, int t) {
	/* a ring of light every 8, a dim one between; copper traces in
	 * dashes along some slices; gold pads */
	int ring = d % ND, slice = a % NA;
	uint16_t c = 0x013;
	if (ring == 0) c = 0x2df;
	else if (ring == 4) c = 0x046;
	else if (slice % 5 == 0 && (ring == 2 || ring == 3)) c = 0xa52;
	else if (slice % 5 == 3 && ring == 6) c = 0xfc3;
	else if ((slice + ring) % 7 == 0) c = 0x025;
	/* the drums light the rings */
	if (ring == 0) c = pd_mix(c, 0xfff, t);
	return c;
}

static uint32_t start;
static int last_x = -1, last_y, last_s;

static void begin(void) {
	pd_job_finish(pd_tunnel_job);
	/* fewer planes: no fog, and the slices past the eighth repeat the
	 * first (6); with 4, two slices round */
	pd_keep(0x3f, 0x0f);
	start = pd_frame_of_bar(16);
	pd_put(&pic, 0, 0, 0, 0, 0, PD_W, PD_H);
	pd_put(&fog, 7, 0, 0, 0, 0, PD_W, PD_H);
	last_x = -1;
}

static void frame(uint32_t t, uint32_t lt) {
	int i, a, d;
	uint32_t len = pd_frame_of_bar(20) - start, bar = len / 4;
	/* in the last bar it rushes, faster and faster, and turns */
	int rush = lt + bar > len ? (int)(lt + bar - len) : 0;
	int depth = (int)(lt * 3 / 4) + rush * rush / 150;     /* rings */
	int turn = (int)(pd_sin((int)lt * 24) * 3 >> 14) + (int)(lt / 6) + rush * rush / 400;
	int kick = pdm_level(PD_CH_KICK) / 2;
	/* out of the donut's dark: the light at the end first, then the
	 * walls */
	int dark = lt < 40 ? (int)(256 - lt * 256 / 40) : 0;
	(void)t;

	/* the palette: entry 1 + a*ND + d shows texture (a + turn, d + depth);
	 * with fog (plane 7) the same, darker; 0 is black */
	pd_pal(0, 0x000);
	pd_pal(128, 0x000);
	for (a = 0; a < NA; a++)
		for (d = 0; d < ND; d++) {
			int e = 1 + a * ND + d;
			uint16_t c = tex(a + turn + NA * 8, d + ND * 64 - depth, kick);
			c = pd_mix(c, 0x000, dark);
			pd_pal(e, c);
			pd_pal(128 + e, pd_mix(c, 0x001, 190));
		}
	for (i = 1 + NA * ND; i < 128; i++) { pd_pal(i, 0); pd_pal(128 + i, 0); }
	/* the light at the far end, breathing with the bass */
	pd_pal(121, pd_mix(pd_mix(0x9df, 0xfff, pdm_level(PD_CH_BASS)), 0x000, dark / 3));
	pd_pal(128 + 121, pd_mix(pd_mix(0x9df, 0xfff, pdm_level(PD_CH_BASS)), 0x000, dark / 3));

	/* the mouse: from near the camera, away down the tunnel */
	if (lt > 30 && lt < len - 10) {
		int k = (int)((len - 10 - lt) * (NSZ - 1) / (len - 40));      /* big .. small */
		int s, x, y;
		if (k < 0) k = 0;
		if (k >= NSZ) k = NSZ - 1;
		if (!mbits[k]) return;
		s = msz[k];
		/* it weaves a little as it goes */
		x = 160 - s / 2 + (pd_sin((int)lt * 40) * (k * 4) >> 14);
		y = 120 - s / 2 + (pd_cos((int)lt * 28) * (k * 3) >> 14) + k * 2;
		if (last_x >= 0) {
			pd_put(&pic, 0, last_x, last_y, last_x, last_y, last_s, last_s);
		}
		for (i = 0; i < 7; i++) pdp_andn(i, mbits[k], mstride[k], 0, 0, x, y, s, s);
		last_x = x; last_y = y; last_s = s;
	} else if (last_x >= 0) {
		pd_put(&pic, 0, last_x, last_y, last_x, last_y, last_s, last_s);
		last_x = -1;
	}
}

const pd_scene_t pd_scene_tunnel = { "the tunnel", 16, begin, frame };

void pd_tunnel_store(pd_store_t *st) {
	int i;
	if (st->writing) while (!pd_tunnel_job()) { }
	pd_store_planar(st, &pic);
	pd_store_planar(st, &fog);
	for (i = 0; i < NSZ; i++) {
		uint32_t s = (uint32_t)msz[i], k = (uint32_t)mstride[i], len = k * s + 4;
		void *b = mbits[i];
		pd_store_u32(st, &s);
		pd_store_u32(st, &k);
		pd_store_block(st, &b, &len);
		if (!st->writing) { msz[i] = (int)s; mstride[i] = (int)k; mbits[i] = (uint32_t *)b; }
	}
	if (!st->writing && !st->bad) step = NSZ + 1;
}
