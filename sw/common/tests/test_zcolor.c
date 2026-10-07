/*
 * sw/common/zcolor.c on the host (docs/color.md).
 *
 * The blitter calls zcolor makes are replaced by a software model of
 * them over a 640x480x1bpp array, with the documented bit order and
 * raster ops, and with every call checked to lie inside the
 * framebuffer -- the real blitter can hang on one that does not. Then
 * every drawing call is checked by reading the colour index back the
 * way the scanout hardware forms it: one bit from each plane, at that
 * plane's offset, folded onto the torus.
 *
 * Covered: every layout; rectangles inside, across the right seam,
 * across the bottom seam and across both; negative and >640 origins;
 * fill, transparent mono blit, opaque plane blit (with a NULL plane)
 * and masked sprite; and that nothing outside the rectangle changes.
 *
 *   make -f Makefile.zcolor
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../zcolor.h"
#include "../zgfx.h"

#define FB_W 640
#define FB_H 480

static uint8_t fb[FB_H][FB_W];
static int errors;
static int calls_out_of_range;

static void check_rect(const char *what, int x, int y, int w, int h) {
	if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > FB_W || y + h > FB_H) {
		if (calls_out_of_range < 5)
			printf("FAIL: %s out of range: %d,%d %dx%d\n", what, x, y, w, h);
		calls_out_of_range++;
	}
}

/* -- the blitter, in software -- */

void z_fb_hw_fill_rect(int x, int y, int w, int h, int color) {
	int i, j;
	check_rect("fill", x, y, w, h);
	for (j = y; j < y + h; j++)
		for (i = x; i < x + w; i++)
			if (i >= 0 && i < FB_W && j >= 0 && j < FB_H)
				fb[j][i] = (uint8_t)(color & 1);
}

static int src_bit(const void *src, int stride, int x, int y) {
	const uint32_t *row = (const uint32_t *)((const uint8_t *)src + y * stride);
	return (int)((row[x >> 5] >> (x & 31)) & 1u);
}

bool z_fb_hw_blit_mem_rop(const void *src, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h, int rop) {
	int i, j;
	check_rect("blit", dst_x, dst_y, w, h);
	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			int dx = dst_x + i, dy = dst_y + j;
			int s = src_bit(src, src_stride, src_x + i, src_y + j);
			uint8_t *d;
			if (dx < 0 || dx >= FB_W || dy < 0 || dy >= FB_H) continue;
			d = &fb[dy][dx];
			switch (rop) {
				case Z_ROP_COPY: *d = (uint8_t)s; break;
				case Z_ROP_OR:   *d = (uint8_t)(*d | s); break;
				case Z_ROP_XOR:  *d = (uint8_t)(*d ^ s); break;
				case Z_ROP_ANDN: *d = (uint8_t)(*d & !s); break;
			}
		}
	return true;
}

bool z_fb_hw_rop_available(void) { return true; }

/* -- the scanout, in software -- */

static int fold(int v, int m) { v %= m; return v < 0 ? v + m : v; }

static int index_at(const z_color_layout_t *l, int x, int y) {
	int k, c = 0;
	for (k = 0; k < l->planes; k++) {
		int px, py;
		z_color_plane_xy(l, k, x, y, &px, &py);
		c |= fb[py][px] << k;
	}
	return c;
}

/* -- test data -- */

#define SW 96
#define SH 40
#define SSTRIDE (SW / 8 + 4)

static uint32_t srcbuf[4][SH * SSTRIDE / 4];
static uint32_t maskbuf[SH * SSTRIDE / 4];

static void set_src(uint32_t *buf, int x, int y, int v) {
	uint32_t *row = buf + y * (SSTRIDE / 4);
	if (v) row[x >> 5] |= 1u << (x & 31);
	else   row[x >> 5] &= ~(1u << (x & 31));
}

static int get_src(const uint32_t *buf, int x, int y) {
	return src_bit(buf, SSTRIDE, x, y);
}

