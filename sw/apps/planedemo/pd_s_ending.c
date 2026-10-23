/*
 * planedemo -- bars 29-32: the end. The name again, in gold, with a
 * glint running over it; on top of it a donut, and the mouse, who runs
 * in and eats it, four bites on four beats. The day goes; the mouse
 * sleeps; on the last chord the words come up -- "a timeless
 * computer" -- and the picture goes to black.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Planes 0-3: drawn once. The letters' faces are in 8 diagonal stripes
 * (1-8) so a glint can run along them in the palette; 9 the letters'
 * depth, 10 their outline, 11 the words under them (put in on the
 * last chord), 12-15 the sky in
 * four dithered bands. Plane 4: stars, which the palette shows as the
 * sky darkens. Planes 5-7: the mouse, the donut and the sleeper's z's,
 * redrawn each frame in their strip.
 */

#include <stdlib.h>
#include <string.h>

#include "pd.h"
#include "pd_gfx.h"
#include "pd_scenes.h"
#include "../mausrun/sprites.h"

#define LOGO_S 5                /* the letters' scale: 40 px */
#define LOGO_Y 78
#define SUB_Y 140
#define MW 36                   /* the mouse's box */
#define DWD 24                  /* the donut's */
#define DHT 16
#define STRIP_Y (LOGO_Y - 44)   /* where planes 5-7 are drawn, and */
#define STRIP_H 48              /* how far down (past the feet) */
#define DONUT_X 172

static const char logo[] = "ZEITLOS";
static const char sub[] = "timeless computing";

static pd_planar_t pic, mouse[3], donut[5], zee, words_p;
static pd_planar_t strip;       /* planes 5-7 of the strip, put together each frame */
static bool words_in;
static int step;

/* Fewer planes (pd_gfx.h): the letters in 3 planes (6) -- 4 stripes,
 * depth, outline, the words; the sky one colour, no stars -- or 2 (4):
 * face, depth, outline; and the mouse in 2: body, outline, the donut.
 * Values are made for the board (enc) and read back for the palette
 * (dec) as the eight-plane ones they stand for. */
static int enc_logo(int v) {
	if (pd_planes >= 8) return v;
	if (pd_planes >= 6) return v >= 12 ? 0 : v >= 9 ? v - 4 : v ? (v + 1) / 2 : 0;
	return v >= 12 ? 0 : v == 9 ? 2 : v == 10 ? 3 : v ? 1 : 0;
}
static int dec_logo(int v) {
	if (pd_planes >= 8) return v;
	if (pd_planes >= 6) return v == 0 ? 13 : v <= 4 ? 2 * v - 1 : v + 4;
	return v == 0 ? 13 : v == 1 ? 5 : v == 2 ? 9 : 10;
}
static int enc_mouse(int v) {
	if (pd_planes >= 6 || !v) return v;
	return v == 3 ? 1 : v == 4 ? 2 : 3;
}
static int dec_mouse(int v) {
	if (pd_planes >= 6 || !v) return v;
	return v == 1 ? 3 : v == 2 ? 4 : 2;
}
static void enc_px(pd_chunky_t *c, int (*enc)(int)) {
	int i;
	if (pd_planes >= 8) return;
	for (i = 0; i < c->w * c->h; i++) c->px[i] = (uint8_t)(enc(c->px[i] & 15) | (c->px[i] & 16));
}

static uint32_t seed = 0xe0d5a1e5u;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

