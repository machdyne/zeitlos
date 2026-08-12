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

typedef struct {
	int32_t		id;	// -1 if creation failed
	uint32_t	x, y, w, h;
} z_win_t;

// sends Z_WM_CREATE_WINDOW and blocks for the reply. title may be
// NULL. w/h of 0 fall back to the wm's defaults (Z_WM_DEFAULT_WIDTH/
// HEIGHT in zwm.h). returns Z_FAIL if the wm didn't reply or refused.
z_rv z_win_create(z_win_t *win, const char *title, uint32_t w, uint32_t h);

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

#endif
