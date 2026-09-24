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
#include "zutf8.h"

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

bool z_gfx_visible_clip(int i, const z_clip_t *clip, z_clip_t *out) {
	z_clip_t e;
	if (i < 0 || i >= zr_region_n) return false;
	e = zr_region[i];
	if (clip) {
		if (e.x0 < clip->x0) e.x0 = clip->x0;
		if (e.y0 < clip->y0) e.y0 = clip->y0;
		if (e.x1 > clip->x1) e.x1 = clip->x1;
		if (e.y1 > clip->y1) e.y1 = clip->y1;
	}
	if (e.x1 < e.x0 || e.y1 < e.y0) return false;
	if (out) *out = e;
	return true;
}

// -- the paint session (zgfx.c's z_gfx_paint_*) -----------------
//
// The same walk without the hardware: one pass per visible-region
// rectangle intersected with the caller's clip, skipping the ones that
// do not intersect, and a single unrestricted pass when there is no
// region at all. A region of one EMPTY rectangle -- what a window that
// has not been told a region gets, see zwin.c's win_use_clip() --
// yields no passes, which is the point of it.
static int zr_paint_open, zr_paint_ri, zr_paint_have;
static z_clip_t zr_paint_eff;

void z_gfx_paint_begin(void) {
	zr_paint_open = 1;
	zr_paint_ri = -1;
	zr_paint_have = 0;
}

int z_gfx_paint_next_rect(const z_clip_t *clip) {

	if (!zr_paint_open) return 0;

	zr_paint_have = 0;

	if (zr_region_n == 0) {
		if (zr_paint_ri >= 0) return 0;
		zr_paint_ri = 0;
		if (clip) {
			zr_paint_eff = *clip;
		} else {
			zr_paint_eff.x0 = 0;
			zr_paint_eff.y0 = 0;
			zr_paint_eff.x1 = Z_SCREEN_W - 1;
			zr_paint_eff.y1 = Z_SCREEN_H - 1;
		}
		zr_paint_have = 1;
		return 1;
	}

	for (;;) {
		zr_paint_ri++;
		if (zr_paint_ri >= zr_region_n) return 0;
		if (z_gfx_visible_clip(zr_paint_ri, clip, &zr_paint_eff)) {
			zr_paint_have = 1;
			return 1;
		}
	}

}

int z_gfx_paint_current(z_clip_t *out) {
	if (!zr_paint_have) return 0;
	if (out) *out = zr_paint_eff;
	return 1;
}

int z_gfx_paint_active(void) { return zr_paint_have; }

void z_gfx_paint_end(void) {
	zr_paint_open = 0;
	zr_paint_ri = -1;
	zr_paint_have = 0;
}

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

// Blits a 1bpp source into the page, in framebuffer packing.
//
// Present so the off-device renderer can draw an in-place image
// exactly as the hardware blitter would -- which is how the layout
// tests can check that a supplied bitmap REPLACES the placeholder box
// rather than being drawn on top of it.
bool z_fb_hw_blit_mem(const void *src, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h) {

	const uint32_t *s = (const uint32_t *)src;
	int stride_w = src_stride / 4;		// BYTES on the way in, as the
	int x, y;							// hardware call takes them

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			int sx = src_x + x, sy = src_y + y;
			uint32_t word = s[sy * stride_w + (sx >> 5)];
			// LEAST significant bit leftmost, as zbm.h describes --
			// the opposite of font and icon data.
			int bit = (int)((word >> (sx & 31)) & 1);
			if (bit) z_fb_set_pixel(dst_x + x, dst_y + y, 1, NULL);
		}
	}

	return true;

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
	if (!f || z_font_index(f, (uint8_t)ch) < 0) return;
	g = f->glyphs + z_font_index(f, (uint8_t)ch) * f->h;
	for (int j = 0; j < f->h; j++)
		for (int i = 0; i < f->w; i++)
			if (g[j] & (0x80 >> i)) z_fb_set_pixel(x + i, y + j, color, c);
}

