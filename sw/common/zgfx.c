/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Direct-framebuffer pixel/text drawing. See zgfx.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

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

// -- hardware line rasterizer path (rtl/gpu/gpu_raster.v) -- see
// zgfx.h's file header comment and z_fb_hw_line()'s own comment for
// why this needed IRQ masking to be safe for concurrent access from
// multiple processes.

static inline void gpu_wait_fifo(void) {
	// bounded defensively -- see z_fb_hw_line()'s own comment on why
	// the rasterizer's hardware state machine can hang forever with
	// no protection of its own if it's ever handed a bad coordinate.
	// The coordinate clamp there is the real fix; this is a backstop
	// in case some other, not-yet-understood path can still stall
	// it -- an arbitrary-but-generous bound, well past anything a
	// genuinely draining FIFO should ever take, so this essentially
	// never fires under normal operation. If it does fire, that's
	// itself important diagnostic information (the rasterizer really
	// is stuck, not just slow), so it's reported, not silently
	// swallowed -- better for the calling process to stay responsive
	// and report the problem than to freeze forever the way it did
	// before this existed.
	uint32_t waited = 0;
	while (gpu_debug_fifo_count > 15) {
		if (++waited > 10000000) {
			printf("zgfx: gpu fifo wait timed out -- rasterizer may be stuck\n");
			return;
		}
	}
}

void z_fb_hw_line(int x0, int y0, int x1, int y1, int color, const z_clip_t *clip) {

	// unconditional, regardless of clip -- clip only ever narrows
	// where on screen a line can land, it was never a bounds check
	// against the framebuffer's actual physical size. The hardware
	// has no protection of its own here: gpu_y0/gpu_y1 are 10-bit
	// registers (0-1023), but the framebuffer is only Z_SCREEN_H=480
	// pixels tall, so 480-1023 is representable but physically
	// invalid -- and rtl/gpu/gpu_raster.v's WAIT_READ/WAIT_WRITE
	// states wait on the framebuffer bus's ack with no timeout at
	// all (only the pixel *count* along a line is bounded, not how
	// long any single pixel's bus transaction is allowed to take).
	// A coordinate that lands somewhere the bus never acks hangs the
	// rasterizer's state machine forever: draw_busy never clears,
	// the FIFO never drains, and gpu_wait_fifo() (below, and in every
	// future call) spins forever waiting for room that will never
	// free up. Clamping here, at the one shared entry point every
	// caller goes through, closes this regardless of where a bad
	// coordinate would otherwise have come from.
	if (x0 < 0) x0 = 0;
	if (x0 >= Z_SCREEN_W) x0 = Z_SCREEN_W - 1;
	if (x1 < 0) x1 = 0;
	if (x1 >= Z_SCREEN_W) x1 = Z_SCREEN_W - 1;
	if (y0 < 0) y0 = 0;
	if (y0 >= Z_SCREEN_H) y0 = Z_SCREEN_H - 1;
	if (y1 < 0) y1 = 0;
	if (y1 >= Z_SCREEN_H) y1 = Z_SCREEN_H - 1;

	// deliberately outside the masked section below -- this can
	// legitimately take a while if the FIFO's backed up, and masking
	// through that would stall the scheduler for every other process,
	// not just this one.
	gpu_wait_fifo();

	uint32_t old_mask = maskirq(0xFFFFFFFF);

	if (clip) {
		gpu_clip_x0 = (uint32_t)clip->x0;
		gpu_clip_y0 = (uint32_t)clip->y0;
		gpu_clip_x1 = (uint32_t)clip->x1;
		gpu_clip_y1 = (uint32_t)clip->y1;
		gpu_clip_enable = 1;
	} else {
		gpu_clip_enable = 0;
	}

	gpu_x0 = (uint32_t)x0;
	gpu_y0 = (uint32_t)y0;
	gpu_x1 = (uint32_t)x1;
	gpu_y1 = (uint32_t)y1;
	gpu_color = color & 1;
	gpu_start = 1;

	maskirq(old_mask);

}

void z_fb_hw_box(int x0, int y0, int x1, int y1, int color, const z_clip_t *clip) {
	z_fb_hw_line(x0, y0, x1, y0, color, clip);	// top
	z_fb_hw_line(x1, y0, x1, y1, color, clip);	// right
	z_fb_hw_line(x1, y1, x0, y1, color, clip);	// bottom
	z_fb_hw_line(x0, y1, x0, y0, color, clip);	// left
}

