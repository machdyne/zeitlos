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

// Returns 0 if wm isn't running.
//
// No fallback to the fixed Z_PID_WM constant: a miss means wm is not
// there, and guessing a pid sends window requests to whatever process
// happens to occupy it -- pid 0 is the kernel. The pid wm lands on
// depends entirely on start order.
//
// A failed lookup is NOT cached: an app may start before wm has
// registered, and caching the miss would keep reporting "no wm" long
// after one appeared.
static uint32_t resolve_wm_pid(void) {
	if (!wm_pid_resolved) {
		if (!z_pid_lookup("wm0", &wm_pid_cache)) return 0;
		wm_pid_resolved = true;
	}
	return wm_pid_cache;
}

// -- static message payloads --
//
// See z_win_create_cb() below for why these aren't built with
// z_obj_map()/z_map_set(): those allocate, the result can't safely be
// freed, and an app that creates windows repeatedly therefore leaks
// until it dies.
//
// CREATE_WINDOW carries at most six keys (title, w, h, x, y, flags).
#define Z_WIN_CREATE_KEYS   6

static z_obj_t create_keys[Z_WIN_CREATE_KEYS];
static z_obj_t create_vals[Z_WIN_CREATE_KEYS];
static z_obj_table_t create_tbl;
static char create_title[64];

// SET_TITLE carries exactly two keys (id, title).
#define Z_WIN_TITLE_SLOTS   2

static z_obj_t title_keys[Z_WIN_TITLE_SLOTS][2];
static z_obj_t title_vals[Z_WIN_TITLE_SLOTS][2];
static z_obj_table_t title_tbl[Z_WIN_TITLE_SLOTS];
static char title_text[Z_WIN_TITLE_SLOTS][32];
static int title_slot;

z_rv z_win_create(z_win_t *win, const char *title, uint32_t w, uint32_t h) {
	return z_win_create_ex(win, title, w, h, -1, -1);
}

// see zwin.h's own comment on why this delegates to
// z_win_create_flags(): z_win_create()/z_win_create_ex() are just the
// flags=0 (no close icon) special case, kept around so no existing
// caller needs to change.

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
	return z_win_create_flags(win, title, w, h, x, y, 0);
}

// does the actual work for all three z_win_create*() entry points --
// see zwin.h's own comment on why there are three. `flags` is a
// Z_WIN_FLAG_* bitmask (zwm.h), sent to the wm as-is; 0 means "no
// close icon", same as before this parameter existed.
z_rv z_win_create_flags(z_win_t *win, const char *title, uint32_t w, uint32_t h,
	int32_t x, int32_t y, uint32_t flags) {

	return z_win_create_cb(win, title, w, h, x, y, flags, NULL, NULL);

}

int z_win_redraw_id(uint32_t packed) {
	return (int)Z_WM_UNPACK_ID(packed);
}