// Inside a paint session the effective rectangle is the session's, as
// on hardware where it is the scissor the caller programmed once for
// this pass. Outside one, the visible region is walked per glyph the
// way zgfx.c walks it. Either way a cell outside the region reaches
// nothing, which is what makes a region test in this harness mean
// something.
static bool zr_glyph_clip(const z_clip_t *in, int i, z_clip_t *out) {
	if (zr_paint_have) {
		*out = zr_paint_eff;
		if (in) {
			if (out->x0 < in->x0) out->x0 = in->x0;
			if (out->y0 < in->y0) out->y0 = in->y0;
			if (out->x1 > in->x1) out->x1 = in->x1;
			if (out->y1 > in->y1) out->y1 = in->y1;
		}
		return i == 0 && out->x1 >= out->x0 && out->y1 >= out->y0;
	}
	if (zr_region_n == 0) {
		if (i != 0) return false;
		if (in) *out = *in;
		else { out->x0 = 0; out->y0 = 0;
			out->x1 = Z_SCREEN_W - 1; out->y1 = Z_SCREEN_H - 1; }
		return true;
	}
	return z_gfx_visible_clip(i, in, out);
}

void z_fb_draw_char2(int x, int y, char ch, int fg, int bg,
	const z_font_t *f, const z_clip_t *c) {
	const uint8_t *g;
	z_clip_t e;
	if (!f) return;
	for (int ri = 0; zr_glyph_clip(c, ri, &e); ri++) {
		z_fb_fill_rect(x, y, f->w, f->h, bg, &e);
		if (z_font_index(f, (uint8_t)ch) < 0) continue;
		g = f->glyphs + z_font_index(f, (uint8_t)ch) * f->h;
		for (int j = 0; j < f->h; j++)
			for (int i = 0; i < f->w; i++)
				if (g[j] & (0x80 >> i)) z_fb_set_pixel(x + i, y + j, fg, &e);
	}
}

void z_fb_draw_text(int x, int y, const char *s, int color,
	const z_font_t *f, const z_clip_t *c) {
	for (; s && *s; s++, x += f->w) z_fb_draw_char(x, y, *s, color, f, c);
}

void z_fb_draw_text2(int x, int y, const char *s, int fg, int bg,
	const z_font_t *f, const z_clip_t *c) {
	for (; s && *s; s++, x += f->w) z_fb_draw_char2(x, y, *s, fg, bg, f, c);
}

// Codepoints and UTF-8, mapped to glyph bytes the way zgfx.c maps them
// (its cp_glyph_byte()): Latin-9 byte if the font has it, else the
// missing-glyph box, else '?'. C0 draws nothing and takes a cell.
static uint8_t zr_cp_byte(const z_font_t *f, uint32_t cp) {
	if (cp < 0x20) return (uint8_t)cp;
	uint8_t b = z_cp_to_l9(cp);
	if (b && z_font_index(f, b) >= 0) return b;
	return z_font_index(f, Z_GLYPH_MISSING) >= 0 ? Z_GLYPH_MISSING : '?';
}

// The Japanese font, handed over by a test (zgfx.h's z_jfont_use()).
#include "zjfont.h"
static const uint8_t *zr_jfont;
static uint32_t zr_jfont_len;
void z_jfont_use(const uint8_t *font, uint32_t len) { zr_jfont = font; zr_jfont_len = len; }

// A two-column character, as zgfx.c's draw_cp_wide(): the 12x12 glyph
// at 6x12 when the font has it, else the box and a blank.
static void zr_wide(int x, int y, uint32_t cp, int fg, int bg, bool solid,
	const z_font_t *f, const z_clip_t *c) {
	int gw, gh;
	const uint8_t *g = (f->h == 12 && f->w == 6) ?
		z_zfn_glyph(zr_jfont, zr_jfont_len, cp, &gw, &gh) : NULL;
	if (g) {
		if (solid) z_fb_fill_rect(x, y, 2 * f->w, f->h, bg, c);
		for (int r = 0; r < gh; r++) {
			uint16_t bits = (uint16_t)((g[2 * r] << 8) | g[2 * r + 1]);
			for (int i = 0; i < gw; i++)
				if (bits & (0x8000u >> i)) z_fb_set_pixel(x + i, y + r, fg, c);
		}
		return;
	}
	if (solid) {
		z_fb_draw_char2(x, y, (char)zr_cp_byte(f, 0xFFFD), fg, bg, f, c);
		z_fb_draw_char2(x + f->w, y, ' ', fg, bg, f, c);
	} else {
		z_fb_draw_char(x, y, (char)zr_cp_byte(f, 0xFFFD), fg, f, c);
	}
}

void z_fb_draw_cp(int x, int y, uint32_t cp, int color,
	const z_font_t *f, const z_clip_t *c) {
	if (z_cp_width(cp) == 2) { zr_wide(x, y, cp, color, 0, false, f, c); return; }
	z_fb_draw_char(x, y, (char)zr_cp_byte(f, cp), color, f, c);
}

