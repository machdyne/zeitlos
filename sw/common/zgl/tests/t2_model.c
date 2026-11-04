/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * T2, modelled on the host: the geometry unit's own program (t2fw.c,
 * which the engine's zeitlos32g core runs) fed a frame's stream, and
 * its records drawn by the raster's model (t2_raster.c). docs/zgl_t2.md.
 */
#include <stdint.h>
#include <string.h>
#include "../zgl_t2.h"
#include "t2_raster.h"

extern t2_stats_t t2fw_stats;

static const uint32_t *in_w;
static uint32_t in_n, in_i;
static int in_over, ended;
static uint8_t (*out_pic)[320];

uint32_t t2io_in(void) {
	if (in_i >= in_n) { in_over = 1; return T2_END << 24; }
	return in_w[in_i++];
}

volatile uint32_t *t2io_rec(int n) { return t2r_rec(n); }

void t2io_frame(int32_t x0, int32_t x1, int32_t y0, int32_t y1, uint8_t clear) {
	t2r_frame(x0, x1, y0, y1, clear);
}

void t2io_end(void) {
	t2r_draw(out_pic, &t2fw_stats);
	ended = 1;
}

int t2_model_run(const uint32_t *w, uint32_t n, uint8_t pic[240][320], t2_stats_t *st) {
	in_w = w; in_n = n; in_i = 0; in_over = 0; ended = 0;
	out_pic = pic;
	memset(&t2fw_stats, 0, sizeof(t2fw_stats));
	while (!ended && !in_over)
		if (t2fw_step() < 0) return -1;
	if (st) *st = t2fw_stats;
	return ended && !in_over ? 0 : -1;
}

/* the words the last t2_model_run() took: a stream of several frames is
 * run a frame a call */
uint32_t t2_model_used(void) { return in_i; }
