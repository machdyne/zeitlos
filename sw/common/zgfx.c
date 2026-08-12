/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Direct-framebuffer pixel/text drawing. See zgfx.h.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zgfx.h"
#include "zfont.h"

#define VRAM ((volatile uint32_t *)0x20000000)

static inline bool clip_allows(int x, int y, const z_clip_t *clip) {

	if (x < 0 || x >= Z_SCREEN_W || y < 0 || y >= Z_SCREEN_H)
		return false;

	if (clip) {
		if (x < clip->x0 || x > clip->x1 || y < clip->y0 || y > clip->y1)
			return false;
	}

	return true;

}

void z_fb_set_pixel(int x, int y, int color, const z_clip_t *clip) {

	if (!clip_allows(x, y, clip)) return;

	uint32_t bit_index = (uint32_t)y * Z_SCREEN_W + (uint32_t)x;
	uint32_t word_index = bit_index / 32;
	uint32_t mask = 1U << (bit_index % 32);

	if (color)
		VRAM[word_index] |= mask;
	else
		VRAM[word_index] &= ~mask;

}

void z_fb_fill_rect(int x, int y, int w, int h, int color, const z_clip_t *clip) {

	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			z_fb_set_pixel(x + i, y + j, color, clip);

}

void z_fb_draw_char(int x, int y, char c, int color, const z_font_t *font, const z_clip_t *clip) {

	unsigned char uc = (unsigned char)c;
	if (uc < font->first || uc > font->last) return;

	const uint8_t *glyph = font->glyphs + (uc - font->first) * font->h;

	for (int row = 0; row < font->h; row++) {
		uint8_t bits = glyph[row];
		for (int col = 0; col < font->w; col++) {
			if (bits & (0x80 >> col))
				z_fb_set_pixel(x + col, y + row, color, clip);
		}
	}

}

void z_fb_draw_text(int x, int y, const char *s, int color, const z_font_t *font, const z_clip_t *clip) {

	int cx = x, cy = y;

	for (; *s; s++) {
		if (*s == '\n') {
			cx = x;
			cy += font->h;
			continue;
		}
		z_fb_draw_char(cx, cy, *s, color, font, clip);
		cx += font->w;
	}

}