/* Fill the framebuffer with noise, remember it, and afterwards compare
 * everything outside the rectangle against what was there. */
static uint8_t before[FB_H][FB_W];

static void noise(unsigned seed) {
	int x, y;
	srand(seed);
	for (y = 0; y < FB_H; y++)
		for (x = 0; x < FB_W; x++)
			fb[y][x] = (uint8_t)(rand() & 1);
	memcpy(before, fb, sizeof(fb));
}

/* Which framebuffer pixels belong to the rectangle in ANY active plane */
static uint8_t touched[FB_H][FB_W];

static void mark(const z_color_layout_t *l, int x, int y, int w, int h) {
	int k, i, j;
	memset(touched, 0, sizeof(touched));
	for (k = 0; k < l->planes; k++)
		for (j = 0; j < h; j++)
			for (i = 0; i < w; i++) {
				int px, py;
				z_color_plane_xy(l, k, x + i, y + j, &px, &py);
				touched[py][px] = 1;
			}
}

static void check_untouched(const char *what) {
	int x, y, bad = 0;
	for (y = 0; y < FB_H; y++)
		for (x = 0; x < FB_W; x++)
			if (!touched[y][x] && fb[y][x] != before[y][x]) bad++;
	if (bad) {
		printf("FAIL: %s: %d pixels outside the rectangle changed\n", what, bad);
		errors++;
	}
}

static const z_color_layout_t *layouts[] = {
	&z_color_16, &z_color_8, &z_color_4_right, &z_color_4_below, &z_color_mono
};
static const char *layout_names[] = { "16", "8", "4-right", "4-below", "mono" };

/* origins: inside, across the right seam, across the bottom seam,
 * across both, negative, beyond 640/480 */
static const int origins[][2] = {
	{ 10, 20 }, { 600, 30 }, { 50, 460 }, { 620, 470 }, { -30, -7 },
	{ 1300, 1000 }, { 317, 237 }, { 0, 0 }
};

