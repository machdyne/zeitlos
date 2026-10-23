/*
 * planedemo -- bars 5-12: the storm, then the mouse.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A night city in the rain. The camera pans along it; lightning
 * strikes twice on the thunder. Then the mouse -- mausrun's own --
 * runs along the rooftops under a leaf, jumping the gaps, while a cat
 * watches from a chimney; at the last roof it leaps, up out of the
 * picture, into the next scene.
 *
 * The planes are layers, each drawn into its own planes only:
 *
 *   plane 0     the sky: storm clouds, two tones dithered, drawn once
 *   planes 1-2  the far skyline (3 colours), a 1024-wide panorama
 *   planes 3-4  the near rooftops (3 colours), 2048 wide, faster
 *   planes 5-7  in front: two depths of rain (1, 2), the mouse
 *               (3 body, 4 dark, 5 leaf, 6 its rib), lightning (7)
 *
 * and the palette shows the frontmost layer that has a pixel: so
 * parallax is a blit per plane at a different offset, the rain falls
 * through everything without touching it, and a lightning flash is a
 * handful of palette writes.
 */

#include <stdlib.h>
#include <string.h>

#include "pd.h"
#include "pd_gfx.h"
#include "pd_scenes.h"
#include "../mausrun/sprites.h"

#define FAR_W   1024
#define NEAR_W  2048
#define RAIN_W  640
#define RAIN_H  480

static pd_planar_t sky, far, near_, rain[2], mouse[3], mouse_m[3];
static pd_planar_t box;                /* the mouse's box in planes 5-7 (mouse_frame()) */
static uint8_t roof[NEAR_W];           /* the near roofs' tops, a column each */
static int step;

/* -- making it, a piece a step (while the clock and ignition play) -- */

static uint32_t seed = 0x2468ace1u;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

/* smooth noise: a hash on a lattice, eased between */
static int lattice(int x, int y) { uint32_t h = (uint32_t)(x * 374761393 + y * 668265263); h = (h ^ (h >> 13)) * 1274126177u; return (int)(h >> 24); }
static int vnoise(int x, int y, int cell) {
	int gx = x / cell, gy = y / cell, fx = (x % cell) * 256 / cell, fy = (y % cell) * 256 / cell;
	int a = lattice(gx, gy), b = lattice(gx + 1, gy), c = lattice(gx, gy + 1), d = lattice(gx + 1, gy + 1);
	int top = a + ((b - a) * fx >> 8), bot = c + ((d - c) * fx >> 8);
	return top + ((bot - top) * fy >> 8);
}

static void make_sky(void) {
	pd_chunky_t c;
	int x, y;
	if (!pd_chunky_new(&c, PD_W, PD_H)) return;
	for (y = 0; y < PD_H; y++)
		for (x = 0; x < PD_W; x++) {
			int v = vnoise(x, y * 2, 64) / 2 + vnoise(x, y * 2, 24) / 4 + vnoise(x, y * 2, 9) / 8;
			/* lighter toward the horizon, then dithered to two tones */
			v = v * 16 / 224 + y * 6 / PD_H - 6;
			c.px[y * PD_W + x] = (uint8_t)(v * 16 + pd_dither(x, y) * 16 > 16 * 8 ? 1 : 0);
		}
	pd_planar_from(&sky, &c, 0, 1, -1);
	pd_chunky_free(&c);
}

/* buildings: (value 1) with windows lit (2, 3); `top` the lowest roof */
static void building(pd_chunky_t *c, int x, int w, int top, int win, int wy) {
	int y, a;
	pdc_fill(c, x, top, w, c->h - top, 1);
	for (y = top + 6; y < c->h - 4; y += wy)
		for (a = x + 3; a + 2 < x + w - 2; a += win)
			if ((rnd() & 3) == 0) pdc_fill(c, a, y, 2, 3, (rnd() & 1) ? 2 : 3);
}

static void make_far(void) {
	pd_chunky_t c;
	int x = 0;
	if (!pd_chunky_new(&c, FAR_W, PD_H)) return;
	while (x < FAR_W) {
		int w = 18 + (int)(rnd() % 40), top = 110 + (int)(rnd() % 70);
		if (x + w > FAR_W) w = FAR_W - x;
		building(&c, x, w, top, 5, 6);
		/* a spire or an aerial now and then */
		if ((rnd() & 7) == 0) pdc_fill(&c, x + w / 2, top - 14, 1, 14, 1);
		x += w + (int)(rnd() % 3);
	}
	pd_planar_from(&far, &c, 0, 2, -1);
	pd_chunky_free(&c);
}

