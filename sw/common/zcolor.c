/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Game-mode colour: layouts, palettes and planar drawing. See zcolor.h
 * and docs/color.md.
 */

#include "zcolor.h"
#include "zgfx.h"

#define FB_W 640
#define FB_H 480

const z_color_layout_t z_color_16 = {
	4, Z_COLOR_OFFSETS(Z_COLOR_DX, Z_COLOR_DY, Z_COLOR_DX | Z_COLOR_DY)
};
const z_color_layout_t z_color_8 = {
	3, Z_COLOR_OFFSETS(Z_COLOR_DX, Z_COLOR_DY, 0)
};
const z_color_layout_t z_color_4_right = {
	2, Z_COLOR_OFFSETS(Z_COLOR_DX, 0, 0)
};
const z_color_layout_t z_color_4_below = {
	2, Z_COLOR_OFFSETS(Z_COLOR_DY, 0, 0)
};
const z_color_layout_t z_color_mono = { 1, 0 };

/* KEEP IN SYNC with rtl/gpu/gpu_video.v's palette initial block. */
const uint16_t z_color_palette_ega[16] = {
	0x000, 0x00a, 0x0a0, 0x0aa, 0xa00, 0xa0a, 0xa50, 0xaaa,
	0x555, 0x55f, 0x5f5, 0x5ff, 0xf55, 0xf5f, 0xff5, 0xfff
};

/* The "Pepto" measurement of the C64's VIC-II colours, rounded to four
 * bits a channel: black, white, red, cyan, purple, green, blue,
 * yellow, orange, brown, light red, dark grey, grey, light green,
 * light blue, light grey. */
const uint16_t z_color_palette_c64[16] = {
	0x000, 0xfff, 0x633, 0x7aa, 0x748, 0x584, 0x327, 0xbc7,
	0x752, 0x430, 0x965, 0x444, 0x666, 0x9c8, 0x66b, 0x999
};

bool z_color_begin(const z_color_layout_t *l) {
	return z_color_set(true, l->planes, l->offsets);
}

void z_color_end(void) {
	if (z_color_available())
		z_color_set(false, 1, 0);
}

void z_color_palette_load_now(const uint16_t *rgb444, int n) {
	int i;
	if (n > 16) n = 16;
	for (i = 0; i < n; i++)
		z_color_set_palette((uint32_t)i, rgb444[i]);
}

void z_color_palette_load(const uint16_t *rgb444, int n) {
	if (!z_color_available()) return;
	z_game_wait_frame();
	z_color_palette_load_now(rgb444, n);
}

static int fold(int v, int m) {
	v %= m;
	return v < 0 ? v + m : v;
}

void z_color_plane_xy(const z_color_layout_t *l, int plane,
	int x, int y, int *px, int *py) {

	if (plane > 0 && plane < 4) {
		uint32_t o = ((uint32_t)l->offsets >> (2 * (plane - 1))) & 3u;
		if (o & Z_COLOR_DX) x += 320;
		if (o & Z_COLOR_DY) y += 240;
	}
	*px = fold(x, FB_W);
	*py = fold(y, FB_H);
}

/* -- the torus split --
 *
 * A rectangle at a folded origin can still run off the right or bottom
 * edge; on the torus it continues at column 0 or row 0. So it is up to
 * four pieces, each with the offset into the source that its first
 * pixel corresponds to. Everything below draws through this, so no
 * blit or fill is ever handed a rectangle past the framebuffer --
 * which matters, because the blitter only bounds-checks when asked to
 * and can hang on an out-of-range destination (zgfx.h). */
typedef struct {
	int x, y, w, h;     /* framebuffer rectangle */
	int ox, oy;         /* offset into the source */
} piece_t;

static int split(int x, int y, int w, int h, piece_t p[4]) {

	int xs[2], ws[2], oxs[2], nx = 1;
	int ys[2], hs[2], oys[2], ny = 1;
	int i, j, n = 0;

	if (w <= 0 || h <= 0) return 0;
	if (w > FB_W) w = FB_W;
	if (h > FB_H) h = FB_H;

	xs[0] = x; oxs[0] = 0;
	ws[0] = (x + w > FB_W) ? FB_W - x : w;
	if (ws[0] < w) {
		xs[1] = 0; oxs[1] = ws[0]; ws[1] = w - ws[0]; nx = 2;
	}

	ys[0] = y; oys[0] = 0;
	hs[0] = (y + h > FB_H) ? FB_H - y : h;
	if (hs[0] < h) {
		ys[1] = 0; oys[1] = hs[0]; hs[1] = h - hs[0]; ny = 2;
	}

	for (j = 0; j < ny; j++)
		for (i = 0; i < nx; i++) {
			p[n].x = xs[i]; p[n].w = ws[i]; p[n].ox = oxs[i];
			p[n].y = ys[j]; p[n].h = hs[j]; p[n].oy = oys[j];
			n++;
		}

	return n;
}

