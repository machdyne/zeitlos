#ifndef ZWIN_H
#define ZWIN_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * App-side window helpers: create a window through the wm (zwm.h) and
 * draw content into it, automatically clipped to the window's own
 * content area (below the titlebar). See docs/window_manager.md.
 *
 * This is the sanctioned way for an app to draw -- it's not enforced
 * (an app can still write to the framebuffer directly through zgfx.h,
 * or poke VRAM itself; apps are fully trusted in this system), but an
 * app that only uses z_win_* calls physically cannot draw outside its
 * own window, since every draw call clips to the window's own rect.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zobj.h"
#include "zfont.h"
#include "zgfx.h"

typedef struct {
	int32_t		id;	// -1 if creation failed
	uint32_t	x, y, w, h;
} z_win_t;

// sends Z_WM_CREATE_WINDOW and blocks for the reply. title may be
// NULL. w/h of 0 fall back to the wm's defaults (Z_WM_DEFAULT_WIDTH/
// HEIGHT in zwm.h). returns Z_FAIL if the wm didn't reply or refused.
z_rv z_win_create(z_win_t *win, const char *title, uint32_t w, uint32_t h);

// like z_win_create(), but places the window at an exact screen
// position (x, y both >= 0) instead of letting the wm auto-cascade
// it -- see zwin.c's own comment for why this is safe to call
// (creation is exempt from the wm's redraw-ack wait) and why it's a
// separate function rather than new parameters on z_win_create()
// itself (keeps every existing caller unaffected).
z_rv z_win_create_ex(z_win_t *win, const char *title, uint32_t w, uint32_t h,
	int32_t x, int32_t y);

// parses a Z_MAP{id,x,y,w,h} object (as sent with Z_WM_WINDOW_CREATED
// or Z_WM_WINDOW_MOVED) into *win. returns false if obj doesn't have
// the expected shape.
bool z_win_parse_rect(z_win_t *win, z_obj_t *obj);

// applies a Z_WM_REDRAW payload (a packed Z_UINT32, not a Z_MAP --
// see zwm.h) to *win, updating x/y only. w/h are left untouched (they
// don't change -- there's no resize support yet).
void z_win_apply_redraw(z_win_t *win, uint32_t packed);

// call this once you're done redrawing in response to Z_WM_REDRAW.
// the wm blocks on this (per window, back-to-front) before letting
// any window in front of yours redraw its own content -- without it,
// content is drawn whenever each app's process happens to get
// scheduled, with no guarantee that's in z-order, so a window behind
// yours could end up drawn on top. see docs/window_manager.md,
// "content z-order". not needed for redraws you initiate yourself
// (the wm isn't waiting on those).
void z_win_redraw_done(const z_win_t *win);

// clears the window's content area (below the titlebar) to color 0
void z_win_clear(const z_win_t *win);

// fills a sub-rectangle of the content area (content-relative
// coordinates, (0,0) is just below the titlebar). useful for
// updating one line of text without clearing (and flashing) the
// whole window -- see sw/apps/hello_win for the pattern.
void z_win_fill_rect(const z_win_t *win, int x, int y, int w, int h, int color);

// draws text at (x,y) relative to the top-left of the content area
// (i.e. (0,0) is just below the titlebar, not the top of the window).
// '\n' starts a new line. clipped to the window's own bounds. pass
// &z_font_6x12 or &z_font_8x16 (zfont.h) for font.
void z_win_draw_text(const z_win_t *win, int x, int y, const char *s, int color, const z_font_t *font);

// the window's content area (below the titlebar, inset to clear wm's
// own outer border) in absolute screen coordinates. This is exactly
// what z_win_clear()/z_win_fill_rect()/z_win_draw_text()/
// z_win_hw_line() etc. all clip to internally -- exposed so callers
// that need to know their own drawable bounds for something else
// (centering content, bouncing something off the edges, and so on)
// can query it instead of duplicating the formula themselves. That
// duplication is exactly what caused a real, previously-shipped bug:
// two callers each kept their own copy of this rectangle, and one of
// them fell out of sync with a border-inset change made here. See
// docs/window_manager.md.
//
// Does NOT need to account for the focused-window highlight anymore
// -- that's now drawn just outside the window's own frame (wm.c's
// draw_window_box()), not inside it, so it never overlaps content
// regardless of focus state.
void z_win_content_rect(const z_win_t *win, z_clip_t *out);

// hardware-accelerated line/box draw via the GPU line rasterizer
// (rtl/gpu/gpu_raster.v) -- absolute screen coordinates (unlike
// z_win_fill_rect()/z_win_draw_text() above, which are
// window-relative: apps drawing through the hardware rasterizer
// typically already compute absolute coordinates themselves, e.g. a
// 3D projection centered on the window, so this only takes over
// clip-region and IRQ-mask management, not coordinate translation).
// Automatically clips to the window's own content area on every call
// -- see zgfx.h's z_fb_hw_line() for why that needed IRQ masking to
// be safe for concurrent access from multiple processes; none of
// that is visible here.
void z_win_hw_line(const z_win_t *win, int x0, int y0, int x1, int y1, int color);
void z_win_hw_box(const z_win_t *win, int x0, int y0, int x1, int y1, int color);

// tells the wm to destroy this window (fire-and-forget, no reply --
// see zwin.c's own comment). Not previously exposed as a client
// helper -- only the raw Z_WM_DESTROY_WINDOW message subject existed
// (zwm.h) -- added for the Zeitlos Scheme API's (win-destroy id),
// docs/scheme_api.md.
void z_win_destroy(const z_win_t *win);

#endif