static void make_pic(void) {
	pd_chunky_t c, t;
	int x, y, w = pdc_text_w(logo, LOGO_S), lx = (PD_W - w) / 2, k;
	if (!pd_chunky_new(&c, PD_W, PD_H)) return;
	if (!pd_chunky_new(&t, PD_W, PD_H)) { pd_chunky_free(&c); return; }
	/* the sky, four bands, and stars over it */
	for (y = 0; y < PD_H; y++)
		for (x = 0; x < PD_W; x++) {
			int v = (y * 3 * 16 / PD_H + pd_dither(x, y)) / 16;
			c.px[y * PD_W + x] = (uint8_t)(12 + (v > 3 ? 3 : v));
			if ((rnd() % 160) == 0) c.px[y * PD_W + x] |= 16;
		}
	/* the letters, once in a scratch picture, then their depth (a few
	 * pixels down and right), outline and faces into the sky */
	memset(t.px, 0, (size_t)PD_W * PD_H);
	pdc_text(&t, lx, LOGO_Y, logo, 1, LOGO_S);
	for (k = 4; k >= 1; k--)
		for (y = 0; y < PD_H - k; y++)
			for (x = 0; x < PD_W - k; x++)
				if (t.px[y * PD_W + x]) c.px[(y + k) * PD_W + x + k] = 9;
	for (y = 1; y < PD_H - 1; y++)
		for (x = 1; x < PD_W - 1; x++)
			if (!t.px[y * PD_W + x] && (t.px[y * PD_W + x - 1] || t.px[y * PD_W + x + 1] ||
			    t.px[(y - 1) * PD_W + x] || t.px[(y + 1) * PD_W + x]))
				c.px[y * PD_W + x] = 10;
	for (y = 0; y < PD_H; y++)
		for (x = 0; x < PD_W; x++)
			if (t.px[y * PD_W + x]) c.px[y * PD_W + x] = (uint8_t)(1 + ((x + y) / 7) % 8);
	/* the words under them: put in when they come up (an index of
	 * their own hidden in the sky would show a little, the sky being
	 * dithered bands) */
	{
		pd_chunky_t w8;
		if (pd_chunky_new(&w8, pdc_text_w(sub, 1), 8)) {
			memset(w8.px, 0, (size_t)w8.w * 8);
			pdc_text(&w8, 0, 0, sub, 1, 1);
			pd_planar_from(&words_p, &w8, 0, 1, -1);
			pd_chunky_free(&w8);
		}
	}
	enc_px(&c, enc_logo);
	pd_planar_from(&pic, &c, 0, 5, -1);
	pd_chunky_free(&t);
	pd_chunky_free(&c);
}

/* The mouse at twice its size: body 3, outline and eye 4. */
static void make_mouse(int f) {
	static const uint16_t *const art[3] = { mr_msk_mouse_run1, mr_msk_mouse_run2, mr_msk_mouse_stand };
	pd_chunky_t c;
	int x, y, i;
	if (!pd_chunky_new(&c, MW, MW)) return;
	memset(c.px, 0, MW * MW);
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
			if ((art[f][y] >> x) & 1) pdc_fill(&c, 2 + x * 2, 2 + y * 2, 2, 2, 3);
	for (y = 0; y < MW; y++)
		for (x = 0; x < MW; x++)
			if (!c.px[y * MW + x] && (pdc_get(&c, x - 1, y) == 3 || pdc_get(&c, x + 1, y) == 3 ||
			    pdc_get(&c, x, y - 1) == 3 || pdc_get(&c, x, y + 1) == 3))
				c.px[y * MW + x] = 4;
	for (i = MW - 1; i > 0; i--) {
		int found = 0;
		for (y = 6; y < 18; y++) if (pdc_get(&c, i, y) == 3) { found = y; break; }
		if (found) { pdc_fill(&c, i - 5, found + 2, 2, 2, 4); break; }
	}
	enc_px(&c, enc_mouse);
	pd_planar_from(&mouse[f], &c, 0, 3, 0);
	pd_chunky_free(&c);
}

/* The donut, after b bites from its left: dough 1, glaze 2, sprinkles
 * 5-7, outline 4; at 4 bites, crumbs. */