static void make_near(void) {
	pd_chunky_t c;
	int x = 0, i;
	if (!pd_chunky_new(&c, NEAR_W, PD_H)) return;
	memset(roof, 255, sizeof(roof));
	/* the first 300 columns are wide and gapless: the mouse starts there */
	while (x < NEAR_W) {
		int w = 70 + (int)(rnd() % 110), top = 150 + (int)(rnd() % 40);
		int gap = x < 300 ? 0 : 10 + (int)(rnd() % 14);
		if (x + w > NEAR_W) w = NEAR_W - x;
		building(&c, x, w, top, 7, 9);
		/* a light rim along the roof edge */
		pdc_fill(&c, x, top, w, 1, 2);
		for (i = x; i < x + w; i++) roof[i] = (uint8_t)top;
		/* chimneys, a water tank */
		if ((rnd() & 1) && w > 60) {
			int cx = x + 10 + (int)(rnd() % (unsigned)(w - 30));
			pdc_fill(&c, cx, top - 16, 9, 16, 1);
			pdc_fill(&c, cx - 1, top - 17, 11, 2, 2);
		}
		x += w + gap;
	}
	/* the cat: on a chimney a little way in, eyes lit (3) */
	{
		int cx = 640, top = roof[cx], y, a;
		pdc_fill(&c, cx - 4, top - 20, 18, 20, 1);
		pdc_fill(&c, cx - 5, top - 21, 20, 2, 2);
		for (y = 0; y < 16; y++)
			for (a = 0; a < 16; a++)
				if ((mr_msk_cat1[y] >> a) & 1) {
					pdc_set(&c, cx - 2 + 15 - a, top - 38 + y, 1);
				}
		pdc_set(&c, cx - 2 + 15 - 4, top - 38 + 5, 3);
		pdc_set(&c, cx - 2 + 15 - 7, top - 38 + 5, 3);
	}
	pd_planar_from(&near_, &c, 0, 2, -1);
	pd_chunky_free(&c);
}

static void make_rain(int k) {
	pd_chunky_t c;
	int i, n = k ? 260 : 520, len = k ? 14 : 7, x0, y0;
	if (!pd_chunky_new(&c, RAIN_W / 2, RAIN_H / 2)) return;
	for (i = 0; i < n; i++) {
		x0 = (int)(rnd() % (RAIN_W / 2));
		y0 = (int)(rnd() % (RAIN_H / 2));
		/* streaks slanting with the wind; wrapped, so the tile repeats */
		{
			int s;
			for (s = 0; s < len; s++)
				pdc_set(&c, (x0 + s / 3) % (RAIN_W / 2), (y0 + s) % (RAIN_H / 2), 1);
		}
	}
	/* two by two, so a window anywhere in it is one blit */
	{
		pd_chunky_t big;
		int y;
		if (pd_chunky_new(&big, RAIN_W, RAIN_H)) {
			for (y = 0; y < RAIN_H; y++) {
				memcpy(big.px + y * RAIN_W, c.px + (y % (RAIN_H / 2)) * (RAIN_W / 2), RAIN_W / 2);
				memcpy(big.px + y * RAIN_W + RAIN_W / 2, c.px + (y % (RAIN_H / 2)) * (RAIN_W / 2), RAIN_W / 2);
			}
			pd_planar_from(&rain[k], &big, 0, 1, -1);
			pd_chunky_free(&big);
		}
	}
	pd_chunky_free(&c);
}

/* The mouse at twice its size, under a leaf: body 3, outline 4, leaf
 * 5 and its rib 6. Frames: running (two) and jumping. */
