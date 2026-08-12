/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * App-side window helpers. See zwin.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "zeitlos.h"
#include "zobj.h"
#include "zwm.h"
#include "zwin.h"
#include "zgfx.h"

z_rv z_win_create(z_win_t *win, const char *title, uint32_t w, uint32_t h) {

	z_obj_t args = z_obj_map(3);
	z_map_set(&args, "title", z_obj_str(title ? title : ""));
	if (w) z_map_set(&args, "w", z_obj_uint32(w));
	if (h) z_map_set(&args, "h", z_obj_uint32(h));

	z_msg_new_send(Z_PID_WM, Z_WM_CREATE_WINDOW, 0, args);
	// note: `args` is intentionally never freed here -- same accepted
	// leak/lifetime tradeoff documented in docs/messaging.md. we're
	// about to block on the reply below, so it's still valid for the
	// wm to read for as long as it needs.

	z_msg_t reply;
	if (z_msg_wait(&reply, Z_WM_WINDOW_CREATED, 0) != Z_OK) {
		win->id = -1;
		return Z_FAIL;
	}

	if (!z_win_parse_rect(win, &reply.obj) || win->id < 0)
		return Z_FAIL;

	return Z_OK;

}

bool z_win_parse_rect(z_win_t *win, z_obj_t *obj) {

	z_obj_t *id = z_map_find(obj, "id");
	if (!id || id->type != Z_INT32) return false;
	win->id = id->val.int32;

	if (win->id < 0) return true;	// failure reply -- id is all that's set

	z_obj_t *x = z_map_find(obj, "x");
	z_obj_t *y = z_map_find(obj, "y");
	z_obj_t *w = z_map_find(obj, "w");
	z_obj_t *h = z_map_find(obj, "h");
	if (!x || !y || !w || !h) return false;

	win->x = x->val.uint32;
	win->y = y->val.uint32;
	win->w = w->val.uint32;
	win->h = h->val.uint32;

	return true;

}

void z_win_apply_redraw(z_win_t *win, uint32_t packed) {
	win->x = Z_WM_UNPACK_X(packed);
	win->y = Z_WM_UNPACK_Y(packed);
}

void z_win_redraw_done(const z_win_t *win) {
	z_msg_new_send(Z_PID_WM, Z_WM_REDRAW_DONE, 0, z_obj_uint32((uint32_t)win->id));
}

static void content_clip(const z_win_t *win, z_clip_t *clip) {
	// inset by 1px on every side -- the un-inset rect's edges coincide
	// exactly with where wm draws the window's border lines (left,
	// right, bottom, and the titlebar separator), so without this,
	// z_win_clear()/z_win_fill_rect() paint directly over the border
	// wherever content touches it.
	clip->x0 = win->x + 1;
	clip->y0 = win->y + Z_WM_TITLEBAR_H + 1;
	clip->x1 = win->x + win->w - 2;
	clip->y1 = win->y + win->h - 2;
}

void z_win_fill_rect(const z_win_t *win, int x, int y, int w, int h, int color) {
	z_clip_t clip;
	content_clip(win, &clip);
	z_fb_fill_rect(win->x + x, clip.y0 + y, w, h, color, &clip);
}

void z_win_clear(const z_win_t *win) {
	// oversized on purpose -- content_clip() (via z_win_fill_rect's
	// clip) cuts this down to the actual content area regardless.
	z_win_fill_rect(win, 0, 0, win->w, win->h, 0);
}

void z_win_draw_text(const z_win_t *win, int x, int y, const char *s, int color, const z_font_t *font) {
	z_clip_t clip;
	content_clip(win, &clip);
	z_fb_draw_text(win->x + x, clip.y0 + y, s, color, font, &clip);
}