static void make_donut(int b) {
	pd_chunky_t c;
	int x, y, i;
	if (!pd_chunky_new(&c, DWD, DHT)) return;
	memset(c.px, 0, DWD * DHT);
	for (y = 0; y < DHT; y++)
		for (x = 0; x < DWD; x++) {
			/* a ring seen from a little above: ellipses, in 8ths */
			int dx = x * 8 - (DWD * 4 - 4), dy = (y * 8 - (DHT * 4 - 4)) * 5 / 3;
			int r2 = dx * dx + dy * dy;
			if (r2 > 84 * 84 || r2 < 26 * 26) continue;
			c.px[y * DWD + x] = (uint8_t)(dy < 18 ? 2 : 1);
		}
	seed = 0x0d0e5u;
	for (i = 0; i < 14; i++) {
		x = 2 + (int)(rnd() % (DWD - 4)); y = 1 + (int)(rnd() % 8);
		if (c.px[y * DWD + x] == 2) c.px[y * DWD + x] = (uint8_t)(5 + rnd() % 3);
	}
	/* the bites: round, from the left, a little deeper each */
	for (i = 0; i < b && i < 4; i++)
		for (y = 0; y < DHT; y++)
			for (x = 0; x < DWD; x++) {
				int bx = i * 6, by = 3 + (i & 1) * 8;
				int dx = x - bx, dy = y - by;
				if (dx * dx + dy * dy < 46 || x < i * 6 - 2) c.px[y * DWD + x] = 0;
			}
	if (b >= 4) {
		memset(c.px, 0, DWD * DHT);
		for (i = 0; i < 5; i++) pdc_set(&c, 6 + i * 3, DHT - 1 - (i & 1), 1);
	}
	/* an outline, so it reads on the gold */
	for (y = 0; y < DHT; y++)
		for (x = 0; x < DWD; x++)
			if (!c.px[y * DWD + x] && b < 4) {
				int n = pdc_get(&c, x - 1, y) | pdc_get(&c, x + 1, y) | pdc_get(&c, x, y - 1) | pdc_get(&c, x, y + 1);
				if (n && n != 4) c.px[y * DWD + x] = 4;
			}
	enc_px(&c, enc_mouse);
	pd_planar_from(&donut[b], &c, 0, 3, 0);
	pd_chunky_free(&c);
}

static void make_zee(void) {
	pd_chunky_t c;
	if (!pd_chunky_new(&c, 8, 8)) return;
	memset(c.px, 0, 64);
	pdc_text(&c, 0, 0, "z", 1, 1);
	pd_planar_from(&zee, &c, 0, 1, -1);
	pd_chunky_free(&c);
}

bool pd_ending_job(void) {
	int s = step++;
	if (s > 9) return true;
	if (s == 0) { make_pic(); return false; }
	if (s <= 3) { make_mouse(s - 1); return false; }
	if (s <= 8) { make_donut(s - 4); return false; }
	make_zee();
	return true;
}

/* -- the palette -- */

static void palette(int glint, int dusk, int words, int white, int black, int lit) {
	static const uint16_t top[4] = { 0x7bf, 0xa8c, 0x013, 0x013 };
	static const uint16_t bot[4] = { 0xcef, 0xfb8, 0x125, 0x024 };
	static const uint16_t mc[8] = { 0, 0xc84, 0xf8c, 0xccd, 0x112, 0xf43, 0x4af, 0xfe4 };
	int i, a = dusk >> 8, f = dusk & 255;
	uint16_t t, b;
	if (a > 2) { a = 2; f = 256; }
	t = pd_mix(top[a], top[a + 1], f);
	b = pd_mix(bot[a], bot[a + 1], f);
	for (i = 0; i < 256; i++) {
		int lo = dec_logo(i & 15), st = (i >> 4) & 1, hi = dec_mouse(i >> 5);
		uint16_t c, bg = pd_mix(t, b, lo >= 12 ? (lo - 12) * 85 : SUB_Y * 3 * 85 / PD_H);
		if (hi) c = mc[hi];
		else if (lo >= 1 && lo <= 8) {
			/* gold, and the glint where it is passing */
			int d = ((lo - 1) - glint + 64) % 8;
			int g = d == 0 ? 256 : d == 1 || d == 7 ? 120 : 0;
			c = pd_mix(pd_mix(0xc81, 0xfd5, lo * 20), 0xfff, g * lit / 256);
		} else if (lo == 9) c = 0x620;
		else if (lo == 10) c = 0x200;
		else if (lo == 11) c = pd_mix(bg, 0xfff, words);
		else c = st ? pd_mix(bg, 0xffe, dusk < 256 ? 0 : dusk > 512 ? 256 : dusk - 256) : bg;
		c = pd_mix(c, 0xfff, white);
		pd_pal(i, pd_mix(c, 0x000, black));
	}
}

static void begin(void) {
	pd_job_finish(pd_ending_job);
	if (!strip.pl[0]) pd_canvas_new(&strip, PD_W, STRIP_H, 3);
	pd_keep(0xe7, 0x63);
	pd_put(&pic, 0, 0, 0, 0, 0, PD_W, PD_H);
	pd_planes_fill(5, 3, 0, 0, PD_W, PD_H, 0);
	words_in = false;
}

static int zx[3], zy[3], zt[3];

