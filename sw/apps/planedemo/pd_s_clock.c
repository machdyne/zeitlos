/*
 * planedemo -- bars 1-2: timeless. A clock face fades up out of the
 * dark, ticking on the beat; on the first beat of bar 2 its hands fall
 * off, bounce and clatter to the floor, and `zeitlos` -- timeless --
 * types itself where they were.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Planes 0-4: the face, 32 colours, drawn once. Planes 5-7: the hands
 * and the word, redrawn every frame -- lines by the rasterizer, the
 * word a 1bpp shape -- so the face is never touched again. The palette
 * puts 5-7 in front: index i shows its top colour when i >> 5 is not
 * 0, else face colour i & 31.
 */

#include <string.h>

#include "pd.h"
#include "pd_gfx.h"
#include "pd_scenes.h"

#define CX 160
#define CY 112
#define R_FACE 84
#define R_BEZEL 92

static pd_planar_t face, word;
static bool made;

/* face colours, 0..31: 0-7 the room (vignette), 8-15 the dial, 16-23
 * the brass bezel, 24 minute ticks, 25 hour ticks, 26 numerals, 27 the
 * cap, 28 the maker's name */
static uint16_t face_rgb[32];
/* the top layer: 1 hour and minute hands, 2 the second hand, 4 the word */
static const uint16_t top_rgb[8] = {
	0x000, 0xc93, 0xe22, 0x112, 0xfff, 0xfff, 0xfff, 0xfff
};

static void make(void) {
	pd_chunky_t c;
	int x, y, i;

	if (made || !pd_chunky_new(&c, PD_W, PD_H)) return;
	for (y = 0; y < PD_H; y++)
		for (x = 0; x < PD_W; x++) {
			int dx = x - CX, dy = y - CY;
			uint32_t d2 = (uint32_t)(dx * dx + dy * dy);
			int v;
			if (d2 <= (uint32_t)(R_FACE * R_FACE)) {
				/* the dial: lighter toward the top left, in 16ths, dithered */
				int l = 7 * 16 - (int)pd_isqrt(d2) * 48 / R_FACE - (dx + dy) * 16 / 50;
				l = (l + pd_dither(x, y)) >> 4;
				v = 8 + (l < 0 ? 0 : l > 7 ? 7 : l);
			} else if (d2 <= (uint32_t)(R_BEZEL * R_BEZEL)) {
				/* the bezel: lit from the top left, a ridge in the middle */
				int a = pd_atan2(dy, dx), r = (int)pd_isqrt(d2) - R_FACE;
				int lit = (pd_cos(a - 2560) >> 11) + 4 - (r - 4 < 0 ? 4 - r : r - 4);
				v = 16 + (lit < 0 ? 0 : lit > 7 ? 7 : lit);
			} else {
				int r = (int)pd_isqrt(d2);
				v = (7 * 16 - r * 16 / 26 + pd_dither(x, y)) >> 4;
				if (v < 0) v = 0;
			}
			c.px[y * PD_W + x] = (uint8_t)v;
		}
	for (i = 0; i < 60; i++) {
		int a = i * 4096 / 60, l = (i % 5) ? 5 : 12, r0 = R_FACE - 3 - l, k;
		for (k = 0; k < l; k++) {
			int r = r0 + k;
			int px = CX + (pd_sin(a) * r >> 14), py = CY - (pd_cos(a) * r >> 14);
			pdc_set(&c, px, py, i % 5 ? 24 : 25);
			if (i % 5 == 0) {
				pdc_set(&c, px + 1, py, 25);
				pdc_set(&c, px, py + 1, 25);
			}
		}
	}
	{
		static const char *const num[4] = { "12", "3", "6", "9" };
		static const int nx[4] = { CX, CX + 54, CX, CX - 54 }, ny[4] = { CY - 60, CY - 6, CY + 46, CY - 6 };
		for (i = 0; i < 4; i++)
			pdc_text(&c, nx[i] - pdc_text_w(num[i], 2) / 2, ny[i], num[i], 26, 2);
		pdc_text(&c, CX - pdc_text_w("ZEITLOS", 1) / 2, CY + 26, "ZEITLOS", 28, 1);
	}
	pdc_disc(&c, CX, CY, 4, 27);
	pd_planar_from(&face, &c, 0, 5, -1);

	/* the word: big, one plane */
	memset(c.px, 0, (size_t)PD_W * PD_H);
	pdc_text(&c, (PD_W - pdc_text_w("zeitlos", 4)) / 2, 0, "zeitlos", 1, 4);
	c.h = 32;
	pd_planar_from(&word, &c, 0, 1, -1);
	c.h = PD_H;
	pd_chunky_free(&c);

	/* the colours */
	for (i = 0; i < 8; i++) face_rgb[i] = pd_rgb(0, i / 3, i * 2 / 3 + 1);
	face_rgb[0] = 0x000;
	for (i = 0; i < 8; i++) face_rgb[8 + i] = pd_rgb(10 + i * 5 / 7, 9 + i * 5 / 7, 7 + i * 4 / 7);
	for (i = 0; i < 8; i++) face_rgb[16 + i] = pd_rgb(4 + i, 3 + i, 1 + i / 2);
	face_rgb[24] = 0x444; face_rgb[25] = 0x222; face_rgb[26] = 0x223;
	face_rgb[27] = 0xb82; face_rgb[28] = 0x844;
	made = true;
}