void z_fb_draw_cp2(int x, int y, uint32_t cp, int fg, int bg,
	const z_font_t *f, const z_clip_t *c) {
	if (z_cp_width(cp) == 2) { zr_wide(x, y, cp, fg, bg, true, f, c); return; }
	z_fb_draw_char2(x, y, (char)zr_cp_byte(f, cp), fg, bg, f, c);
}

// Columns follow z_cp_width(), as in zgfx.c.
static void zr_utf8(int x, int y, const char *s, int fg, int bg, bool solid,
	const z_font_t *f, const z_clip_t *c) {
	const char *end = s + strlen(s);
	int cx = x;
	while (s < end) {
		uint32_t cp = z_utf8_next(&s, end);
		if (cp == '\n') { cx = x; y += f->h; continue; }
		int w = z_cp_width(cp);
		if (w == 0) continue;
		if (solid) z_fb_draw_cp2(cx, y, cp, fg, bg, f, c);
		else z_fb_draw_cp(cx, y, cp, fg, f, c);
		cx += w * f->w;
	}
}

void z_fb_draw_utf8(int x, int y, const char *s, int color,
	const z_font_t *f, const z_clip_t *c) {
	zr_utf8(x, y, s, color, 0, false, f, c);
}

void z_fb_draw_utf8_2(int x, int y, const char *s, int fg, int bg,
	const z_font_t *f, const z_clip_t *c) {
	zr_utf8(x, y, s, fg, bg, true, f, c);
}

// Software VRAM scroll, with the same contract as zgfx.c's: content
// moves by dy (negative = up), the strip that scrolls in is NOT
// touched, and a partially occluded window is refused. Refusal is
// decided by the same rule so a render test exercises the caller's
// fallback path exactly when the hardware would.
bool z_fb_hw_scroll_allowed(int x, int y, int w, int h) {
	z_clip_t r;
	if (zr_region_n == 0) return true;
	if (zr_region_n != 1 || w <= 0 || h <= 0) return false;
	r = zr_region[0];
	return x >= r.x0 && y >= r.y0 && x + w - 1 <= r.x1 && y + h - 1 <= r.y1;
}

static int z_render_get(int x, int y);

void z_fb_hw_scroll(int x, int y, int w, int h, int dy) {
	if (!z_fb_hw_scroll_allowed(x, y, w, h)) return;
	if (dy == 0 || dy <= -h || dy >= h) return;
	if (dy < 0) {
		for (int j = 0; j < h + dy; j++)
			for (int i = 0; i < w; i++)
				z_fb_set_pixel(x + i, y + j,
					z_render_get(x + i, y + j - dy), NULL);
	} else {
		for (int j = h - 1; j >= dy; j--)
			for (int i = 0; i < w; i++)
				z_fb_set_pixel(x + i, y + j,
					z_render_get(x + i, y + j - dy), NULL);
	}
}

// -- harness -----------------------------------------------------

// Map VRAM and set `win` up the way wm would, so the content rect the
// app gets is the real shape -- including the titlebar inset, which is
// what broke logic's first layout.
//
// Returns false if the address cannot be mapped; the caller should
// return 77 so CI skips.
// Reads and clears the shim's framebuffer.
//
// For tests that need to assert what was DRAWN rather than only that
// drawing did not crash -- checking, for instance, that an in-place
// image replaces its placeholder frame instead of being drawn inside
// it.
static int z_render_get(int x, int y) {
	volatile uint32_t *vram = Z_RENDER_VRAM;
	uint32_t bit;
	if (x < 0 || y < 0 || x >= Z_SCREEN_W || y >= Z_SCREEN_H) return 0;
	bit = (uint32_t)y * Z_SCREEN_W + (uint32_t)x;
	return (int)((vram[bit / 32] >> (bit % 32)) & 1);
}

static void z_render_clear(void) {
	volatile uint32_t *vram = Z_RENDER_VRAM;
	for (long i = 0; i < ((long)Z_SCREEN_W * Z_SCREEN_H) / 32; i++)
		vram[i] = 0;
}

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

	// The region wm sends behind Z_WM_WINDOW_CREATED. Not optional: a
	// windowed process is born invisible and draws nothing until its
	// first SET_CLIP arrives (zwin.c's win_use_clip()), so a window
	// left with clip_n == 0 renders an empty page here.
	win->clip[0].x0 = win->x;
	win->clip[0].y0 = win->y;
	win->clip[0].x1 = win->x + win_w - 1;
	win->clip[0].y1 = win->y + win_h - 1;
	win->clip_n = 1;

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
