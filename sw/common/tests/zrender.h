#ifndef ZRENDER_H
#define ZRENDER_H

/*
 * zrender.h -- draw an app's panel on the build machine and write it
 * out as an image, so it can be LOOKED AT before it reaches a screen.
 *
 * Host-only. Include this from an app's tests/render.c, include the
 * app's panel source, set up whatever state you want to see, then
 * call z_render_open(), draw, and z_render_write().
 *
 * -- Why this exists --
 *
 * sw/apps/logic's panel was shipped wrong three times, and its
 * arithmetic test passed every time:
 *
 *   1. Window coordinates used where content coordinates were needed,
 *      so the bottom 15 rows were off the window. Diagnosed as
 *      spacing.
 *   2. Widgets inside the window but across the frames they were
 *      meant to be inside. Diagnosed as spacing again.
 *   3. z_win_hw_box()/z_win_hw_line() take ABSOLUTE SCREEN
 *      COORDINATES while everything else an app draws with is
 *      content-relative -- so every frame was drawn at the window's
 *      screen position. THIS was the actual bug behind 1 and 2.
 *
 * After each one the assertions were extended, and each time the next
 * mistake was of a kind the new assertion did not cover. That is not
 * bad luck, it is the shape of the technique: a geometry assertion
 * can only check a relationship somebody thought to write down.
 *
 * Rendering checks every relationship at once, including the ones
 * nobody anticipated. The third bug was found in one look.
 *
 * This does NOT replace an app's tests/test_layout.c. That runs
 * unattended and fails loudly on the relationships that ARE known.
 * This is the step before shipping: generate it and look at it.
 *
 * -- How it works --
 *
 * VRAM is a fixed address (sw/common/zgfx.c: 0x20000000), so mapping
 * real memory there lets the REAL zwin.c and zwidget.c run unmodified
 * -- which matters, because those own the content-rect inset and the
 * widget geometry that the bugs above lived in.
 *
 * WHAT IS NOT REAL is the pixel plotting. zgfx.c's box, line and fill
 * primitives all program the GPU rasterizer and blitter (rtl/gpu/*),
 * which on a build machine write to unmapped MMIO and draw nothing.
 * So this file provides software implementations of the dozen z_fb_*
 * entry points zwin.c and zwidget.c call, and zgfx.c is NOT linked.
 *
 * That split is deliberate and it is where the value is: pixel
 * plotting is not where layout bugs live. A wrongly DRAWN line is
 * obvious the moment you look; a wrongly PLACED one is not.
 *
 * It does mean this cannot catch a bug in zgfx.c itself, or one that
 * depends on the blitter's exact clipping. Worth knowing before
 * trusting a render over a real screen.
 *
 * -- Linking --
 *
 *   cc -std=gnu99 -Wall -I sw/common -o /tmp/render \
 *      sw/apps/<app>/tests/render.c \
 *      sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
 *      sw/common/zobj.c sw/common/zeitlos.c
 *
 * Note the absence of zgfx.c, and see above for why.
 *
 * maskirq() (sw/common/zeitlos.h) has a non-RISC-V branch so the
 * window layer compiles here at all.
 *
 * Linux and x86-64 only -- it needs MAP_FIXED_NOREPLACE at a low
 * address. Exits 77 elsewhere so a CI runner skips rather than fails.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/mman.h>

#include "zgfx.h"
#include "zwin.h"
#include "zwidget.h"
#include "zfont.h"
#include "zwm.h"

#define Z_RENDER_VRAM ((void *)0x20000000UL)
#define Z_RENDER_VRAM_LEN 0x40000u

// -- software pixel primitives ----------------------------------
//
// See this file's header on why these are here rather than linked
// from zgfx.c.

static z_clip_t zr_region[8];
static int zr_region_n;

void z_gfx_set_visible(const z_clip_t *r, int n) {
	if (n > 8) n = 8;
	if (n < 0) n = 0;
	for (int i = 0; i < n; i++) zr_region[i] = r[i];
	zr_region_n = n;
}

void z_gfx_clear_visible(void) { zr_region_n = 0; }
int z_gfx_visible_count(void) { return zr_region_n; }

static bool zr_allows(int x, int y, const z_clip_t *c) {
	if (x < 0 || y < 0 || x >= Z_SCREEN_W || y >= Z_SCREEN_H) return false;
	if (c && (x < c->x0 || x > c->x1 || y < c->y0 || y > c->y1)) return false;
	return true;
}

void z_fb_set_pixel(int x, int y, int color, const z_clip_t *c) {
	volatile uint32_t *vram = Z_RENDER_VRAM;
	uint32_t bit;
	if (!zr_allows(x, y, c)) return;
	bit = (uint32_t)y * Z_SCREEN_W + (uint32_t)x;
	if (color) vram[bit / 32] |= 1u << (bit % 32);
	else vram[bit / 32] &= ~(1u << (bit % 32));
}

void z_fb_fill_rect(int x, int y, int w, int h, int color,
	const z_clip_t *c) {
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			z_fb_set_pixel(x + i, y + j, color, c);
}

void z_fb_hw_fill_rect(int x, int y, int w, int h, int color) {
	z_fb_fill_rect(x, y, w, h, color, NULL);
}

// A dither on target; a light stipple here, so a shaded area is
// visibly distinct from a solid one rather than indistinguishable.
void z_fb_hw_fill_shade_async(int x, int y, int w, int h, int level) {
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			if (((x + i + y + j) & 3) < level)
				z_fb_set_pixel(x + i, y + j, 1, NULL);
}

void z_fb_hw_fill_pattern(int x, int y, int w, int h, const uint8_t *pat) {
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			z_fb_set_pixel(x + i, y + j,
				pat ? (pat[(y + j) & 7] >> ((x + i) & 7)) & 1 : 0, NULL);
}

void z_fb_hw_line(int x0, int y0, int x1, int y1, int color,
	const z_clip_t *c) {
	int dx = x1 > x0 ? x1 - x0 : x0 - x1;
	int dy = y1 > y0 ? y1 - y0 : y0 - y1;
	int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
	int err = (dx > dy ? dx : -dy) / 2, e2;
	for (;;) {
		z_fb_set_pixel(x0, y0, color, c);
		if (x0 == x1 && y0 == y1) break;
		e2 = err;
		if (e2 > -dx) { err -= dy; x0 += sx; }
		if (e2 < dy) { err += dx; y0 += sy; }
	}
}

void z_fb_hw_box(int x0, int y0, int x1, int y1, int color,
	const z_clip_t *c) {
	z_fb_hw_line(x0, y0, x1, y0, color, c);
	z_fb_hw_line(x0, y1, x1, y1, color, c);
	z_fb_hw_line(x0, y0, x0, y1, color, c);
	z_fb_hw_line(x1, y0, x1, y1, color, c);
}

void z_fb_draw_char(int x, int y, char ch, int color, const z_font_t *f,
	const z_clip_t *c) {
	const uint8_t *g;
	if (!f || (uint8_t)ch < f->first || (uint8_t)ch > f->last) return;
	g = f->glyphs + ((uint8_t)ch - f->first) * f->h;
	for (int j = 0; j < f->h; j++)
		for (int i = 0; i < f->w; i++)
			if (g[j] & (0x80 >> i)) z_fb_set_pixel(x + i, y + j, color, c);
}

void z_fb_draw_char2(int x, int y, char ch, int fg, int bg,
	const z_font_t *f, const z_clip_t *c) {
	const uint8_t *g;
	if (!f) return;
	z_fb_fill_rect(x, y, f->w, f->h, bg, c);
	if ((uint8_t)ch < f->first || (uint8_t)ch > f->last) return;
	g = f->glyphs + ((uint8_t)ch - f->first) * f->h;
	for (int j = 0; j < f->h; j++)
		for (int i = 0; i < f->w; i++)
			if (g[j] & (0x80 >> i)) z_fb_set_pixel(x + i, y + j, fg, c);
}

void z_fb_draw_text(int x, int y, const char *s, int color,
	const z_font_t *f, const z_clip_t *c) {
	for (; s && *s; s++, x += f->w) z_fb_draw_char(x, y, *s, color, f, c);
}

void z_fb_draw_text2(int x, int y, const char *s, int fg, int bg,
	const z_font_t *f, const z_clip_t *c) {
	for (; s && *s; s++, x += f->w) z_fb_draw_char2(x, y, *s, fg, bg, f, c);
}

// -- harness -----------------------------------------------------

// Map VRAM and set `win` up the way wm would, so the content rect the
// app gets is the real shape -- including the titlebar inset, which is
// what broke logic's first layout.
//
// Returns false if the address cannot be mapped; the caller should
// return 77 so CI skips.
static bool z_render_open(z_win_t *win, int win_w, int win_h) {

	void *m = mmap(Z_RENDER_VRAM, Z_RENDER_VRAM_LEN,
		PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

	if (m == MAP_FAILED) return false;

	memset(m, 0, Z_RENDER_VRAM_LEN);

	memset(win, 0, sizeof(*win));
	win->id = 1;
	win->x = 0;
	win->y = 0;
	win->w = win_w;
	win->h = win_h;

	return true;

}

// Write the content area as a PBM.
//
// Scaled up, because at 1x a 1bpp panel is too small on a modern
// display to judge and judging it is the whole point. P1 is ASCII and
// 1 = black, so it comes out ink-on-white the way the screen shows it.
//
// The content area is offset from the window origin by the same inset
// z_win_content_rect() applies, so what lands in the file is exactly
// what the app may draw in -- anything outside is a bug and will be
// missing rather than shown, which is itself the signal.
static void z_render_write(const char *path, const z_win_t *win, int scale) {

	volatile uint32_t *vram = Z_RENDER_VRAM;
	z_clip_t clip;
	FILE *f;
	int x, y, sx, sy, w, h;

	z_win_content_rect(win, &clip);
	w = clip.x1 - clip.x0 + 1;
	h = clip.y1 - clip.y0 + 1;

	f = fopen(path, "wb");
	if (!f) { perror(path); exit(1); }

	fprintf(f, "P1\n%d %d\n", w * scale, h * scale);

	for (y = 0; y < h; y++)
		for (sy = 0; sy < scale; sy++) {
			for (x = 0; x < w; x++) {
				uint32_t bit = (uint32_t)(clip.y0 + y) * Z_SCREEN_W
					+ (uint32_t)(clip.x0 + x);
				int on = (vram[bit / 32] >> (bit % 32)) & 1;
				for (sx = 0; sx < scale; sx++) fputc(on ? '1' : '0', f);
			}
			fputc('\n', f);
		}

	fclose(f);

	printf("render: wrote %s -- content %dx%d at %dx\n", path, w, h, scale);

}

#endif