static int planes_of(const z_color_layout_t *l) {
	int n = l->planes;
	if (n < 1) n = 1;
	if (n > 4) n = 4;
	return n;
}

void z_color_fill_rect(const z_color_layout_t *l,
	int x, int y, int w, int h, int c) {

	piece_t p[4];
	int k, i, n, px, py;

	for (k = 0; k < planes_of(l); k++) {
		z_color_plane_xy(l, k, x, y, &px, &py);
		n = split(px, py, w, h, p);
		for (i = 0; i < n; i++)
			z_fb_hw_fill_rect(p[i].x, p[i].y, p[i].w, p[i].h, (c >> k) & 1);
	}
}

/* One source bitmap into one plane's pieces, with one raster op. */
bool z_color_plane_blit(const z_color_layout_t *l, int k,
	const void *src, int src_stride, int src_x, int src_y,
	int x, int y, int w, int h, int rop) {

	piece_t p[4];
	int i, n, px, py;

	z_color_plane_xy(l, k, x, y, &px, &py);
	n = split(px, py, w, h, p);
	for (i = 0; i < n; i++) {
		if (!z_fb_hw_blit_mem_rop(src, src_stride,
				src_x + p[i].ox, src_y + p[i].oy,
				p[i].x, p[i].y, p[i].w, p[i].h, rop))
			return false;
	}
	return true;
}

/* One plane's rectangle filled with a constant. */
void z_color_plane_fill(const z_color_layout_t *l, int k,
	int x, int y, int w, int h, int bit) {

	piece_t p[4];
	int i, n, px, py;

	z_color_plane_xy(l, k, x, y, &px, &py);
	n = split(px, py, w, h, p);
	for (i = 0; i < n; i++)
		z_fb_hw_fill_rect(p[i].x, p[i].y, p[i].w, p[i].h, bit);
}

bool z_color_blit_mono(const z_color_layout_t *l,
	const void *src, int src_stride, int src_x, int src_y,
	int x, int y, int w, int h, int c) {

	int k;

	if (!z_fb_hw_rop_available()) return false;

	/* Where the bitmap is set, each plane takes c's bit: OR to set it,
	 * ANDN to clear it. Where it is clear, both leave the plane
	 * alone -- which is what makes this transparent. */
	for (k = 0; k < planes_of(l); k++) {
		if (!z_color_plane_blit(l, k, src, src_stride, src_x, src_y, x, y, w, h,
				((c >> k) & 1) ? Z_ROP_OR : Z_ROP_ANDN))
			return false;
	}
	return true;
}

bool z_color_blit_planes(const z_color_layout_t *l,
	const void *const *src, int src_stride, int src_x, int src_y,
	int x, int y, int w, int h) {

	int k;

	for (k = 0; k < planes_of(l); k++) {
		if (!src[k]) {
			z_color_plane_fill(l, k, x, y, w, h, 0);
			continue;
		}
		if (!z_color_plane_blit(l, k, src[k], src_stride, src_x, src_y,
				x, y, w, h, Z_ROP_COPY))
			return false;
	}
	return true;
}

bool z_color_blit_sprite(const z_color_layout_t *l,
	const void *const *src, const void *mask, int src_stride,
	int src_x, int src_y, int x, int y, int w, int h) {

	int k;

	if (!z_fb_hw_rop_available()) return false;

	for (k = 0; k < planes_of(l); k++) {
		if (!z_color_plane_blit(l, k, mask, src_stride, src_x, src_y,
				x, y, w, h, Z_ROP_ANDN))
			return false;
		if (src[k] && !z_color_plane_blit(l, k, src[k], src_stride, src_x, src_y,
				x, y, w, h, Z_ROP_OR))
			return false;
	}
	return true;
}