static void make_mouse(int f) {
	static const uint16_t *const art[3] = { mr_msk_mouse_run1, mr_msk_mouse_run2, mr_msk_mouse_jump };
	pd_chunky_t c;
	int x, y, i;
	if (!pd_chunky_new(&c, 48, 56)) return;
	memset(c.px, 0, 48 * 56);
	/* the body, doubled, at the bottom */
	for (y = 0; y < 16; y++)
		for (x = 0; x < 16; x++)
			if ((art[f][y] >> x) & 1) pdc_fill(&c, 8 + x * 2, 24 + y * 2, 2, 2, 3);
	/* an outline round it */
	for (y = 0; y < 56; y++)
		for (x = 0; x < 48; x++)
			if (!c.px[y * 48 + x] && (pdc_get(&c, x - 1, y) == 3 || pdc_get(&c, x + 1, y) == 3 ||
			    pdc_get(&c, x, y - 1) == 3 || pdc_get(&c, x, y + 1) == 3))
				c.px[y * 48 + x] = 4;
	/* the eye: near the front of the head */
	for (i = 47; i > 0; i--) {
		int found = 0;
		for (y = 30; y < 40; y++) if (pdc_get(&c, i, y) == 3) { found = y; break; }
		if (found) { pdc_fill(&c, i - 5, found + 2, 2, 2, 4); break; }
	}
	/* the leaf, held up: an ellipse and its rib, a stalk down */
	for (y = 0; y < 18; y++)
		for (x = 0; x < 44; x++) {
			int dx = x - 22, dy = y - 12;
			if (dx * dx * 81 + dy * dy * 484 <= 81 * 484 && dy <= 4) pdc_set(&c, x + 2, y + 2, 5);
		}
	pdc_line(&c, 4, 13, 46, 10, 6);
	pdc_line(&c, 26, 14, 30, 28, 6);
	pd_planar_from(&mouse[f], &c, 0, 3, 0);
	pd_chunky_free(&c);
	(void)mouse_m;
}

bool pd_city_job(void) {
	switch (step++) {
	case 0: make_sky(); return false;
	case 1: make_far(); return false;
	case 2: make_near(); return false;
	case 3: make_rain(0); return false;
	case 4: make_rain(1); return false;
	case 5: make_mouse(0); return false;
	case 6: make_mouse(1); return false;
	case 7: make_mouse(2); return true;
	default: return true;
	}
}

/* -- the palette: the frontmost layer that has a pixel -- */

static uint16_t sky_rgb[2], far_rgb[4], near_rgb[4], top_rgb[8];
static int flash;               /* 0..256: lightning's light */

static int white;               /* everything toward white: the mouse's last leap */

static void palette(void) {
	int i;
	for (i = 0; i < 256; i++) {
		int top = i >> 5, nr = (i >> 3) & 3, fr = (i >> 1) & 3, sk = i & 1;
		uint16_t c;
		if (top) c = top_rgb[top];
		else if (nr) c = pd_mix(near_rgb[nr], nr == 1 ? 0x556 : near_rgb[nr], flash);
		else if (fr) c = pd_mix(far_rgb[fr], fr == 1 ? 0x89a : far_rgb[fr], flash);
		else c = pd_mix(sky_rgb[sk], 0xddf, flash);
		pd_pal(i, white ? pd_mix(c, 0xfff, white) : c);
	}
}

/* -- lightning: a forked path from the clouds to a roof -- */

#define BOLT_N 48
static int16_t seg[BOLT_N][4];  /* x0 y0 x1 y1 */
static int bolt_n;
static uint32_t bolt_t;         /* the frame it struck */

static void bolt_add(int x0, int y0, int x1, int y1) {
	if (bolt_n >= BOLT_N) return;
	seg[bolt_n][0] = (int16_t)x0; seg[bolt_n][1] = (int16_t)y0;
	seg[bolt_n][2] = (int16_t)x1; seg[bolt_n][3] = (int16_t)y1;
	bolt_n++;
}

/* From (x, 0) down to y_end, jagged; a fork from a third of the way
 * and another from halfway, each fading out in the air. */
static void bolt_make(int x, int y_end) {
	int y = 0, i, fx[2] = { 0, 0 }, fy[2] = { 0, 0 }, n = 0, f, k;
	bolt_n = 0;
	for (i = 0; y < y_end && i < 24; i++) {
		int nx = x + (int)(rnd() % 21) - 10, ny = y + 8 + (int)(rnd() % 10);
		if (ny > y_end) ny = y_end;
		bolt_add(x, y, nx, ny);
		x = nx; y = ny;
		if (i == 3 || i == 7) { fx[n] = x; fy[n] = y; n++; }
	}
	for (f = 0; f < n; f++) {
		x = fx[f]; y = fy[f];
		for (k = 0; k < 5; k++) {
			int nx = x + (f ? 10 : -10) + (int)(rnd() % 7) - 3, ny = y + 6 + (int)(rnd() % 6);
			bolt_add(x, y, nx, ny);
			x = nx; y = ny;
		}
	}
}