/* The palette, the face brought up by `fade` (0..256), and at the
 * very end heating (`heat` 0..256) toward the fire that follows */
static void palette(int fade, int word_glow, int heat) {
	int i;
	for (i = 0; i < 256; i++) {
		int top = i >> 5, f = i & 31;
		uint16_t c;
		/* fewer planes: the face's low bits are gone, so f is the first
		 * of a run of shades: show the one in its middle; with 4, the
		 * top layer's 3 is the word (there is no shadow) */
		if (pd_planes < 8) f = f >= 24 ? (pd_planes < 6 || f < 28 ? 26 : 28) : f + (pd_planes < 6 ? 4 : 2);
		if (pd_planes < 6 && top == 3) top = 4;
		c = top ? top_rgb[top] : face_rgb[f];
		if (top >= 4) c = pd_mix(0xfff, 0xf82, word_glow);
		else if (top == 3) c = pd_mix(0x000, 0x112, fade);
		else c = pd_mix(0x000, c, fade);
		if (heat) c = pd_mix(c, heat < 128 ? 0xf41 : 0xfd8, heat < 128 ? heat * 2 : 256 - (256 - heat) / 2);
		pd_pal(i, c);
	}
}

/* -- the hands -- */

typedef struct {
	int len, tail, width;       /* pixels */
	int value;                  /* top-layer colour */
	/* falling: position of the pivot (8.8), its velocity, the angle
	 * (4096ths) and its turning, a bounce counter */
	int32_t x, y, vx, vy;
	int a, w, bounces;
} hand_t;

static hand_t hand[3];
#define FALL_ROW 16             /* the first beat of bar 2 */

static void hands_reset(void) {
	/* ten past ten, with the second hand at 45 */
	static const int len[3] = { 48, 70, 76 }, tail[3] = { 10, 12, 18 }, wid[3] = { 3, 2, 1 };
	static const int ang[3] = { 4096 * 10 / 12 + 4096 / 72, 4096 * 2 / 12, 4096 * 45 / 60 };
	int i;
	for (i = 0; i < 3; i++) {
		hand[i].len = len[i];
		hand[i].tail = tail[i];
		hand[i].width = wid[i];
		hand[i].value = i == 2 ? 2 : 1;
		hand[i].x = CX << 8;
		hand[i].y = CY << 8;
		hand[i].vx = hand[i].vy = 0;
		hand[i].a = ang[i];
		hand[i].w = 0;
		hand[i].bounces = 0;
	}
}

static void hand_draw(const hand_t *h) {
	int s = pd_sin(h->a), c = pd_cos(h->a), d, k;
	int px = h->x >> 8, py = h->y >> 8;
	int x0 = px - (s * h->tail >> 14), y0 = py + (c * h->tail >> 14);
	int x1 = px + (s * h->len >> 14), y1 = py - (c * h->len >> 14);
	for (d = -(h->width / 2); d <= h->width / 2; d++) {
		/* parallel strokes, a pixel apart across the hand */
		int ox = c * d >> 14, oy = s * d >> 14;
		if (!ox && !oy && d) ox = d;
		for (k = 0; k < 3; k++)
			if ((h->value >> k) & 1)
				pdp_line(5 + k, x0 + ox, y0 + oy, x1 + ox, y1 + oy, 1);
	}
}