int main(void) {

	unsigned li, oi;
	int x, y, k, c;

	/* the plane offsets */
	{
		int px, py;
		z_color_plane_xy(&z_color_16, 3, 400, 300, &px, &py);
		if (px != 80 || py != 60) {
			printf("FAIL: plane 3 of (400,300): (%d,%d)\n", px, py);
			errors++;
		}
		z_color_plane_xy(&z_color_4_below, 1, -1, 0, &px, &py);
		if (px != 639 || py != 240) {
			printf("FAIL: plane 1 of (-1,0) below: (%d,%d)\n", px, py);
			errors++;
		}
	}

	/* source bitmaps */
	srand(7);
	for (k = 0; k < 4; k++)
		for (y = 0; y < SH; y++)
			for (x = 0; x < SW; x++)
				set_src(srcbuf[k], x, y, rand() & 1);
	for (y = 0; y < SH; y++)
		for (x = 0; x < SW; x++) {
			int m = (rand() % 3) != 0;
			set_src(maskbuf, x, y, m);
			if (!m) for (k = 0; k < 4; k++) set_src(srcbuf[k], x, y, 0);
		}

	for (li = 0; li < sizeof(layouts) / sizeof(layouts[0]); li++) {
		const z_color_layout_t *l = layouts[li];
		int maskc = (1 << l->planes) - 1;

		for (oi = 0; oi < sizeof(origins) / sizeof(origins[0]); oi++) {
			int ox = origins[oi][0], oy = origins[oi][1];
			int w = 70, h = 33, bad;
			char what[96];

			/* fill */
			for (c = 0; c < 16; c += 5) {
				noise(li * 100 + oi * 10 + (unsigned)c);
				mark(l, ox, oy, w, h);
				z_color_fill_rect(l, ox, oy, w, h, c);
				bad = 0;
				for (y = 0; y < h; y++)
					for (x = 0; x < w; x++)
						if (index_at(l, ox + x, oy + y) != (c & maskc)) bad++;
				snprintf(what, sizeof(what), "fill %s at (%d,%d) colour %d",
					layout_names[li], ox, oy, c);
				if (bad) { printf("FAIL: %s: %d pixels\n", what, bad); errors++; }
				check_untouched(what);
			}

			/* transparent mono blit, odd source offset */
			noise(li * 7 + oi);
			mark(l, ox, oy, w, h);
			z_color_blit_mono(l, srcbuf[0], SSTRIDE, 5, 3, ox, oy, w, h, 9);
			bad = 0;
			for (y = 0; y < h; y++)
				for (x = 0; x < w; x++) {
					int got = index_at(l, ox + x, oy + y);
					int exp;
					if (get_src(srcbuf[0], 5 + x, 3 + y)) exp = 9 & maskc;
					else {
						int kk; exp = 0;
						for (kk = 0; kk < l->planes; kk++) {
							int px, py;
							z_color_plane_xy(l, kk, ox + x, oy + y, &px, &py);
							exp |= before[py][px] << kk;
						}
					}
					if (got != exp) bad++;
				}
			snprintf(what, sizeof(what), "mono blit %s at (%d,%d)",
				layout_names[li], ox, oy);
			if (bad) { printf("FAIL: %s: %d pixels\n", what, bad); errors++; }
			check_untouched(what);

			/* opaque planes, plane 1 NULL */
			{
				const void *src[4] = { srcbuf[0], NULL, srcbuf[2], srcbuf[3] };
				noise(li * 13 + oi);
				mark(l, ox, oy, w, h);
				z_color_blit_planes(l, src, SSTRIDE, 11, 2, ox, oy, w, h);
				bad = 0;
				for (y = 0; y < h; y++)
					for (x = 0; x < w; x++) {
						int exp = 0;
						for (k = 0; k < l->planes; k++)
							if (src[k]) exp |= get_src(src[k], 11 + x, 2 + y) << k;
						if (index_at(l, ox + x, oy + y) != exp) bad++;
					}
				snprintf(what, sizeof(what), "plane blit %s at (%d,%d)",
					layout_names[li], ox, oy);
				if (bad) { printf("FAIL: %s: %d pixels\n", what, bad); errors++; }
				check_untouched(what);
			}

			/* masked sprite */
			{
				const void *src[4] = { srcbuf[0], srcbuf[1], srcbuf[2], srcbuf[3] };
				noise(li * 17 + oi);
				mark(l, ox, oy, w, h);
				z_color_blit_sprite(l, src, maskbuf, SSTRIDE, 1, 1, ox, oy, w, h);
				bad = 0;
				for (y = 0; y < h; y++)
					for (x = 0; x < w; x++) {
						int exp = 0;
						if (get_src(maskbuf, 1 + x, 1 + y)) {
							for (k = 0; k < l->planes; k++)
								exp |= get_src(srcbuf[k], 1 + x, 1 + y) << k;
						} else {
							for (k = 0; k < l->planes; k++) {
								int px, py;
								z_color_plane_xy(l, k, ox + x, oy + y, &px, &py);
								exp |= before[py][px] << k;
							}
						}
						if (index_at(l, ox + x, oy + y) != exp) bad++;
					}
				snprintf(what, sizeof(what), "sprite %s at (%d,%d)",
					layout_names[li], ox, oy);
				if (bad) { printf("FAIL: %s: %d pixels\n", what, bad); errors++; }
				check_untouched(what);
			}
		}
	}

	(void)fold;

	if (calls_out_of_range) {
		printf("FAIL: %d blitter calls outside the framebuffer\n", calls_out_of_range);
		errors++;
	}

	if (errors) {
		printf("RESULT: FAIL (%d)\n", errors);
		return 1;
	}
	printf("all zcolor tests passed\n");
	return 0;
}
