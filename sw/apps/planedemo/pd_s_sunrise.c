/*
 * planedemo -- bars 25-28: sunrise. Out of the light at the end of the
 * vectors, morning over the city from the storm: the sky goes from
 * violet to gold to blue, the sun climbs, a rainbow comes out, the
 * windows go dark one street at a time, and a sine scroller says hello.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Planes 0-4: a field drawn once -- the sky in 10 bands by height,
 * dithered, and a rainbow in 7 arcs, each in 3 pieces by height so it
 * can be mixed over the sky behind it -- whose colours the palette makes.
 * Under the city those planes instead say which of 8 groups a window
 * belongs to, so the windows can go out a group at a time. Plane 5:
 * the sun, a disc in a dithered halo, moved up a little each frame.
 * Plane 6: the city. Plane 7: the windows (with plane 6) and the
 * scroller (without it), a column at a time on a sine.
 */

#include <stdlib.h>
#include <string.h>

#include "pd.h"
#include "pd_gfx.h"
#include "pd_scenes.h"

#define SKY 10                  /* sky bands: field values 1..10 */
#define RB 11                   /* rainbow: 11 + zone*7 + arc, 3 zones */
#define SUN 76                  /* the sun's box */
#define TXT_Y 66                /* the scroller's middle */
#define TXT_A 26                /* and its swing */
/* the scroller's band of rows (a sine can come out a pixel past its
 * swing), put together in RAM each frame */
#define SCR_Y (TXT_Y - TXT_A - 10)
#define SCR_H (2 * TXT_A + 20)
static uint32_t scr[SCR_H * (PD_W / 32)];

static const char text[] =
	"ZEITLOS * TIMELESS COMPUTING * NO FPU WAS HARMED * "
	"GREETINGS TO YOSYS AND NEXTPNR";

static pd_planar_t pic, bowp, sun, strip;
static int step;

static uint32_t seed = 0x5e7a11edu;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

static void make_pic(void) {
	pd_chunky_t c;
	int x, y, i;
	if (!pd_chunky_new(&c, PD_W, PD_H)) return;
	for (y = 0; y < PD_H; y++)
		for (x = 0; x < PD_W; x++) {
			/* the sky: band by height, dithered between bands */
			int v = 1 + ((y * (SKY - 1) * 16 / 200) + pd_dither(x, y)) / 16;
			int dx = x - 110, dy = y - 236;
			int d = (int)pd_isqrt((uint32_t)(dx * dx + dy * dy));
			if (v > SKY) v = SKY;
			/* the rainbow: seven arcs 4 px wide, red outside */
			if (d >= 150 && d < 150 + 7 * 4) v = RB + (v - 1) * 3 / SKY * 7 + 6 - (d - 150) / 4;
			c.px[y * PD_W + x] = (uint8_t)v;
		}
	/* the city: blocks with roofs, a spire or two, lit windows */
	x = 0;
	while (x < PD_W) {
		int w = 14 + (int)(rnd() % 22), h = 28 + (int)(rnd() % 70), top, wx, wy;
		int g = (int)(rnd() % 8);
		if (x + w > PD_W) w = PD_W - x;
		top = PD_H - h;
		for (y = top; y < PD_H; y++)
			for (i = x; i < x + w; i++) c.px[y * PD_W + i] = (uint8_t)(64 | g);
		if ((rnd() % 4) == 0) {                         /* a spire */
			int sx = x + w / 2, sh = 8 + (int)(rnd() % 14);
			for (y = top - sh; y < top; y++) c.px[y * PD_W + sx] = (uint8_t)(64 | g);
			pdc_fill(&c, sx - 1, top - 3, 3, 3, 64 | g);
		}
		/* windows: 2x2 in a grid, some lit, each street a group */
		for (wy = top + 4; wy < PD_H - 4; wy += 6)
			for (wx = x + 3; wx < x + w - 3; wx += 5)
				if ((rnd() % 100) < 45) {
					int k = (g + (int)(rnd() % 3)) & 7;
					pdc_fill(&c, wx, wy, 2, 2, 64 | 128 | k);
				}
		x += w + (int)(rnd() % 3);
	}
	/* the rainbow comes out across the sky later: its pixels (where
	 * the city is not in front) to a picture of their own, and back
	 * to plain sky in this one */
	{
		pd_chunky_t b;
		if (pd_chunky_new(&b, PD_W, PD_H)) {
			for (y = 0; y < PD_H; y++)
				for (x = 0; x < PD_W; x++) {
					uint8_t *q = &c.px[y * PD_W + x];
					b.px[y * PD_W + x] = 0;
					if (*q >= RB && *q < 64) {
						b.px[y * PD_W + x] = *q;
						int v = 1 + ((y * (SKY - 1) * 16 / 200) + pd_dither(x, y)) / 16;
						*q = (uint8_t)(v > SKY ? SKY : v);
					}
				}
			pd_planar_from(&bowp, &b, 0, 5, 0);
			pd_chunky_free(&b);
		}
	}
	pd_planar_from(&pic, &c, 0, 8, -1);
	pd_chunky_free(&c);
}