static void bolt_draw(void) {
	int i, k;
	for (i = 0; i < bolt_n; i++)
		for (k = 0; k < 3; k++)
			pdp_line(5 + k, seg[i][0], seg[i][1], seg[i][2], seg[i][3], 1);
}

/* -- the scenes -- */

static uint32_t start_t;        /* the storm's first frame */

static void storm_begin(void) {
	pd_job_finish(pd_city_job);
	start_t = pd_frame_of_bar(4);
	sky_rgb[0] = 0x112; sky_rgb[1] = 0x223;
	far_rgb[1] = 0x112; far_rgb[2] = 0x553; far_rgb[3] = 0x652;
	near_rgb[1] = 0x001; near_rgb[2] = 0x224; near_rgb[3] = 0xdb4;
	top_rgb[1] = 0x446; top_rgb[2] = 0x89b; top_rgb[3] = 0xbbc; top_rgb[4] = 0x102;
	top_rgb[5] = 0x3a3; top_rgb[6] = 0x161; top_rgb[7] = 0xfff;
	flash = 0;
	/* fewer planes: no clouds and fewer windows (6); with 4, only the
	 * near rooftops behind the rain, the mouse and the lightning */
	pd_keep(0xfa, 0xe8);
	palette();
	pd_put(&sky, 0, 0, 0, 0, 0, PD_W, PD_H);
	bolt_n = 0;
}

/* where the mouse is: world x on the near panorama, height, and its
 * jump. Run by the mouse scene, from its start. */
static int m_x, m_y, m_vy, m_air;      /* m_y 8.8, screen */

/* Planes 5-7 in columns x0 .. x1-1 of rows y0 .. y1-1: the rain (two
 * depths, or the far one alone) and nothing in front of it. */
#define RAIN0_X(u) (((u) / 3) % (RAIN_W / 2))
#define RAIN0_Y(u) (RAIN_H / 2 - ((u) * 5) % (RAIN_H / 2))
static void front(int u, bool heavy_rain, int x0, int x1, int y0, int y1) {
	if (x0 < 0) x0 = 0;
	if (x1 > PD_W) x1 = PD_W;
	if (y1 <= y0 || x1 <= x0) return;
	/* rain: the far streaks slow, the near ones fast, slanting */
	pd_put(&rain[0], 5, RAIN0_X(u) + x0, RAIN0_Y(u) + y0, x0, y0, x1 - x0, y1 - y0);
	if (heavy_rain)
		pd_put(&rain[1], 6, (u * 2 / 3) % (RAIN_W / 2) + x0, RAIN_H / 2 - (u * 11) % (RAIN_H / 2) + y0,
			x0, y0, x1 - x0, y1 - y0);
	else
		pd_planes_fill(6, 1, x0, y0, x1 - x0, y1 - y0, 0);
	pd_planes_fill(7, 1, x0, y0, x1 - x0, y1 - y0, 0);
}

/* The layers; planes 5-7 except a box (the mouse's, put together in
 * RAM: mouse_frame()). */
static void layers(uint32_t t, bool heavy_rain, int bx0, int bx1, int by0, int by1) {
	int u = (int)(t - start_t);
	pd_put_wrap(&far, 1, u / 2, 0, 0, PD_H);
	pd_put_wrap(&near_, 3, u * 2, 0, 0, PD_H);
	if (by1 <= by0 || bx1 <= bx0) { front(u, heavy_rain, 0, PD_W, 0, PD_H); return; }
	front(u, heavy_rain, 0, PD_W, 0, by0);
	front(u, heavy_rain, 0, bx0, by0, by1);
	front(u, heavy_rain, bx1, PD_W, by0, by1);
	front(u, heavy_rain, 0, PD_W, by1, PD_H);
}

