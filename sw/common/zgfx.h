#ifndef ZGFX_H
#define ZGFX_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Direct-framebuffer pixel/text drawing. Deliberately separate from
 * the GPU line rasterizer/blitter -- these are plain memory writes to
 * the 1bpp framebuffer, not shared hardware registers, so two apps
 * drawing into two different (non-overlapping) windows can't race on
 * hardware state the way they could racing on the rasterizer's clip
 * rect. See docs/window_manager.md for the caveats this doesn't cover
 * (overlapping windows sharing a framebuffer word).
 *
 * All coordinates are absolute screen coordinates. z_win.h layers
 * window-relative, auto-clipped helpers on top of this for apps that
 * just want to draw inside their own window.
 *
 * Character/text drawing (z_fb_draw_char/z_fb_draw_text) has two
 * implementations, selected at compile time by defining Z_GFX_HW_BLIT
 * (add `-DZ_GFX_HW_BLIT` to CFLAGS, and link rtl-side glyph memory
 * must actually be present in the bitstream -- see rtl/mem/glyph.v
 * and MEM_GLYPH in rtl/sysctl.v):
 *   - undefined (default): the original software renderer, direct
 *     per-pixel writes. Always correct, always available.
 *   - defined: drives the hardware glyph blitter (rtl/gpu/gpu_blit.v)
 *     instead, which is dramatically faster for anything more than a
 *     handful of characters -- but falls back to the software path
 *     for any glyph that would need partial clipping, since the
 *     hardware blit is unclipped by design. See
 *     docs/window_manager.md, "hardware glyph blitting" for the full
 *     writeup, including what hasn't been tested on real hardware
 *     yet.
 */

#include <stdint.h>
#include "zfont.h"

#define Z_SCREEN_W 512
#define Z_SCREEN_H 384

// inclusive bounds
typedef struct {
	int32_t x0, y0, x1, y1;
} z_clip_t;

// clip may be NULL to clip to the screen only
void z_fb_set_pixel(int x, int y, int color, const z_clip_t *clip);
void z_fb_fill_rect(int x, int y, int w, int h, int color, const z_clip_t *clip);
void z_fb_draw_char(int x, int y, char c, int color, const z_font_t *font, const z_clip_t *clip);
void z_fb_draw_text(int x, int y, const char *s, int color, const z_font_t *font, const z_clip_t *clip);

// loads a font's glyph data into hardware glyph memory (rtl/mem/glyph.v)
// for use by the hardware-accelerated draw path. Call once, before the
// first draw using that font. Always declared/callable regardless of
// Z_GFX_HW_BLIT -- it's a no-op when built without it, so callers don't
// need their own #ifdef. See docs/window_manager.md, "hardware glyph
// blitting".
void z_gfx_hw_font_load(const z_font_t *font);

#endif
