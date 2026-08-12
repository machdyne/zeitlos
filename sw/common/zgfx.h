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

#endif
