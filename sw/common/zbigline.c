/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Big text: a line drawn in memory and copied to the screen. See
 * zbigline.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zbigline.h"
#include "zutf8.h"

// The line, laid out like the framebuffer: row-major, 32 pixels a word,
// least significant bit leftmost. One word more than the widest line:
// the blitter may read a word past the last one it needs (zgfx.h).
#define WORDS	(Z_BIGLINE_MAX_W / 32 + 1)

static uint32_t line[Z_BIGLINE_MAX_H][WORDS];
static int line_w, line_h;

bool z_bigline_available(void) {
	return z_fb_hw_blit_mem_available();
}

void z_bigline_begin(int w, int h) {
	if (w > Z_BIGLINE_MAX_W) w = Z_BIGLINE_MAX_W;
	if (h > Z_BIGLINE_MAX_H) h = Z_BIGLINE_MAX_H;
	if (w < 0) w = 0;
	if (h < 0) h = 0;
	line_w = w;
	line_h = h;
	int words = (w + 31) / 32;
	for (int y = 0; y < h; y++)
		memset(line[y], 0, (size_t)words * 4u);
}

// The glyph for `cp`, or NULL. Binary search: the table is ascending.
static const uint16_t *glyph_of(const z_bigfont_t *f, uint32_t cp) {
	int lo = 0, hi = f->count - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		uint32_t c = f->cps[mid];
		if (c == cp) return &f->rows[(uint32_t)mid * f->h];
		if (c < cp) lo = mid + 1;
		else hi = mid - 1;
	}
	return NULL;
}

// ORs `bits` (framebuffer order, at most 16 wide) into row `y` at
// column `x`. Bits past the line's width are dropped.
static void put_bits(int y, int x, uint32_t bits, int w) {
	if (x >= line_w || y < 0 || y >= line_h || !bits) return;
	if (x + w > line_w) bits &= (1u << (line_w - x)) - 1u;
	int wi = x >> 5, sh = x & 31;
	line[y][wi] |= bits << sh;
	if (sh + w > 32) line[y][wi + 1] |= bits >> (32 - sh);
}

// The box a character with no glyph is drawn as: `w` wide, inset a
// pixel, between the font's cap height and baseline roughly.
static void put_box(int x, int w, int h) {
	int top = h / 5, bot = h - h / 5 - 1;
	if (w < 4 || bot <= top) return;
	uint32_t edge = ((1u << (w - 2)) - 1u) << 1;
	uint32_t sides = (1u << 1) | (1u << (w - 2));
	for (int y = top; y <= bot; y++)
		put_bits(y, x, (y == top || y == bot) ? edge : sides, w);
}

int z_bigline_text(int x, const char *s, int n, const z_bigfont_t *f) {

	const char *p = s, *end = s + n;

	while (p < end && x < line_w) {

		const char *before = p;
		uint32_t cp = z_utf8_next(&p, end);
		bool malformed = (p - before == 1 && (uint8_t)before[0] >= 0x80);
		int cells = malformed ? 1 : z_cp_width(cp);

		// As text counts them (text.c, width_at()): a control character
		// is one cell, drawn as a space; a combining mark takes none and
		// is not drawn.
		if (cp < 0x20 || cp == 0x7F) { x += f->w; continue; }
		if (cells == 0) continue;

		int cw = cells * f->w;
		const uint16_t *g = malformed ? NULL : glyph_of(f, cp);
		if (!g && !malformed) {
			uint8_t alt = z_cp_lookalike(cp);
			if (alt) g = glyph_of(f, alt);
		}

		if (g && cells == 1) {
			for (int y = 0; y < f->h && y < line_h; y++)
				put_bits(y, x, g[y], f->w);
		} else {
			put_box(x, cw, f->h < line_h ? f->h : line_h);
		}

		x += cw;
	}

	return x;

}

void z_bigline_invert(int x0, int x1) {
	if (x0 < 0) x0 = 0;
	if (x1 > line_w) x1 = line_w;
	if (x1 <= x0) return;
	for (int wi = x0 >> 5; wi <= (x1 - 1) >> 5; wi++) {
		int a = wi * 32, b = a + 32;
		uint32_t m = 0xFFFFFFFFu;
		if (x0 > a) m &= 0xFFFFFFFFu << (x0 - a);
		if (x1 < b) m &= 0xFFFFFFFFu >> (b - x1);
		for (int y = 0; y < line_h; y++) line[y][wi] ^= m;
	}
}

bool z_bigline_put(int sx, int sy, const z_clip_t *clip) {

	int src_x = 0, src_y = 0, w = line_w, h = line_h;

	// The blitter clips to the screen and the visible region, not to a
	// rectangle of the window's: the text area is narrowed here, the
	// source moving in step.
	if (sx < clip->x0) { int d = clip->x0 - sx; sx += d; src_x += d; w -= d; }
	if (sy < clip->y0) { int d = clip->y0 - sy; sy += d; src_y += d; h -= d; }
	if (sx + w - 1 > clip->x1) w = clip->x1 - sx + 1;
	if (sy + h - 1 > clip->y1) h = clip->y1 - sy + 1;
	if (w <= 0 || h <= 0) return true;

	return z_fb_hw_blit_mem(line, WORDS * 4, src_x, src_y, sx, sy, w, h);

}