static void make_sun(void) {
	pd_chunky_t c;
	int x, y;
	if (!pd_chunky_new(&c, SUN, SUN)) return;
	for (y = 0; y < SUN; y++)
		for (x = 0; x < SUN; x++) {
			int dx = x - SUN / 2, dy = y - SUN / 2;
			int d = (int)pd_isqrt((uint32_t)(dx * dx + dy * dy));
			/* solid to 20, then a halo thinning out to 36 */
			int k = d < 20 ? 16 : d < 36 ? (36 - d) * 12 / 16 : 0;
			c.px[y * SUN + x] = (uint8_t)(k > pd_dither(x, y) ? 1 : 0);
		}
	pd_planar_from(&sun, &c, 0, 1, -1);
	pd_chunky_free(&c);
}

static void make_strip(void) {
	pd_chunky_t c;
	int w = pdc_text_w(text, 2);
	if (!pd_chunky_new(&c, w, 16)) return;
	memset(c.px, 0, (size_t)w * 16);
	pdc_text(&c, 0, 0, text, 1, 2);
	pd_planar_from(&strip, &c, 0, 1, -1);
	pd_chunky_free(&c);
}

bool pd_sunrise_job(void) {
	switch (step++) {
	case 0: make_pic(); return false;
	case 1: make_sun(); return false;
	case 2: make_strip(); return true;
	}
	return true;
}

/* -- the palette -- */

/* the sky at height band v (1 top .. SKY horizon), at time of day m
 * (0 dawn .. 256 morning) */
static uint16_t sky(int v, int m) {
	/* top and horizon colours at three times of day */
	static const uint16_t top[3] = { 0x113, 0x237, 0x38d };
	static const uint16_t hor[3] = { 0x635, 0xf94, 0xbdf };
	int a = m < 128 ? 0 : 1, f = (m < 128 ? m : m - 128) * 2;
	uint16_t t = pd_mix(top[a], top[a + 1], f), h = pd_mix(hor[a], hor[a + 1], f);
	return pd_mix(t, h, (v - 1) * 256 / (SKY - 1));
}

static void palette(int m, int bow, int white, int text_hue) {
	static const uint16_t rain[7] = { 0xf33, 0xf93, 0xfe4, 0x4d4, 0x39f, 0x55d, 0xa5e };
	static const uint16_t hues[6] = { 0xfff, 0xff8, 0xfdf, 0xdff, 0xfff, 0xffc };
	int i;
	uint16_t cityc = pd_mix(0x102, 0x235, m);
	for (i = 0; i < 256; i++) {
		int f = i & 31, s = (i >> 5) & 1, cy = (i >> 6) & 1, w = i >> 7;
		uint16_t c;
		if (w && cy) {
			/* window group f&7 goes out at its own time of morning */
			int off = 60 + (f & 7) * 22;
			int lit = m < off ? 256 : m > off + 24 ? 0 : 256 - (m - off) * 256 / 24;
			c = pd_mix(cityc, 0xfd6, lit);
		} else if (w) {
			int h = text_hue >> 8;
			c = pd_mix(hues[h % 6], hues[(h + 1) % 6], text_hue & 255);
		} else if (cy) c = cityc;
		else if (s) c = pd_mix(0xf62, 0xffe, m);
		else if (f >= RB && f < RB + 21) {
			/* the rainbow over the sky behind it: the band in the
			 * middle of the piece's zone */
			static const int mid[3] = { 2, 6, 9 };
			c = pd_mix(sky(mid[(f - RB) / 7], m), rain[(f - RB) % 7], bow);
		} else c = sky(f < 1 ? 1 : f > SKY ? SKY : f, m);
		pd_pal(i, pd_mix(c, 0xfff, white));
	}
}