/* One frame of a falling hand: gravity, the turn, the floor. */
static void hand_fall(hand_t *h) {
	int floor_y = 228 << 8;
	int s, c, ly;
	h->vy += 40;                         /* gravity, 8.8 pixels a frame a frame */
	h->x += h->vx;
	h->y += h->vy;
	h->a = (h->a + h->w) & 4095;
	s = pd_sin(h->a); c = pd_cos(h->a);
	/* the lowest end of the hand */
	{
		int ya = h->y + ((-c) * h->len >> 6), yb = h->y + (c * h->tail >> 6);
		ly = ya > yb ? ya : yb;
	}
	(void)s;
	if (ly > floor_y && h->vy > 0) {
		h->y -= ly - floor_y;
		if (h->bounces < 3) {
			h->vy = -h->vy * 2 / 5;
			h->w = -h->w * 3 / 5 + (h->bounces ? 0 : 24);
			h->vx = h->vx * 3 / 4;
			h->bounces++;
		} else {
			/* at rest: lie flat */
			h->vy = 0; h->vx = 0;
			h->w = 0;
			h->a = (h->a & 2048) ? 3072 : 1024;
		}
	}
}

static void begin(void) {
	make();
	hands_reset();
	palette(0, 0, 0);
	/* fewer planes: the face's shades in fewer steps; with 4, the top
	 * layer in two planes (hands 1, second hand 2, the word 3) */
	pd_keep(0xfc, 0x78);
	pd_put(&face, 0, 0, 0, 0, 0, PD_W, PD_H);
	pd_planes_fill(5, 3, 0, 0, PD_W, PD_H, 0);
}

static void frame(uint32_t t, uint32_t lt) {
	int row = pd_row_of(t), i, letters = 0;
	uint32_t fall_f = pd_frame_of_row(FALL_ROW);
	int fade = lt < 60 ? (int)(lt * 256 / 60) : 256;
	int glow = 0;

	/* the second hand ticks on each beat until the fall */
	if (row < FALL_ROW) hand[2].a = 4096 * (45 + row / 4) / 60;

	if (t == fall_f) {
		/* off they come: each its own way */
		static const int vx[3] = { -90, 70, 150 }, w[3] = { -30, 44, 70 };
		for (i = 0; i < 3; i++) {
			hand[i].vx = vx[i];
			hand[i].vy = -260 - i * 40;
			hand[i].w = w[i];
			ar_shake(3, 12);
		}
	}
	if (t >= fall_f) for (i = 0; i < 3; i++) hand_fall(&hand[i]);

	/* the word: a letter a row from row 20 */
	if (row >= 20) letters = row - 19 > 7 ? 7 : row - 19;
	if (row >= 27) glow = (int)((t - pd_frame_of_row(27)) * 4 > 256 ? 256 : (t - pd_frame_of_row(27)) * 4);

	{
		/* the last 20 frames: the clock catches fire */
		uint32_t len = pd_frame_of_bar(2);
		int heat = lt + 20 > len ? (int)((lt + 20 - len) * 256 / 20) : 0;
		palette(fade, glow, heat);
	}
	pd_planes_fill(5, 3, 0, 0, PD_W, PD_H, 0);
	for (i = 0; i < 3; i++) hand_draw(&hand[i]);
	if (letters) {
		int x0 = (PD_W - pdc_text_w("zeitlos", 4)) / 2;
		/* a shadow (3) under the letters (4) */
		int wv = pd_planes < 6 ? 3 : 4, k;
		if (pd_planes >= 6) pd_shape(word.pl[0], word.stride, 5, 3, 3, x0, 0, x0 + 3, 99, letters * 32, 32);
		pd_shape(word.pl[0], word.stride, 5, 3, wv, x0, 0, x0, 96, letters * 32, 32);
		/* the cursor after the last letter, blinking */
		if (letters < 7 || (lt / 15) % 2)
			for (k = 0; k < 3; k++)
				if ((wv >> k) & 1) pd_planes_fill(5 + k, 1, x0 + letters * 32 + 2, 96, 6, 28, 1);
	}
}

const pd_scene_t pd_scene_clock = { "timeless", 0, begin, frame };

/* What make() makes, made on the build host (pd_gfx.h, "made on the
 * build host"). */
void pd_clock_store(pd_store_t *st) {
	if (st->writing) make();
	pd_store_planar(st, &face);
	pd_store_planar(st, &word);
	pd_store_bytes(st, face_rgb, sizeof(face_rgb));
	if (!st->writing && !st->bad) made = true;
}
