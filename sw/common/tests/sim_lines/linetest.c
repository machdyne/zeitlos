/*
 * zgfx's lines by the CPU, for a bitstream without the line rasterizer
 * (the console's Lakritz, docs/profiles.md): random lines (set, clear,
 * XOR; clipped and not; steep, shallow, reversed, points, off the
 * screen) through z_fb_hw_line(), against a model written from
 * rtl/gpu/gpu_raster.v's own equations (err_init = dx + dy, e2 = 2 err,
 * x steps on e2 > dy, y on e2 < dx, err1 then err2) with zgfx's clamp to
 * the screen and the scissor, pixel for pixel.
 *
 * The simulator's own rasterizer model is of the old 512x384 screen, so
 * the hardware path is not compared here; ZSIM_CONSOLE=1 makes the
 * bitstream say it has none.
 *   make -C sw/common/tests/sim_lines test
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../../zeitlos.h"
#include "../../zgfx.h"
#include "../../zsoc.h"

static uint32_t seed = 12345;
static int rnd(int n) { seed = seed * 1103515245u + 12345u; return (int)((seed >> 16) % (uint32_t)n); }

static uint32_t model[9600];

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void model_line(int x0, int y0, int x1, int y1, int op, const z_clip_t *c) {
	int dx, dy, err, e2, x, y;
	x0 = clampi(x0, 0, 639); x1 = clampi(x1, 0, 639);
	y0 = clampi(y0, 0, 479); y1 = clampi(y1, 0, 479);
	dx = x1 >= x0 ? x1 - x0 : x0 - x1;
	dy = y1 >= y0 ? -(y1 - y0) : (y1 - y0);
	err = dx + dy; x = x0; y = y0;
	for (;;) {
		if (!c || (x >= c->x0 && x <= c->x1 && y >= c->y0 && y <= c->y1)) {
			uint32_t b = (uint32_t)y * 640u + (uint32_t)x, m = 1u << (b % 32);
			if (op == 2) model[b / 32] ^= m; else if (op) model[b / 32] |= m; else model[b / 32] &= ~m;
		}
		if (x == x1 && y == y1) break;
		e2 = err * 2;
		{
			int gt = e2 > dy, lt = e2 < dx, err1 = gt ? err + dy : err;
			if (gt) x += x1 >= x0 ? 1 : -1;
			if (lt) y += y1 >= y0 ? 1 : -1;
			err = lt ? err1 + dx : err1;
		}
	}
}

int main(void) {
	volatile uint32_t *v = (volatile uint32_t *)0x20000000;
	uint32_t i;
	int k, bad = 0;
	if (!z_soc_csrs_present() || z_soc_has_feature(Z_FEATURE_GPU_RASTER)) {
		printf("linetest: this bitstream has the rasterizer (run with ZSIM_CONSOLE=1)\n");
		return 1;
	}
	for (i = 0; i < 9600; i++) model[i] = v[i] = (i * 2654435761u) ^ 0x5a5a5a5au;
	for (k = 0; k < 600; k++) {
		int x0 = rnd(700) - 30, y0 = rnd(540) - 30, x1, y1;
		z_clip_t c = { rnd(300), rnd(200), 300 + rnd(339), 200 + rnd(279) };
		switch (k % 4) {
		case 0: x1 = x0 + rnd(40) - 20; y1 = y0 + rnd(400) - 200; break;   /* steep */
		case 1: x1 = x0 + rnd(600) - 300; y1 = y0 + rnd(40) - 20; break;   /* shallow */
		case 2: x1 = x0; y1 = y0; break;                                   /* a point */
		default: x1 = rnd(640); y1 = rnd(480); break;
		}
		z_fb_hw_line(x0, y0, x1, y1, k % 3, (k & 4) ? &c : NULL);
		model_line(x0, y0, x1, y1, k % 3, (k & 4) ? &c : NULL);
	}
	z_fb_hw_sync();
	for (i = 0; i < 9600; i++) if (v[i] != model[i]) bad++;
	printf("linetest: %d of 9600 words differ (600 lines, by the CPU)\n", bad);
	return bad != 0;
}