static int sun_y = -1, bow_x;

static void begin(void) {
	pd_job_finish(pd_sunrise_job);
	/* fewer planes: the sky in fewer bands and a coarser rainbow (6);
	 * with 4, a plain sky, the sun, the city and the words */
	pd_keep(0xfc, 0xf0);
	pd_put(&pic, 0, 0, 0, 0, 0, PD_W, PD_H);
	sun_y = -1;
	bow_x = 0;
}

static void frame(uint32_t t, uint32_t lt) {
	uint32_t len = pd_frame_of_bar(28) - pd_frame_of_bar(24);
	int m = (int)(lt * 256 / len), x, y, cw = pd_late ? 8 : 4;
	int white = lt < 30 ? (int)(256 - lt * 256 / 30) : 0;
	int bow = m < 64 ? 0 : m < 128 ? 60 + (m - 64) * 140 / 64 : 200;
	/* the whole text across, between the flash and the last half beat */
	int scroll = lt < 20 ? -PD_W : (int)(lt - 20) * (strip.w + PD_W) / (int)(len - 50) - PD_W;
	(void)t;

	/* the rainbow, drawn across the sky left to right as it comes out */
	if (m >= 64 && bow_x < PD_W && bowp.pl[0]) {
		int to = m >= 128 ? PD_W : (m - 64) * PD_W / 64;
		to = (to + 7) & ~7;
		if (to > PD_W) to = PD_W;
		if (to > bow_x) {
			pd_put_masked(&bowp, 0, bow_x, 0, bow_x, 0, to - bow_x, PD_H);
			bow_x = to;
		}
	}

	/* the sun: up from behind the city, slowing */
	{
		int k = 256 - m;
		int ny = 108 + k * k * 120 / 65536 - SUN / 2;
		if (ny != sun_y) {
			if (sun_y >= 0) pd_planes_fill(5, 1, 236 - SUN / 2, sun_y, SUN, PD_H - sun_y < SUN ? PD_H - sun_y : SUN, 0);
			pd_put(&sun, 5, 0, 0, 236 - SUN / 2, ny, SUN, PD_H - ny < SUN ? PD_H - ny : SUN);
			sun_y = ny;
		}
	}
	/* the scroller: four-pixel columns of the strip, each on the sine
	 * (eight while frames are being dropped: half the work), put
	 * together here in its band of rows and blitted in one pass -- a
	 * band cleared and then drawn a column at a time can be caught
	 * half done by the beam */
	if (strip.pl[0]) {
		const uint32_t *src = strip.pl[0];
		int sw = strip.stride / 4, r;
		memset(scr, 0, sizeof(scr));
		for (x = 0; x < PD_W; x += cw) {
			int sx = scroll + x, o, dy;
			if (sx < 0 || sx + cw > strip.w) continue;
			y = TXT_Y - 8 + (pd_sin(x * 12 + (int)lt * 40) * TXT_A >> 14);
			dy = y - SCR_Y;
			o = sx & 31;
			for (r = 0; r < 16; r++) {
				const uint32_t *row = src + r * sw + (sx >> 5);
				uint32_t v = row[0] >> o;
				if (dy + r < 0 || dy + r >= SCR_H) continue;
				if (o + cw > 32) v |= row[1] << (32 - o);
				v &= (1u << cw) - 1u;
				scr[(dy + r) * (PD_W / 32) + (x >> 5)] |= v << (x & 31);
			}
		}
		pdp_plane(7, scr, PD_W / 8, 0, 0, 0, SCR_Y, PD_W, SCR_H);
	} else pd_planes_fill(7, 1, 0, SCR_Y, PD_W, SCR_H, 0);
	palette(m, bow, white, (int)lt * 8);
}

const pd_scene_t pd_scene_sunrise = { "sunrise", 24, begin, frame };

void pd_sunrise_store(pd_store_t *st) {
	if (st->writing) while (!pd_sunrise_job()) { }
	pd_store_planar(st, &pic);
	pd_store_planar(st, &bowp);
	pd_store_planar(st, &sun);
	pd_store_planar(st, &strip);
	if (!st->writing && !st->bad) step = 3;
}