static void lightning(uint32_t t) {
	/* strikes on the thunder: bars 5 and 7 */
	static const int strike_bar[2] = { 4, 6 };
	int i;
	for (i = 0; i < 2; i++)
		if (t == pd_frame_of_bar(strike_bar[i])) {
			int x = 90 + (int)(rnd() % 140);
			bolt_make(x, 150 + (int)(rnd() % 30));
			bolt_t = t;
			ar_shake(3, 10);
		}
	if (bolt_n && t - bolt_t < 40) {
		uint32_t a = t - bolt_t;
		/* the flash: white, dark, white again, then dying away */
		flash = a < 2 ? 256 : a < 4 ? 40 : a < 6 ? 220 : (int)(220 * (40 - a) / 34);
		if (a < 10 && a != 3) bolt_draw();
	} else flash = 0;
	/* a flicker in the clouds now and then, no bolt */
	if (!flash && (t % 97) == 13) flash = 60;
}

static void storm_frame(uint32_t t, uint32_t lt) {
	(void)lt;
	layers(t, true, 0, 0, 0, 0);
	lightning(t);
	palette();
}

static void mouse_begin(void) {
	if (!box.pl[0]) pd_canvas_new(&box, 48, 56, 3);
	white = 0;
	m_x = 0;
	m_y = 0;
	m_air = 0;
}

static void mouse_frame(uint32_t t, uint32_t lt) {
	int u = (int)(t - start_t), wx, ground, sx, f;
	uint32_t len = pd_frame_of_bar(12) - pd_frame_of_bar(8);
	/* in from the left over the first beat, then keeping pace at a
	 * third of the way across */
	sx = lt < 40 ? -40 + (int)lt * 3 : 80;
	wx = u * 2 + sx + 24;
	ground = roof[wx % NEAR_W];
	{
		int ahead = roof[(wx + 22) % NEAR_W];
		if (!m_air && (ahead == 255 || ahead + 0 < ground - 12 || lt + 30 > len)) {
			/* a gap, or a wall, or the end: jump (the last one high) */
			m_vy = lt + 30 > len ? -1400 : -700;
			m_air = 1;
		}
	}
	if (m_air) {
		m_y += m_vy;
		m_vy += 64;
		if (m_vy > 0 && ground != 255 && (m_y >> 8) >= ground && lt + 30 <= len) {
			m_y = ground << 8;
			m_air = 0;
		}
	} else {
		m_y = (ground == 255 ? 239 : ground) << 8;
	}
	f = m_air ? 2 : (int)(lt / 5) & 1;
	/* up, up and into the light: the last 16 frames to white */
	white = lt + 16 > len ? (int)((lt + 16 - len) * 256 / 16) : 0;
	{
		/* the mouse's box: the rain there and the mouse, put together
		 * in RAM and blitted in one pass -- the rain blitted and the
		 * mouse over it, the mouse flashed when the beam came by in
		 * between */
		int y = (m_y >> 8) - 56 + 1;
		int x0 = sx < 0 ? 0 : sx, x1 = sx + 48 > PD_W ? PD_W : sx + 48;
		int y0 = y < 0 ? 0 : y, y1 = y + 56 > PD_H ? PD_H : y + 56;
		if (y1 > y0 && x1 > x0 && box.pl[0]) {
			pd_canvas_put(&box, 0, &rain[0], false, RAIN0_X(u) + x0, RAIN0_Y(u) + y0, 0, 0, x1 - x0, y1 - y0);
			pd_canvas_zero(&box, 1, 0, y1 - y0);
			pd_canvas_zero(&box, 2, 0, y1 - y0);
			pd_canvas_put(&box, 0, &mouse[f], true, x0 - sx, y0 - y, 0, 0, x1 - x0, y1 - y0);
			layers(t, false, x0, x1, y0, y1);
			pd_put(&box, 5, 0, 0, x0, y0, x1 - x0, y1 - y0);
		} else layers(t, false, 0, 0, 0, 0);
	}
	lightning(t);
	palette();
}

const pd_scene_t pd_scene_storm = { "the storm", 4, storm_begin, storm_frame };
const pd_scene_t pd_scene_mouse = { "the mouse", 8, mouse_begin, mouse_frame };

void pd_city_store(pd_store_t *st) {
	int k;
	if (st->writing) while (!pd_city_job()) { }
	pd_store_planar(st, &sky);
	pd_store_planar(st, &far);
	pd_store_planar(st, &near_);
	for (k = 0; k < 2; k++) pd_store_planar(st, &rain[k]);
	for (k = 0; k < 3; k++) pd_store_planar(st, &mouse[k]);
	pd_store_bytes(st, roof, sizeof(roof));
	pd_store_u32(st, &seed);        /* the lightning goes on from it */
	if (!st->writing && !st->bad) step = 8;
}
