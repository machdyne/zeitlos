/*
 * planedemo -- bars 3-4: ignition. The word stays, white hot, and a
 * plasma of fire floods in behind it; on the snare roll everything
 * burns to white, which is the storm's first lightning.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Planes 0-6: a field of 128 values, computed while the clock plays
 * and drawn once. Plane 7: the word. Nothing is redrawn after the
 * first frame: the motion is all palette -- 128 entries rotated through
 * a fire gradient a little every frame, and kicked on the drums.
 */

#include <string.h>

#include "pd.h"
#include "pd_gfx.h"
#include "pd_scenes.h"

static pd_chunky_t field;
static pd_planar_t pic, word;
static int made_rows = -1;

/* One step of the making: a few rows of the field, then the planes. */
bool pd_plasma_job(void) {
	int x, y, n;
	if (pic.pl[0]) return true;             /* made (or read: pd_plasma_store) */
	if (made_rows < 0) {
		if (!pd_chunky_new(&field, PD_W, PD_H)) return true;
		made_rows = 0;
	}
	for (n = 0; n < 8 && made_rows < PD_H; n++, made_rows++) {
		y = made_rows;
		for (x = 0; x < PD_W; x++) {
			int dx = x - 160, dy = y - 112;
			int d = (int)pd_isqrt((uint32_t)(dx * dx + dy * dy));
			int v = pd_sin(x * 4096 / 113) + pd_sin(y * 4096 / 97) +
				pd_sin((x + y) * 4096 / 151) + pd_sin(d * 4096 / 61);
			/* -4..4 in Q14 -> 0..127 */
			v = (v + 4 * 16384) * 127 / (8 * 16384);
			field.px[y * PD_W + x] = (uint8_t)v;
		}
	}
	if (made_rows < PD_H) return false;
	pd_planar_from(&pic, &field, 0, 7, -1);
	memset(field.px, 0, (size_t)PD_W * PD_H);
	pdc_text(&field, (PD_W - pdc_text_w("zeitlos", 4)) / 2, 96, "zeitlos", 1, 4);
	pd_planar_from(&word, &field, 0, 1, -1);
	pd_chunky_free(&field);
	return true;
}

/* fire: black, deep red, orange, yellow, white, and back */
static uint16_t fire(int i) {
	static const uint16_t k[8] = { 0x000, 0x301, 0x902, 0xe40, 0xf90, 0xfe4, 0xf90, 0x601 };
	int a = (i & 255) >> 5, f = (i & 31) * 8;
	return pd_mix(k[a], k[(a + 1) & 7], f);
}

static void begin(void) {
	pd_job_finish(pd_plasma_job);
	/* fewer planes: the field in coarser steps; the word kept */
	pd_keep(0xfc, 0xf0);
	pd_put(&pic, 0, 0, 0, 0, 0, PD_W, PD_H);
	pd_put(&word, 7, 0, 0, 0, 0, PD_W, PD_H);
}

static void frame(uint32_t t, uint32_t lt) {
	uint32_t len = pd_frame_of_bar(4) - pd_frame_of_bar(2);
	int i;
	/* out of the burning clock (hot yellow) over the first beat;
	 * brighter on each kick; to white over the last eighth (the roll) */
	int up = lt < 28 ? (int)(lt * 256 / 28) : 256;
	int kick = pdm_level(PD_CH_KICK);
	int white = lt + 28 > len ? (int)((lt + 28 - len) * 256 / 28) : 0;
	for (i = 0; i < 128; i++) {
		uint16_t c = fire(i * 2 + (int)lt * 3);
		c = pd_mix(0xfd8, c, up);
		c = pd_mix(c, 0xfff, kick / 6);
		pd_pal(i, pd_mix(c, 0xfff, white));
	}
	for (i = 128; i < 256; i++)
		pd_pal(i, pd_mix(pd_mix(0xfff, 0xfc6, (int)(lt % 8) * 16), 0xfff, white));
	(void)t;
}

const pd_scene_t pd_scene_plasma = { "ignition", 2, begin, frame };

void pd_plasma_store(pd_store_t *st) {
	if (st->writing) while (!pd_plasma_job()) { }
	pd_store_planar(st, &pic);
	pd_store_planar(st, &word);
	if (!st->writing && !st->bad) made_rows = PD_H;
}
