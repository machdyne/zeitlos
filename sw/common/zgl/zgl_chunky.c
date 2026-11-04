/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zgl_chunky.c -- ZGL_FRAMES into chunky VRAM (zgl_desc_t.flags
 * ZGL_CHUNKY; docs/zgl.md, "Chunky"): by the triangle engine where the
 * bitstream has one (rtl/gpu/gpu_tri.v, docs/gpu_tri.md), else by the
 * software strips, a byte a pixel straight into VRAM -- no conversion
 * to planes. The program has chunky game mode on itself.
 *
 * And a black-and-white window's frames, when the engine is there:
 * its MONO output writes the 1 bpp desktop at the window's place,
 * inside its one visible rectangle (zglc_mono()).
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zgl.h"
#include "zgl_int.h"
#include "../zsoc.h"
#include "zgl_t2.h"

static bool use_engine;
static uint8_t lum_now[256];
static bool lum_valid;

bool zglc_probe(uint8_t flags) {
	if ((flags & ZGL_NO_ENGINE) || !z_gtri_present()) return false;
	*z_gtri_reg(Z_GTRI_W_RESET) = 1;
	*z_gtri_reg(Z_GTRI_W_VBASE) = (uint32_t)(uintptr_t)Z_CHUNKY_VRAM;
	use_engine = true;
	lum_valid = false;
	return true;
}

/* A black-and-white window's frame: the window at (ox, oy) on the
 * screen, the visible picture columns x0..x1 and rows y0..y1, each
 * index's brightness (0..16). Written to the engine only when changed
 * (and the brightnesses only when the palette changed). */
void zglc_mono(int ox, int oy, int x0, int x1, int y0, int y1, const uint8_t *lum) {
	int i;
	if (!lum_valid || memcmp(lum, lum_now, sizeof(lum_now))) {
		for (i = 0; i < 256; i++) *z_gtri_reg(Z_GTRI_W_LUM) = (uint32_t)i | (uint32_t)lum[i] << 8;
		memcpy(lum_now, lum, sizeof(lum_now));
		lum_valid = true;
	}
	*z_gtri_reg(Z_GTRI_W_CTRL) = Z_GTRI_CTRL_MONO;
	*z_gtri_reg(Z_GTRI_W_WIN) = (uint32_t)ox | (uint32_t)oy << 16;
	*z_gtri_reg(Z_GTRI_W_CLIPX) = (uint32_t)x0 | (uint32_t)x1 << 16;
	*z_gtri_reg(Z_GTRI_W_CLIPY) = (uint32_t)y0 | (uint32_t)y1 << 16;
}

int zglc_open(uint8_t flags) {
	int kind = 0;
	if (!(flags & ZGL_NO_ENGINE)) {
		uint32_t id = *z_gtri_reg(Z_GTRI_W_ID);
		if (id == Z_GTRI_ID) kind = 1;
		if (id == T2_ID) kind = 2;
	}
	use_engine = kind != 0;
	if (use_engine) {
		*z_gtri_reg(Z_GTRI_W_RESET) = 1;
		*z_gtri_reg(Z_GTRI_W_VBASE) = (uint32_t)(uintptr_t)Z_CHUNKY_VRAM;
		/* each frame starts going out in vertical blanking */
		*z_gtri_reg(Z_GTRI_W_CTRL) = Z_GTRI_CTRL_VSYNC;
	}
	return kind;
}

/* a command word: the store waits while the engine's FIFO is full */
void zglc_put(uint32_t w) {
	*z_gtri_reg(Z_GTRI_W_CMD) = w;
}

volatile uint32_t *zglc_cmd(void) {
	return z_gtri_reg(Z_GTRI_W_CMD);
}

void zglc_finish(void) {
	if (!use_engine) return;
	while (*z_gtri_reg(Z_GTRI_W_STATUS) & 1u) ;
}

/* A software strip's rows, columns cx0..cx1, to chunky VRAM: whole
 * words where four pixels are inside, bytes at the ends. */
void zglc_strip(int32_t y0, int rows, int32_t cx0, int32_t cx1, const uint8_t *col) {
	int r;
	for (r = 0; r < rows; r++) {
		const uint8_t *src = &col[r * 320];
		uint32_t y = (uint32_t)(y0 + r);
		int32_t x = cx0;
		while (x <= cx1) {
			uint32_t seg = (uint32_t)x / 80u, off = (uint32_t)x % 80u;
			volatile uint32_t *d = z_chunky_seg(y, seg);
			if ((x & 3) == 0 && x + 3 <= cx1) {
				/* to the end of the segment, or of the span */
				int32_t end = (int32_t)(seg * 80u + 80u) - 1;
				if (end > cx1) end = cx1;
				while (x + 3 <= end) {
					d[off >> 2] = *(const uint32_t *)&src[x];
					x += 4;
					off += 4;
				}
			} else {
				((volatile uint8_t *)d)[off] = src[x];
				x++;
			}
		}
	}
}