// -- GPU blitter fill path (rtl/gpu/gpu_blit.v) -- see zgfx.h's
// z_fb_hw_fill_rect() comment for why this needed the same
// clamp+mask treatment as z_fb_hw_line() above. Declared here,
// unconditionally (not inside the Z_GFX_HW_BLIT block below), since
// fill mode is independent of whether this build also drives the
// hardware glyph path -- the blitter itself is always present in the
// register map regardless of that build flag.

static inline void gpu_blit_wait_idle(void) {
	// bounded defensively, same reasoning and same pattern as
	// gpu_wait_fifo() above -- an out-of-range destination could hang
	// this state machine forever with no protection of its own; the
	// coordinate clamp in z_fb_hw_fill_rect() is the real fix, this
	// is a backstop in case some other path can still stall it.
	uint32_t waited = 0;
	while (gpu_blit_status & 1) {
		if (++waited > 10000000) {
			printf("zgfx: gpu blitter wait timed out -- blitter may be stuck\n");
			return;
		}
	}
}

void z_fb_hw_fill_rect(int x, int y, int w, int h, int color) {

	// clamp to actual screen bounds -- unconditional, regardless of
	// CTRL_CLIP, for the same reason z_fb_hw_line() clamps
	// unconditionally. See this function's own declaration in zgfx.h
	// for the full reasoning.
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > Z_SCREEN_W) w = Z_SCREEN_W - x;
	if (y + h > Z_SCREEN_H) h = Z_SCREEN_H - y;
	if (w <= 0 || h <= 0) return;	// nothing left to draw

	// wait for any prior blitter operation to finish -- including one
	// from the hardware glyph path (z_fb_draw_char(), below, shares
	// this same register set) or another process -- before touching
	// its registers ourselves.
	gpu_blit_wait_idle();

	uint32_t old_mask = maskirq(0xFFFFFFFF);

	gpu_blit_dst_x = (uint32_t)x;
	gpu_blit_dst_y = (uint32_t)y;
	gpu_blit_width = (uint32_t)w;
	gpu_blit_height = (uint32_t)h;
	gpu_blit_pattern = color ? 0xFFFFFFFFu : 0x00000000u;
	gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_FILL | GPU_BLIT_CTRL_CLIP;

	maskirq(old_mask);

	// wait for this fill to actually finish before returning -- see
	// zgfx.h's comment on why this can't rely on the next operation
	// to serialize the way consecutive glyph blits do.
	gpu_blit_wait_idle();

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
		draw_char_sw(x, y, c, color, font, clip);
		return;
	}

	hw_blit_wait();	// wait for any previous glyph blit to finish --
					// deliberately outside the masked section below,
					// same reasoning as z_fb_hw_line()'s own
					// gpu_wait_fifo() comment: this can legitimately
					// take a while, and masking through it would
					// stall the scheduler for every other process,
					// not just this one.

	// z_fb_hw_line()/z_fb_hw_fill_rect() both mask IRQs around their
	// own multi-register hardware setup, specifically because the
	// blitter/rasterizer registers are SHARED, board-wide hardware --
	// term/hello_win/wm (and multiple `term` instances at once) can
	// all reach this same register set, preemptively scheduled. This
	// function used to be the one exception: 7 separate MMIO writes
	// (dst_x, dst_y, glyph_addr, glyph_w, glyph_h, fg_color, bg_color)
	// before the ctrl=START trigger, all unprotected -- a timer IRQ
	// landing between any two of them could let a DIFFERENT process's
	// own glyph-blit setup interleave before ctrl=START ever fires,
	// mixing one process's coordinates with another's glyph/color,
	// which is exactly the kind of thing that shows up as garbled
	// pixels near text on real hardware, timing-dependent (worse with
	// more than one process actually drawing glyphs around the same
	// time, e.g. two `term` windows, or `term` alongside `hello_win`).
	uint32_t old_mask = maskirq(0xFFFFFFFF);

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

	maskirq(old_mask);

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

