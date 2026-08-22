#ifndef ZGFX_H
#define ZGFX_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Direct-framebuffer pixel/text drawing, plus (see z_fb_hw_line()
 * below) IRQ-masked access to the GPU line rasterizer
 * (rtl/gpu/gpu_raster.v). The two are different enough to be worth
 * calling out: z_fb_set_pixel()/z_fb_fill_rect()/etc. are plain
 * memory writes to the 1bpp framebuffer, so two apps drawing into two
 * different (non-overlapping) windows can't race on anything -- no
 * shared state involved at all. z_fb_hw_line() is different: the
 * rasterizer's registers are global, shared peripheral state with no
 * per-process isolation whatsoever, so any two processes drawing
 * through it *can* race, in two distinct ways -- see z_fb_hw_line()'s
 * own comment for both, and why IRQ masking (not avoiding the
 * hardware, which is what this file did originally) is what actually
 * closes them.
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
#include "zicon.h"

#define Z_SCREEN_W 640
#define Z_SCREEN_H 480

// inclusive bounds
typedef struct {
	int32_t x0, y0, x1, y1;
} z_clip_t;

// clip may be NULL to clip to the screen only
void z_fb_set_pixel(int x, int y, int color, const z_clip_t *clip);
void z_fb_fill_rect(int x, int y, int w, int h, int color, const z_clip_t *clip);
void z_fb_draw_char(int x, int y, char c, int color, const z_font_t *font, const z_clip_t *clip);
void z_fb_draw_text(int x, int y, const char *s, int color, const z_font_t *font, const z_clip_t *clip);

// like z_fb_draw_char()/z_fb_draw_text(), but with fg AND bg
// independently colorable, instead of z_fb_draw_char()'s bg hardcoded
// to 0 (see its own comment). The hardware glyph blitter
// (rtl/gpu/gpu_blit.v) already has two genuinely independent color
// registers (fg_color_reg/bg_color_reg, each latched into every pixel
// of the cell depending on that pixel's glyph bit -- see
// ST_GLYPH_WRITE_LO's read-modify-write) -- z_fb_draw_char() just
// never exposed the second one. Useful for anything drawing solid
// two-color cells, e.g. a terminal emulator's reverse-video attribute
// (sw/apps/term) -- swap fg_color/bg_color for a "reverse" cell
// instead of falling back to per-pixel software rendering, which is
// what motivated adding this: sw/apps/term used to do exactly that
// (see its own git history) and redrawing a full 80x25 grid through
// z_fb_set_pixel() one pixel at a time was slow enough to be visibly
// laggy while typing.
void z_fb_draw_char2(int x, int y, char c, int fg_color, int bg_color, const z_font_t *font, const z_clip_t *clip);
void z_fb_draw_text2(int x, int y, const char *s, int fg_color, int bg_color, const z_font_t *font, const z_clip_t *clip);

// hardware-accelerated line draw via the GPU line rasterizer
// (rtl/gpu/gpu_raster.v). Handles both hazards of that shared
// hardware state internally -- callers never need to think about
// either one:
//   - clip == NULL disables the rasterizer's own hardware clipping
//     entirely (for callers, like wm.c, that already know their
//     coordinates are valid and never want clipping); clip != NULL
//     sets the rasterizer's clip region to those bounds first. Either
//     way this is asserted fresh on every single call, never assumed
//     to still be correctly set from a previous call, since some
//     other process's own drawing may have changed it in the
//     meantime.
//   - the register-writes-then-trigger sequence is wrapped in
//     maskirq() so it can't be interleaved with another process's own
//     sequence (which would otherwise produce a line drawn from a mix
//     of two callers' parameters) -- but only that sequence, not the
//     FIFO-wait beforehand, which can legitimately take a while if
//     the FIFO's backed up and shouldn't stall the scheduler for
//     other processes while it does.
void z_fb_hw_line(int x0, int y0, int x1, int y1, int color, const z_clip_t *clip);

