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

// resolved once, cached for the lifetime of this process -- z_win_*
// can be called often (z_win_redraw_done() potentially once per
// redraw), and re-doing a name lookup on every single call would be
// wasteful when wm's pid never changes for as long as this process
// runs. Falls back to the fixed Z_PID_WM constant (zwm.h) if lookup
// ever fails -- e.g. wm hasn't registered itself yet (a startup-order
// race, though wm is normally started well before any client) or is
// an older build that predates the registry. A false resolution here
// isn't fatal either way: z_msg_send() already fails safely against
// a wrong/dead pid, same as it always has -- see sw/os/pidreg.h's
// comment on this same tradeoff.
static uint32_t wm_pid_cache;
static bool wm_pid_resolved = false;

static uint32_t resolve_wm_pid(void) {
	if (!wm_pid_resolved) {
		if (!z_pid_lookup("wm0", &wm_pid_cache))
			wm_pid_cache = Z_PID_WM;
		wm_pid_resolved = true;
	}
	return wm_pid_cache;
}

z_rv z_win_create(z_win_t *win, const char *title, uint32_t w, uint32_t h) {

	z_obj_t args = z_obj_map(3);
	z_map_set(&args, "title", z_obj_str(title ? title : ""));
	if (w) z_map_set(&args, "w", z_obj_uint32(w));
	if (h) z_map_set(&args, "h", z_obj_uint32(h));

	z_msg_new_send(resolve_wm_pid(), Z_WM_CREATE_WINDOW, 0, args);
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
	z_msg_new_send(resolve_wm_pid(), Z_WM_REDRAW_DONE, 0, z_obj_uint32((uint32_t)win->id));
}

void z_win_content_rect(const z_win_t *win, z_clip_t *out) {
	// inset by 2px on left/right/bottom, 1px below the titlebar
	// separator on top. Two separate things overlap the outer 1px
	// border here: the border itself (win->x/win->x+win->w-1 etc),
	// and -- only when the window is focused -- wm.c's own bold
	// focus-border, drawn a further 1px in (win->x+1/win->w-2 etc,
	// on left/right/bottom; its top edge sits inside the titlebar
	// area, not down here, so no extra inset is needed on that side).
	// A 1px-only inset used to coincide exactly with the focus
	// border's own pixels -- invisible for content that stays well
	// clear of the edge (nothing before drew close enough to notice),
	// but any content actually reaching the content area's own
	// boundary would draw directly on top of the focus border,
	// visibly gnawing at it whenever the window happened to be
	// focused.
	out->x0 = win->x + 2;
	out->y0 = win->y + Z_WM_TITLEBAR_H + 1;
	out->x1 = win->x + win->w - 3;
	out->y1 = win->y + win->h - 3;
}

void z_win_fill_rect(const z_win_t *win, int x, int y, int w, int h, int color) {
	z_clip_t clip;
	z_win_content_rect(win, &clip);
	z_fb_fill_rect(win->x + x, clip.y0 + y, w, h, color, &clip);
}

void z_win_clear(const z_win_t *win) {
	// oversized on purpose -- z_win_content_rect() (via
	// z_win_fill_rect's clip) cuts this down to the actual content
	// area regardless.
	z_win_fill_rect(win, 0, 0, win->w, win->h, 0);
}

void z_win_draw_text(const z_win_t *win, int x, int y, const char *s, int color, const z_font_t *font) {
	z_clip_t clip;
	z_win_content_rect(win, &clip);
	z_fb_draw_text(win->x + x, clip.y0 + y, s, color, font, &clip);
}

void z_win_hw_line(const z_win_t *win, int x0, int y0, int x1, int y1, int color) {
	z_clip_t clip;
	z_win_content_rect(win, &clip);
	z_fb_hw_line(x0, y0, x1, y1, color, &clip);
}

void z_win_hw_box(const z_win_t *win, int x0, int y0, int x1, int y1, int color) {
	z_clip_t clip;
	z_win_content_rect(win, &clip);
	z_fb_hw_box(x0, y0, x1, y1, color, &clip);
}
