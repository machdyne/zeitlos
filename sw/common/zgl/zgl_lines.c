/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zgl_lines.c -- ZGL_LINES: lines and points into the arcade runtime's
 * picture as it is shown, by the line rasterizer (ar_plane_line(),
 * rtl/gpu/gpu_raster.v), on every board and in every mode.
 * docs/zgl.md.
 *
 * There is no back buffer -- a window has no off-screen VRAM, and in
 * colour the planes are the picture -- so a frame is a display list:
 * the lines submitted since the last zgl_frame(). At the end of a
 * frame last frame's lines that changed or went are drawn again in
 * colour 0 to erase them, and this frame's are drawn. Comparing
 * index by index keeps a still scene still: nothing is touched where
 * nothing moved (the way space3d always drew). A line that crosses an
 * erased one may lose the shared pixel until it is drawn again, which
 * in motion is not seen.
 *
 * A colour index is drawn in planes 0 .. n-1: a plane gets the line
 * where the index has that bit, so lines of different colours that
 * cross mix their bits at the crossing (OR); erasing clears all n. In
 * monochrome, index 0 is black and every other index white.
 */

#pragma GCC optimize ("O2")

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "zgl.h"
#include "zgl_int.h"
#include "../games/zarcade.h"

typedef struct {
	int16_t x0, y0, x1, y1;
	uint8_t c;
} zl_line_t;

static zl_line_t *zl_buf[2];
static int zl_n[2];
static int zl_cur;
static bool zl_open;
static uint8_t zl_bits;
static zgl_info_t zl_info;

bool zgl_open(const zgl_desc_t *d) {
	int max = d && d->max_lines ? d->max_lines : 512;
	zgl_close();
	if (d && d->output == ZGL_FRAMES) {
		if (!zglf_open) return false;           /* zgl_frames.o not linked */
		memset(&zl_info, 0, sizeof(zl_info));
		zl_info.output = ZGL_FRAMES;
		zl_info.backend = ZGL_BACKEND_SOFTWARE;
		if (!zglf_open(d, &zl_info)) return false;
		zgl_reset_state();
		zgl_s.output = ZGL_FRAMES;
		zl_open = true;
		return true;
	}
	if (d && d->output != ZGL_LINES) return false;
	zl_buf[0] = malloc((size_t)max * sizeof(zl_line_t));
	zl_buf[1] = malloc((size_t)max * sizeof(zl_line_t));
	if (!zl_buf[0] || !zl_buf[1]) { zgl_close(); return false; }
	memset(&zl_info, 0, sizeof(zl_info));
	zl_info.output = ZGL_LINES;
	zl_info.backend = ZGL_BACKEND_LINE;
	zl_info.max_lines = (uint16_t)max;
	zl_bits = d ? d->index_bits : 0;
	zl_n[0] = zl_n[1] = 0;
	zl_cur = 0;
	zl_open = true;
	zgl_reset_state();
	zgl_s.output = ZGL_LINES;
	return true;
}

void zgl_close(void) {
	if (zl_open && zl_info.output == ZGL_FRAMES && zglf_close) zglf_close();
	free(zl_buf[0]);
	free(zl_buf[1]);
	zl_buf[0] = zl_buf[1] = NULL;
	zl_open = false;
}

const zgl_info_t *zgl_info(void) {
	return &zl_info;
}

void zgl_back_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t c) {
	int n;
	zl_line_t *l;
	if (!zl_open) return;
	n = zl_n[zl_cur];
	if (n >= zl_info.max_lines) { zl_info.overflows++; return; }
	l = &zl_buf[zl_cur][n];
	l->x0 = (int16_t)x0; l->y0 = (int16_t)y0;
	l->x1 = (int16_t)x1; l->y1 = (int16_t)y1;
	l->c = c;
	zl_n[zl_cur] = n + 1;
}

/* how many planes an index is drawn in, now */
static int planes_now(void) {
	int n = 0, shown;
	if (!ar_color()) return 1;
	shown = ar_colours();
	while ((1 << n) < shown) n++;
	if (zl_bits && zl_bits < n) n = zl_bits;
	return n ? n : 1;
}

static void draw(const zl_line_t *l, bool erase, int np) {
	int k;
	if (np == 1 && !ar_color()) {
		ar_plane_line(0, l->x0, l->y0, l->x1, l->y1, (!erase && l->c) ? 1 : 0);
		return;
	}
	for (k = 0; k < np; k++) {
		if (!erase && !((l->c >> k) & 1)) continue;
		ar_plane_line(k, l->x0, l->y0, l->x1, l->y1, erase ? 0 : 1);
	}
}

void zgl_frame(void) {
	int prev = zl_cur ^ 1;
	int n_new = zl_n[zl_cur], n_old = zl_n[prev];
	int np, i;
	if (!zl_open) return;
	if (zl_info.output == ZGL_FRAMES) {
		zglf_frame();
		return;
	}
	np = planes_now();
	zl_info.lines_erased = 0;
	for (i = 0; i < n_old; i++) {
		const zl_line_t *o = &zl_buf[prev][i];
		if (i < n_new) {
			const zl_line_t *n = &zl_buf[zl_cur][i];
			if (o->x0 == n->x0 && o->y0 == n->y0 && o->x1 == n->x1 &&
			    o->y1 == n->y1 && o->c == n->c)
				continue;                            /* still there */
		}
		draw(o, true, np);
		zl_info.lines_erased++;
	}
	for (i = 0; i < n_new; i++)
		draw(&zl_buf[zl_cur][i], false, np);
	zl_info.lines = (uint32_t)n_new;
	zl_info.lines_drawn = (uint32_t)n_new;
	zl_info.frames++;
	zl_n[prev] = 0;
	zl_cur = prev;
}

void zgl_invalidate(void) {
	zl_n[zl_cur ^ 1] = 0;
}

/* -- the palette as shading ramps (both outputs) --------------------------- */

void zglRamp(int first, int n, uint16_t dark, uint16_t light) {
	int i;
	int r0 = (dark >> 8) & 15, g0 = (dark >> 4) & 15, b0 = dark & 15;
	int r1 = (light >> 8) & 15, g1 = (light >> 4) & 15, b1 = light & 15;
	for (i = 0; i < n && first + i < 256; i++) {
		int d = n > 1 ? n - 1 : 1;
		int r = r0 + ((r1 - r0) * i * 2 + d) / (2 * d);
		int g = g0 + ((g1 - g0) * i * 2 + d) / (2 * d);
		int b = b0 + ((b1 - b0) * i * 2 + d) / (2 * d);
		ar_pal(first + i, (uint16_t)(r << 8 | g << 4 | b));
	}
}