// four z_fb_hw_line() calls forming a rectangle outline -- see
// z_fb_hw_line() for the clip/coordinate semantics.
void z_fb_hw_box(int x0, int y0, int x1, int y1, int color, const z_clip_t *clip);

// hardware-accelerated rectangle fill via the GPU blitter
// (rtl/gpu/gpu_blit.v, fill mode -- see docs/gpu_blitter.md). Same
// two hazards as z_fb_hw_line()/the line rasterizer, both closed the
// same way -- see that function's own comment for the reasoning in
// full:
//   - x/y/w/h are clamped to the actual screen bounds unconditionally,
//     not just when the blitter's own CTRL_CLIP is set: rtl/gpu/
//     gpu_blit.v's ST_CLIP state only bounds-checks the destination
//     when CTRL_CLIP is requested, and even then, ST_WAIT_READ/
//     ST_WAIT_WRITE wait on the framebuffer bus's ack with no timeout
//     of their own -- an out-of-range destination can hang the
//     blitter's state machine forever, same as the rasterizer.
//   - the register-writes-then-trigger sequence is IRQ-masked, for
//     the same reason and the same way as z_fb_hw_line().
// Unlike z_fb_hw_line() (which only waits for FIFO space before
// submitting, letting the hardware drain asynchronously), this waits
// for the blitter to actually finish before returning: the blitter
// has no FIFO/queue at all (one operation at a time, globally shared
// -- see "Concurrent register access" in docs/gpu_blitter.md, still
// not arbitrated between processes), and whatever a caller does right
// after a fill is very often *not* going through this same hardware
// (e.g. wm.c's own draw_window_box(), which draws through the line
// rasterizer instead) -- so there's no other natural point where that
// next operation would otherwise wait for this fill's pixel writes to
// have actually landed.
void z_fb_hw_fill_rect(int x, int y, int w, int h, int color);

// loads a font's glyph data into hardware glyph memory (rtl/mem/glyph.v)
// for use by the hardware-accelerated draw path. Call once, before the
// first draw using that font. Always declared/callable regardless of
// Z_GFX_HW_BLIT -- it's a no-op when built without it, so callers don't
// need their own #ifdef. See docs/window_manager.md, "hardware glyph
// blitting".
void z_gfx_hw_font_load(const z_font_t *font);

// loads one 8x8 window icon's glyph data (Z_ICON_H bytes, one row per
// byte, MSB-first -- same row-major/bit-order convention as a
// z_font_t's own glyphs, see zfont.h) into hardware glyph memory's
// reserved icon region (zicon.h's Z_ICON_MEM_OFFSET). `icon_id` is a
// zicon.h z_icon_id_t. Same single-owner discipline as
// z_gfx_hw_font_load(): call this once per icon, from wm's own
// startup, right after loading the font -- see sw/apps/wm/win_icons.h
// and wm.c's main(). Always declared/callable regardless of
// Z_GFX_HW_BLIT -- a documented no-op without it, so callers don't
// need their own #ifdef, same contract z_gfx_hw_font_load() has.
void z_gfx_hw_icon_load(int icon_id, const uint8_t *bitmap);

// hardware-blits window icon `icon_id` (zicon.h) at (x,y), 8x8,
// solid two-color-cell semantics (fg where an icon bit is set, bg
// where it isn't) -- same as z_fb_draw_char2(). Unlike font glyphs
// there's no software fallback for a partially off-screen/clipped
// icon: window icons are only ever drawn by wm itself, entirely
// within a titlebar rect it already computed and knows is on-screen
// (see sw/apps/wm/wm.c's draw_titlebar_content()), so that case is
// never actually exercised, and a call that doesn't fully fit simply
// draws nothing rather than carrying a second, untested rendering
// path. Always declared/callable regardless of Z_GFX_HW_BLIT -- a
// documented no-op without it (window icons are a hardware-only
// feature, see zicon.h's own header comment), so callers don't need
// their own #ifdef.
void z_fb_draw_icon(int x, int y, int icon_id, int fg_color, int bg_color, const z_clip_t *clip);

#endif
