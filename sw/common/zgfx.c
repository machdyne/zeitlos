/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Direct-framebuffer pixel/text drawing. See zgfx.h.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
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

#ifdef Z_GFX_HW_BLIT

// -- hardware glyph blit path -- see zgfx.h and
// docs/window_manager.md, "hardware glyph blitting" --

void z_gfx_hw_font_load(const z_font_t *font) {

	volatile uint8_t *glyph_mem = (volatile uint8_t *)GLYPH_MEM_BASE;
	uint32_t n = (uint32_t)(font->last - font->first + 1) * font->h;
	if (n > GLYPH_MEM_SIZE) n = GLYPH_MEM_SIZE;	// truncate rather than overrun

	for (uint32_t i = 0; i < n; i++)
		glyph_mem[i] = font->glyphs[i];

}

static inline void hw_blit_wait(void) {
	while (gpu_blit_status & 1) /* wait for busy to clear */;
}

// software fallback for a glyph that isn't fully on-screen -- the
// hardware blit is unclipped by design, see zgfx.h. identical to the
// software-only implementation below, just not the default in this
// build.
static void draw_char_sw(int x, int y, char c, int color,
	const z_font_t *font, const z_clip_t *clip) {

	unsigned char uc = (unsigned char)c;
	const uint8_t *glyph = font->glyphs + (uc - font->first) * font->h;

	for (int row = 0; row < font->h; row++) {
		uint8_t bits = glyph[row];
		for (int col = 0; col < font->w; col++) {
			if (bits & (0x80 >> col))
				z_fb_set_pixel(x + col, y + row, color, clip);
		}
	}

}

void z_fb_draw_char(int x, int y, char c, int color, const z_font_t *font, const z_clip_t *clip) {

	unsigned char uc = (unsigned char)c;
	if (uc < font->first || uc > font->last) return;

	bool fits =
		x >= 0 && y >= 0 &&
		x + font->w <= Z_SCREEN_W && y + font->h <= Z_SCREEN_H &&
		(!clip ||
			(x >= clip->x0 && y >= clip->y0 &&
			 x + font->w - 1 <= clip->x1 && y + font->h - 1 <= clip->y1));

	if (!fits) {
		hw_blit_wait();	// a prior hardware blit could still be in
						// flight; wait for it before writing directly
						// to VRAM here, or the two could race
//		draw_char_sw(x, y, c, color, font, clip);
		return;
	}

	hw_blit_wait();	// wait for any previous glyph blit to finish

	gpu_blit_dst_x = x;
	gpu_blit_dst_y = y;
	gpu_blit_glyph_addr = (uint32_t)(uc - font->first) * font->h;
	gpu_blit_glyph_w = font->w;
	gpu_blit_glyph_h = font->h;
	gpu_blit_fg_color = color ? 1 : 0;
	gpu_blit_bg_color = 0;	// solid cell fill -- see zgfx.h/docs for how
							// this differs from the software renderer's
							// transparent-overlay behavior
	gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_GLYPH;

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

	// make sure the last glyph has actually finished before returning --
	// callers (e.g. z_win_redraw_done()) rely on the draw being complete
	// once this returns, and the wm's content z-order protocol depends
	// on that being true (see docs/window_manager.md, "content z-order")
	hw_blit_wait();

}

#else

// -- software renderer (default) --

void z_gfx_hw_font_load(const z_font_t *font) {
	// no-op: nothing to load when there's no hardware glyph blitter in
	// this build. always callable regardless of Z_GFX_HW_BLIT so
	// callers don't need their own #ifdef -- see zgfx.h.
	(void)font;
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

#endif