static void frame(uint32_t t, uint32_t lt) {
	uint32_t bar = pd_frame_of_bar(29) - pd_frame_of_bar(28);
	uint32_t len = PD_END_FRAME - pd_frame_of_bar(28);
	uint32_t end31 = pd_frame_of_bar(31) - pd_frame_of_bar(28);
	int white = lt < 20 ? (int)(256 - lt * 256 / 20) : 0;
	int black = lt + 80 > len ? (int)((lt + 80 - len) * 256 / 70) : 0;
	int words = lt < end31 ? 0 : lt < end31 + 40 ? (int)((lt - end31) * 256 / 40) : 256;
	int dusk = lt < bar * 2 ? 0 : (int)((lt - bar * 2) * 512 / bar);
	int bites, mx, my = LOGO_Y - MW + 2, f, i;
	(void)t;
	if (black > 256) black = 256;
	if (dusk > 768) dusk = 768;
	/* the last chord: a flash */
	if (lt >= end31 && lt < end31 + 16) white = (int)((end31 + 16 - lt) * 160 / 16);

	/* the mouse runs in over the first bar and stops by the donut */
	if (lt < bar - 10) {
		mx = -MW + (int)(lt * (DONUT_X - MW + 2 + MW) / (bar - 10));
		f = (lt / 5) & 1;
		my -= (lt / 5) & 1;
	} else {
		mx = DONUT_X - MW + 2;
		f = 2;
	}
	/* then bites it on the beats of the second */
	bites = lt < bar ? 0 : (int)((lt - bar) * 4 / bar) + 1;
	if (bites > 4) bites = 4;
	if (lt >= bar && lt < bar * 2) {
		uint32_t b = (lt - bar) % (bar / 4);
		if (b < 8) my -= (int)(b < 4 ? b : 8 - b);
	}

	if (lt >= end31 && !words_in && words_p.pl[0]) {
		pd_shape(words_p.pl[0], words_p.stride, 0, 4, enc_logo(11), 0, 0, (PD_W - words_p.w) / 2, SUB_Y, words_p.w, 8);
		words_in = true;
	}
	/* planes 5-7 in the strip: the donut, the mouse and the z's, put
	 * together in RAM and blitted in one pass (pd_canvas_new()) --
	 * cleared on the screen and drawn again, the mouse flashed when the
	 * beam came by in between */
	if (strip.pl[0]) {
		pd_canvas_clear(&strip);
		if (donut[bites].pl[0]) pd_canvas_put(&strip, 0, &donut[bites], true, 0, 0, DONUT_X, LOGO_Y - DHT + 2 - STRIP_Y, DWD, DHT);
		if (mouse[f].pl[0]) pd_canvas_put(&strip, 0, &mouse[f], true, 0, 0, mx, my - STRIP_Y, MW, MW);
		/* asleep in the third: z's drift up */
		if (lt >= bar * 2 + 20 && zee.pl[0]) {
			for (i = 0; i < 3; i++) {
				int age = (int)(lt - bar * 2 - 20) - i * 26;
				if (age < 0) continue;
				age %= 78;
				zx[i] = mx + 24 + age / 4 + (pd_sin(age * 60) * 3 >> 14);
				zy[i] = my + 2 - age / 3;
				zt[i] = age;
				if (zy[i] >= STRIP_Y && lt < len - 80)
					pd_canvas_shape(&strip, zee.pl[0], zee.stride, 0, 3, enc_mouse(3), 0, 0, zx[i], zy[i] - STRIP_Y, 8, 8);
			}
		}
		pd_put(&strip, 5, 0, 0, 0, STRIP_Y, PD_W, STRIP_H);
	}
	(void)zt;
	palette((int)lt / 3, dusk, words, white, black, 256 - dusk / 3);
}

const pd_scene_t pd_scene_ending = { "the end", 28, begin, frame };

/* Made for eight planes (fewer make their own values, enc_logo()). */
void pd_ending_store(pd_store_t *st) {
	int k;
	if (st->writing) while (!pd_ending_job()) { }
	pd_store_planar(st, &pic);
	for (k = 0; k < 3; k++) pd_store_planar(st, &mouse[k]);
	for (k = 0; k < 5; k++) pd_store_planar(st, &donut[k]);
	pd_store_planar(st, &zee);
	pd_store_planar(st, &words_p);
	if (!st->writing && !st->bad) step = 10;
}
