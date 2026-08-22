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
	return z_win_create_ex(win, title, w, h, -1, -1);
}

// like z_win_create(), but places the window at an exact screen
// position instead of letting the wm auto-cascade it -- x/y >= 0
// both required to take effect (either one negative falls back to
// the normal cascade, same as z_win_create() itself always requests).
// Safe to call regardless of what else is on screen: unlike MOVING an
// existing window later would be, window CREATION is explicitly
// exempt from the wm's redraw-ack wait (wm.c's own create_window()
// caller uses repair_region()'s exclude_idx specifically for this --
// see docs/window_manager.md, "Content z-order" -- since a brand-new
// window's owner is still blocked on Z_WM_WINDOW_CREATED and can't
// possibly be listening for Z_WM_REDRAW yet). A NEW function rather
// than adding parameters to z_win_create() itself, so every existing
// caller (sw/apps/hello_win) is completely unaffected.
z_rv z_win_create_ex(z_win_t *win, const char *title, uint32_t w, uint32_t h,
	int32_t x, int32_t y) {

	z_obj_t args = z_obj_map(5);
	z_map_set(&args, "title", z_obj_str(title ? title : ""));
	if (w) z_map_set(&args, "w", z_obj_uint32(w));
	if (h) z_map_set(&args, "h", z_obj_uint32(h));
	if (x >= 0 && y >= 0) {
		z_map_set(&args, "x", z_obj_uint32((uint32_t)x));
		z_map_set(&args, "y", z_obj_uint32((uint32_t)y));
	}

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
	// inset by 2px on every content-bearing edge: 1px to clear the
	// window's own outer border/titlebar-separator line, plus a
	// genuine 1px blank margin beyond that so content never sits
	// directly against the frame. A 1px-only inset (just enough to
	// not share a pixel with the border) used to be here instead --
	// mathematically correct (content and border never touched the
	// same pixel), but visually wrong: zero blank pixels between
	// them reads as text right up against, or even overlapping, the
	// frame, which is exactly what it looked like on real hardware.
	// This is a real margin, not a border-avoidance side effect --
	// worth restating since an EARLIER version of this inset also
	// happened to be 2px, but only on left/right/bottom and only
	// because wm.c's old focus-border used to draw 1px INSIDE the
	// frame, needing a second pixel of clearance for a completely
	// different reason (that focus-border now draws outside the
	// frame instead, see wm.c's draw_window_box()) -- this version
	// applies the same 2px inset on all four sides, top included,
	// specifically for visual breathing room, not to dodge anything
	// else drawn nearby.
	out->x0 = win->x + 2;
	out->y0 = win->y + Z_WM_TITLEBAR_H + 2;
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

// tells the wm to destroy this window -- fire-and-forget, no reply
// (see wm.c's own Z_WM_DESTROY_WINDOW handler: it repairs the screen
// region itself and doesn't send anything back). Safe to call even if
// win->id is already -1 (a failed z_win_create(), or a window that
// was never actually created) -- the wm just won't find a matching id
// and drops it, same as it already does for any unrecognized id.
void z_win_destroy(const z_win_t *win) {
	if (win->id < 0) return;
	z_msg_new_send(resolve_wm_pid(), Z_WM_DESTROY_WINDOW, 0, z_obj_uint32((uint32_t)win->id));
}