// software fallback for z_fb_draw_char2() below, used the same way
// draw_char_sw() is used by z_fb_draw_char() -- a glyph that needs
// clipping, since the hardware blit is unclipped by design. Unlike
// draw_char_sw() (transparent overlay, only touches ink pixels), this
// draws a SOLID cell -- fills the whole glyph-sized rect with bg_color
// first, then ink pixels with fg_color on top -- matching what the
// hardware path below actually does, so clipped and unclipped glyphs
// look identical.
static void draw_char_sw2(int x, int y, char c, int fg_color, int bg_color,
	const z_font_t *font, const z_clip_t *clip) {

	unsigned char uc = (unsigned char)c;
	const uint8_t *glyph = font->glyphs + (uc - font->first) * font->h;

	z_fb_fill_rect(x, y, font->w, font->h, bg_color, clip);

	for (int row = 0; row < font->h; row++) {
		uint8_t bits = glyph[row];
		for (int col = 0; col < font->w; col++) {
			if (bits & (0x80 >> col))
				z_fb_set_pixel(x + col, y + row, fg_color, clip);
		}
	}

}

void z_fb_draw_char2(int x, int y, char c, int fg_color, int bg_color,
	const z_font_t *font, const z_clip_t *clip) {

	unsigned char uc = (unsigned char)c;
	if (uc < font->first || uc > font->last) return;

	bool fits =
		x >= 0 && y >= 0 &&
		x + font->w <= Z_SCREEN_W && y + font->h <= Z_SCREEN_H &&
		(!clip ||
			(x >= clip->x0 && y >= clip->y0 &&
			 x + font->w - 1 <= clip->x1 && y + font->h - 1 <= clip->y1));

	if (!fits) {
		hw_blit_wait();	// see z_fb_draw_char()'s own comment on why
		draw_char_sw2(x, y, c, fg_color, bg_color, font, clip);
		return;
	}

	hw_blit_wait();	// wait for any previous glyph blit to finish --
					// deliberately outside the masked section below,
					// same reasoning as z_fb_draw_char()'s own comment.

	// see z_fb_draw_char()'s own comment above for why this needs the
	// same IRQ masking z_fb_hw_line()/z_fb_hw_fill_rect() already
	// have -- this function has the identical 7-write-then-trigger
	// shape, and term.c's own per-cell rendering (draw_cell(), via
	// this exact function) is precisely the kind of frequent,
	// multi-process-concurrent caller that gap mattered for.
	uint32_t old_mask = maskirq(0xFFFFFFFF);

	gpu_blit_dst_x = x;
	gpu_blit_dst_y = y;
	gpu_blit_glyph_addr = (uint32_t)(uc - font->first) * font->h;
	gpu_blit_glyph_w = font->w;
	gpu_blit_glyph_h = font->h;
	gpu_blit_fg_color = fg_color ? 1 : 0;
	gpu_blit_bg_color = bg_color ? 1 : 0;
	gpu_blit_ctrl = GPU_BLIT_CTRL_START | GPU_BLIT_CTRL_GLYPH;

	maskirq(old_mask);

}

void z_fb_draw_text2(int x, int y, const char *s, int fg_color, int bg_color,
	const z_font_t *font, const z_clip_t *clip) {

	int cx = x, cy = y;

	for (; *s; s++) {
		if (*s == '\n') {
			cx = x;
			cy += font->h;
			continue;
		}
		z_fb_draw_char2(cx, cy, *s, fg_color, bg_color, font, clip);
		cx += font->w;
	}

	hw_blit_wait();	// see z_fb_draw_text()'s own comment on why

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

void z_fb_draw_char2(int x, int y, char c, int fg_color, int bg_color,
	const z_font_t *font, const z_clip_t *clip) {

	unsigned char uc = (unsigned char)c;
	if (uc < font->first || uc > font->last) return;

	const uint8_t *glyph = font->glyphs + (uc - font->first) * font->h;

	z_fb_fill_rect(x, y, font->w, font->h, bg_color, clip);

	for (int row = 0; row < font->h; row++) {
		uint8_t bits = glyph[row];
		for (int col = 0; col < font->w; col++) {
			if (bits & (0x80 >> col))
				z_fb_set_pixel(x + col, y + row, fg_color, clip);
		}
	}

}

void z_fb_draw_text2(int x, int y, const char *s, int fg_color, int bg_color,
	const z_font_t *font, const z_clip_t *clip) {

	int cx = x, cy = y;

	for (; *s; s++) {
		if (*s == '\n') {
			cx = x;
			cy += font->h;
			continue;
		}
		z_fb_draw_char2(cx, cy, *s, fg_color, bg_color, font, clip);
		cx += font->w;
	}

}

#endif