// does the actual work for all four z_win_create*() entry points. See
// zwin.h for why the callback exists -- in short, z_msg_wait() throws
// away every message that isn't the one it's waiting for, and for an
// app creating a window while already running, one of those thrown-
// away messages can be a Z_WM_REDRAW that wm is blocking on an ack
// for.
z_rv z_win_create_cb(z_win_t *win, const char *title, uint32_t w, uint32_t h,
	int32_t x, int32_t y, uint32_t flags, z_win_msg_cb cb, void *user) {

	// Built in static storage, not with z_obj_map()/z_map_set().
	//
	// Those malloc() a table, two arrays and a copy of every key
	// string, and the result was then deliberately never freed --
	// the payload is borrowed until wm reads it, so freeing it here
	// would race (docs/messaging.md). That is fine for an app that
	// creates one window at startup and unbounded for one that
	// creates them repeatedly, which is exactly what an app showing
	// dialogs does: roughly 380 bytes per dialog, out of the 16KB an
	// app gets for stack AND heap together
	// (Z_PROC_STACK_SIZE_DEFAULT, sw/os/kernel.h). wm had the same
	// bug on the other side of this exchange, where it showed up as
	// the machine crashing after ~20 window moves.
	//
	// One slot is enough here, unlike wm's ring: this function
	// BLOCKS on Z_WM_WINDOW_CREATED below, and wm only replies after
	// it has read the request. The payload cannot still be in flight
	// when the next call overwrites it.
	int n = 0;

	create_keys[n].type = Z_STR;
	create_keys[n].val.str = (char *)"title";
	create_vals[n].type = Z_STR;
	// Copied, because the caller's `title` may be a stack buffer that
	// is gone by the time wm reads this. The other values are
	// scalars, stored inline, and need no such care.
	{
		const char *src = title ? title : "";
		int i = 0;
		for (; i < (int)sizeof(create_title) - 1 && src[i]; i++)
			create_title[i] = src[i];
		create_title[i] = 0;
	}
	create_vals[n].val.str = create_title;
	n++;

	if (w) {
		create_keys[n].type = Z_STR;
		create_keys[n].val.str = (char *)"w";
		create_vals[n].type = Z_UINT32;
		create_vals[n].val.uint32 = w;
		n++;
	}

	if (h) {
		create_keys[n].type = Z_STR;
		create_keys[n].val.str = (char *)"h";
		create_vals[n].type = Z_UINT32;
		create_vals[n].val.uint32 = h;
		n++;
	}

	if (x >= 0 && y >= 0) {
		create_keys[n].type = Z_STR;
		create_keys[n].val.str = (char *)"x";
		create_vals[n].type = Z_UINT32;
		create_vals[n].val.uint32 = (uint32_t)x;
		n++;
		create_keys[n].type = Z_STR;
		create_keys[n].val.str = (char *)"y";
		create_vals[n].type = Z_UINT32;
		create_vals[n].val.uint32 = (uint32_t)y;
		n++;
	}

	// omitted entirely when 0 (no flags), same "missing key falls
	// back to a default" convention every other optional key here
	// already follows (zwm.h's own Z_WM_CREATE_WINDOW comment) --
	// not required for correctness (wm treats a missing "flags" the
	// same as an explicit 0), just consistent with the others.
	if (flags) {
		create_keys[n].type = Z_STR;
		create_keys[n].val.str = (char *)"flags";
		create_vals[n].type = Z_UINT32;
		create_vals[n].val.uint32 = flags;
		n++;
	}

	create_tbl.len = (uint32_t)n;
	create_tbl.a = create_keys;
	create_tbl.b = create_vals;

	z_obj_t args;
	args.type = Z_MAP;
	args.val.ptr = &create_tbl;

	uint32_t wmpid = resolve_wm_pid();
	if (!wmpid) return Z_FAIL;	// no wm running -- fail, don't guess
	z_msg_new_send(wmpid, Z_WM_CREATE_WINDOW, 0, args);

	z_msg_t reply;

	if (!cb) {

		// No callback -- the original behavior, kept verbatim for
		// every caller that predates this parameter.
		if (z_msg_wait(&reply, Z_WM_WINDOW_CREATED, 0) != Z_OK) {
			win->id = -1;
			return Z_FAIL;
		}

	} else {

		// Same wait, but nothing gets dropped on the floor. Spins on
		// the non-blocking z_msg_read() rather than z_msg_wait()
		// precisely because z_msg_wait() is the thing doing the
		// dropping -- there's no way to ask it for "the next message,
		// whatever it is, and let me decide".
		for (;;) {

			if (z_msg_read(&reply) != Z_OK) continue;

			if (reply.subject == Z_WM_WINDOW_CREATED && reply.tag == 0)
				break;

			cb(&reply, user);

		}

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

// Applies a Z_WM_SET_CLIP and acknowledges it.
//
// Call this from an app's message loop for Z_WM_SET_CLIP. It is one
// line at each call site rather than something zwin can do on the
// app's behalf, because there is no central message dispatch -- every
// app owns its own loop, the same way each already handles
// Z_WM_REDRAW and Z_WM_WINDOW_MOVED itself.
//
// THE ACK IS NOT OPTIONAL. wm waits for it when a region NARROWS,
// before it draws the window that did the narrowing -- that wait is
// the whole reason an app cannot still be drawing into pixels that
// are about to belong to someone else. An app that applies the region
// without acking will stall wm for the full timeout on every overlap.
//
// Returns false if the message was not a well-formed region, in which
// case nothing is changed and nothing is acked.
// Loads a window's own region into zgfx, so the region in force
// belongs to the window about to be drawn. See z_win_t.clip.
//
// Called at the top of every z_win_* drawing call. Cheap -- a memcpy
// of at most eight rectangles, and usually one -- and it is the only
// thing that makes an app with a dialog open draw both windows
// correctly.
static void win_use_clip(const z_win_t *win) {
	if (!win || win->clip_n <= 0) z_gfx_clear_visible();
	else z_gfx_set_visible(win->clip, win->clip_n);
}

bool z_win_apply_clip(z_win_t *win, z_obj_t *obj) {

	if (!obj || obj->type != Z_BLOB) return false;

	uint32_t len = z_blob_len(obj);
	const z_wm_cliprect_t *r = z_blob_data(obj);

	if (!r || (len % sizeof(z_wm_cliprect_t)) != 0) return false;

	int n = (int)(len / sizeof(z_wm_cliprect_t));
	if (n > Z_WM_MAX_CLIP) n = Z_WM_MAX_CLIP;

	z_clip_t rects[Z_WM_MAX_CLIP];
	for (int i = 0; i < n; i++) {
		rects[i].x0 = r[i].x0;
		rects[i].y0 = r[i].y0;
		rects[i].x1 = r[i].x1;
		rects[i].y1 = r[i].y1;
	}

	// n == 0 would mean "unrestricted" to zgfx, which is the opposite
	// of what an empty region means. wm never sends that -- a fully
	// occluded window arrives as one empty rectangle -- but a
	// malformed message must not be able to silently unclip an app.
	if (n == 0) return false;

	// Stored on the WINDOW, not applied globally here. The window
	// being drawn decides which region is in force, and that is
	// win_use_clip()'s job -- applying it here would mean the last
	// message received won, which is wrong the moment an app owns a
	// dialog as well as its main window.
	if (win) {
		for (int i = 0; i < n; i++) win->clip[i] = rects[i];
		win->clip_n = n;
	}

	uint32_t wmpid = resolve_wm_pid();
	if (wmpid)
		z_msg_new_send(wmpid, Z_WM_CLIP_DONE, 0,
			z_obj_uint32(win ? (uint32_t)win->id : 0));

	return true;

}

void z_win_apply_redraw(z_win_t *win, uint32_t packed) {
	win->x = Z_WM_UNPACK_X(packed);
	win->y = Z_WM_UNPACK_Y(packed);
}

bool z_win_apply_resized(z_win_t *win, z_obj_t *obj) {
	return z_win_parse_rect(win, obj);
}

bool z_win_mouse_content_xy(const z_win_t *win, uint32_t packed, int *cx, int *cy) {

	z_clip_t clip;
	z_win_content_rect(win, &clip);

	int sx = (int)Z_WM_UNPACK_MOUSE_X(packed);
	int sy = (int)Z_WM_UNPACK_MOUSE_Y(packed);

	*cx = sx - clip.x0;
	*cy = sy - clip.y0;

	return sx >= clip.x0 && sx <= clip.x1 && sy >= clip.y0 && sy <= clip.y1;

}

int z_win_content_w(const z_win_t *win) {
	z_clip_t clip;
	z_win_content_rect(win, &clip);
	int w = clip.x1 - clip.x0 + 1;
	return w > 0 ? w : 0;
}

int z_win_content_h(const z_win_t *win) {
	z_clip_t clip;
	z_win_content_rect(win, &clip);
	int h = clip.y1 - clip.y0 + 1;
	return h > 0 ? h : 0;
}

void z_win_redraw_done(const z_win_t *win) {
	uint32_t wmpid_r = resolve_wm_pid();
	if (wmpid_r)
		z_msg_new_send(wmpid_r, Z_WM_REDRAW_DONE, 0, z_obj_uint32((uint32_t)win->id));
}

void z_win_content_rect(const z_win_t *win, z_clip_t *out) {

	// Loads this window's visible region as a side effect.
	//
	// Deliberate, and worth the impurity. Apps that draw through
	// z_win_* get their region loaded by those calls, but plenty draw
	// straight to z_fb_* with a clip they got from HERE -- clock's
	// hands, draw's canvas, gpu3d's cube. This is the one call every
	// one of those makes immediately before drawing, so it is the
	// only chokepoint that covers them without adding a line to a
	// dozen apps and relying on nobody forgetting it in the
	// thirteenth.
	//
	// It also reads correctly at the call site: "give me where I may
	// draw" now sets up where you may draw.
	win_use_clip(win);
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

	win_use_clip(win);

	z_clip_t clip;
	z_win_content_rect(win, &clip);

	// Clamp to the content area ourselves, then fill through the
	// BLITTER. This used to call z_fb_fill_rect(), which is a
	// per-pixel software loop (one z_fb_set_pixel() per pixel, each a
	// read-modify-write of a VRAM word), and the cost of that is
	// proportional to AREA -- which nothing here noticed for as long
	// as the only callers were filling a line of text at a time.
	//
	// z_win_clear() is the same function with the whole window as its
	// rectangle, and that is where it became visible: clearing a
	// 288x216 dialog is ~62,000 individually clipped VRAM
	// read-modify-writes, which measured as roughly three seconds of
	// blank window on real hardware before its contents appeared. A
	// smaller dialog took proportionally less, which is exactly the
	// signature of an area-proportional loop and how it was found.
	//
	// The clamp is not optional: z_fb_hw_fill_rect() clamps to the
	// SCREEN, not to this window, and takes no clip argument. Handing
	// it the oversized rectangle z_win_clear() passes would paint over
	// every other window on screen.
	int x0 = clip.x0 + x;
	int y0 = clip.y0 + y;
	int x1 = x0 + w - 1;
	int y1 = y0 + h - 1;

	if (x0 < clip.x0) x0 = clip.x0;
	if (y0 < clip.y0) y0 = clip.y0;
	if (x1 > clip.x1) x1 = clip.x1;
	if (y1 > clip.y1) y1 = clip.y1;

	if (x1 < x0 || y1 < y0) return;

	z_fb_hw_fill_rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, color);

}

void z_win_clear(const z_win_t *win) {

	win_use_clip(win);
	// oversized on purpose -- z_win_content_rect() (via
	// z_win_fill_rect's clip) cuts this down to the actual content
	// area regardless.
	z_win_fill_rect(win, 0, 0, win->w, win->h, 0);
}

void z_win_draw_text(const z_win_t *win, int x, int y, const char *s, int color, const z_font_t *font) {

	win_use_clip(win);

	z_clip_t clip;
	z_win_content_rect(win, &clip);

	// clip.x0 + x, NOT win->x + x.
	//
	// This function used to mix its origins: y was measured from the
	// content area's top edge (clip.y0) while x was measured from the
	// WINDOW's left edge, two pixels further left. zwin.h has always
	// documented both as content-relative, and everything else that
	// draws into a window -- z_win_fill_rect(), zwidget.c's own
	// widget_abs() -- uses the content rect for both.
	//
	// Two consequences, both of which looked like something else:
	// text drawn at x = 0 sat directly against the window frame,
	// defeating the entire purpose of the 2px inset
	// z_win_content_rect() applies (see its own comment on why that
	// margin exists); and a zwidget button's label was centered two
	// pixels to the left of the frame it was centered inside, since
	// the frame came from widget_abs() and the label came from here.
	z_fb_draw_text(clip.x0 + x, clip.y0 + y, s, color, font, &clip);

}

void z_win_draw_text2(const z_win_t *win, int x, int y, const char *s,
	int fg_color, int bg_color, const z_font_t *font) {

	win_use_clip(win);

	z_clip_t clip;
	z_win_content_rect(win, &clip);

	z_fb_draw_text2(clip.x0 + x, clip.y0 + y, s, fg_color, bg_color,
		font, &clip);

}

void z_win_hw_line(const z_win_t *win, int x0, int y0, int x1, int y1, int color) {

	win_use_clip(win);
	z_clip_t clip;
	z_win_content_rect(win, &clip);
	z_fb_hw_line(x0, y0, x1, y1, color, &clip);
}

// Shaded fill, clipped to the window's content area.
//
// The clip matters more here than for a line: a shaded SPAN is the
// primitive a software triangle rasterizer emits, and a rasterizer
// working from projected vertices will happily produce spans that run
// off the window when the model is scaled up or rotated near the edge.
void z_win_hw_fill_shade(const z_win_t *win, int x, int y, int w, int h,
	int level) {

	win_use_clip(win);
	z_clip_t clip;
	z_win_content_rect(win, &clip);
	/* z_clip_t is INCLUSIVE bounds (x0..x1), not origin-plus-size --
	 * so the right edge is x1 + 1, not x + w. Getting that wrong
	 * loses the last column of every clipped span, which on a shaded
	 * triangle shows up as a one-pixel notch down one side rather
	 * than as an obviously wrong rectangle. */
	if (x < clip.x0) { w -= (int)(clip.x0 - x); x = (int)clip.x0; }
	if (y < clip.y0) { h -= (int)(clip.y0 - y); y = (int)clip.y0; }
	if (x + w > (int)clip.x1 + 1) w = (int)clip.x1 + 1 - x;
	if (y + h > (int)clip.y1 + 1) h = (int)clip.y1 + 1 - y;
	if (w <= 0 || h <= 0) return;
	z_fb_hw_fill_shade_async(x, y, w, h, level);
}

void z_win_hw_box(const z_win_t *win, int x0, int y0, int x1, int y1, int color) {

	win_use_clip(win);
	z_clip_t clip;
	z_win_content_rect(win, &clip);
	z_fb_hw_box(x0, y0, x1, y1, color, &clip);
}

// -- launch arguments -- see zwin.h and Z_WM_SET_ARG in zwm.h --

// Static, for the same reason every other payload in this file is:
// the argument is borrowed until wm reads it, so it cannot be a
// caller's stack buffer and it cannot be malloc'd and freed.
static char launch_arg_buf[Z_WM_ARG_MAX];

void z_launch_arg_set(const char *arg) {

	uint32_t wmpid = resolve_wm_pid();
	if (!wmpid) return;

	int i = 0;
	if (arg)
		for (; i < Z_WM_ARG_MAX - 1 && arg[i]; i++)
			launch_arg_buf[i] = arg[i];
	launch_arg_buf[i] = 0;

	z_obj_t obj;
	obj.type = Z_STR;
	obj.val.str = launch_arg_buf;

	z_msg_new_send(wmpid, Z_WM_SET_ARG, 0, obj);

}

bool z_launch_arg_take(char *out, int outlen) {

	if (!out || outlen < 1) return false;

	out[0] = 0;

	uint32_t wmpid = resolve_wm_pid();
	if (!wmpid) return false;

	z_obj_t none;
	none.type = Z_NONE;
	z_msg_new_send(wmpid, Z_WM_GET_ARG, 0, none);

	z_msg_t reply;
	if (z_msg_wait(&reply, Z_WM_ARG, 0) != Z_OK) return false;

	if (reply.obj.type != Z_STR || !reply.obj.val.str) return false;

	int i = 0;
	for (; i < outlen - 1 && reply.obj.val.str[i]; i++)
		out[i] = reply.obj.val.str[i];
	out[i] = 0;

	return out[0] != 0;

}

// -- clipboard -- see zwin.h and Z_WM_CLIP_SET in zwm.h --

// Dropped entirely by --gc-sections in an app that never copies --
// see z_clip_set()'s comment in zwin.h.
static char clip_buf[Z_WM_CLIP_MAX];

void z_clip_set(const char *text, int len) {

	uint32_t wmpid = resolve_wm_pid();
	if (!wmpid) return;

	int i = 0;

	if (text) {
		for (; i < Z_WM_CLIP_MAX - 1; i++) {
			if (len >= 0 && i >= len) break;
			if (!text[i]) break;
			clip_buf[i] = text[i];
		}
	}

	clip_buf[i] = 0;

	z_obj_t obj;
	obj.type = Z_STR;
	obj.val.str = clip_buf;

	z_msg_new_send(wmpid, Z_WM_CLIP_SET, 0, obj);

}

int z_clip_get(char *out, int outlen) {

	if (!out || outlen < 1) return 0;

	out[0] = 0;

	uint32_t wmpid = resolve_wm_pid();
	if (!wmpid) return 0;

	z_obj_t none;
	none.type = Z_NONE;
	z_msg_new_send(wmpid, Z_WM_CLIP_GET, 0, none);

	z_msg_t reply;
	if (z_msg_wait(&reply, Z_WM_CLIP_DATA, 0) != Z_OK) return 0;

	if (reply.obj.type != Z_STR || !reply.obj.val.str) return 0;

	int i = 0;
	for (; i < outlen - 1 && reply.obj.val.str[i]; i++)
		out[i] = reply.obj.val.str[i];
	out[i] = 0;

	return i;

}

void z_win_set_title(const z_win_t *win, const char *title) {

	if (win->id < 0) return;

	uint32_t wmpid = resolve_wm_pid();
	if (!wmpid) return;

	// Static, for the same reason z_win_create_cb() is -- see its own
	// comment. This one matters more than it looks: an app that puts
	// its document name in the titlebar (sw/apps/text) calls this on
	// every open, every save, and on the first edit after each, and
	// the map it used to build was never freed.
	//
	// Two slots, unlike create's one: this is fire-and-forget, so the
	// payload is still borrowed when this returns and a second
	// retitle immediately afterwards would otherwise overwrite a
	// message wm hasn't read yet.
	int s = title_slot;
	title_slot = (title_slot + 1) % Z_WIN_TITLE_SLOTS;

	title_keys[s][0].type = Z_STR;
	title_keys[s][0].val.str = (char *)"id";
	title_vals[s][0].type = Z_INT32;
	title_vals[s][0].val.int32 = win->id;

	title_keys[s][1].type = Z_STR;
	title_keys[s][1].val.str = (char *)"title";
	title_vals[s][1].type = Z_STR;

	// copied -- the caller's buffer is very often a local
	{
		const char *src = title ? title : "";
		int i = 0;
		for (; i < (int)sizeof(title_text[s]) - 1 && src[i]; i++)
			title_text[s][i] = src[i];
		title_text[s][i] = 0;
	}
	title_vals[s][1].val.str = title_text[s];

	title_tbl[s].len = 2;
	title_tbl[s].a = title_keys[s];
	title_tbl[s].b = title_vals[s];

	z_obj_t args;
	args.type = Z_MAP;
	args.val.ptr = &title_tbl[s];

	z_msg_new_send(wmpid, Z_WM_SET_TITLE, 0, args);

}

// tells the wm to destroy this window -- fire-and-forget, no reply
// (see wm.c's own Z_WM_DESTROY_WINDOW handler: it repairs the screen
// region itself and doesn't send anything back). Safe to call even if
// win->id is already -1 (a failed z_win_create(), or a window that
// was never actually created) -- the wm just won't find a matching id
// and drops it, same as it already does for any unrecognized id.
void z_win_destroy(const z_win_t *win) {
	if (win->id < 0) return;
	uint32_t wmpid_d = resolve_wm_pid();
	if (wmpid_d)
		z_msg_new_send(wmpid_d, Z_WM_DESTROY_WINDOW, 0, z_obj_uint32((uint32_t)win->id));
}
