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
#include "zutf8.h"
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

// Last window this process created. z_launch_arg_take() used to
// drain the mailbox with z_msg_wait(), which discards everything
// that isn't Z_WM_ARG -- including the first SET_CLIP and REDRAW
// wm sends right after WINDOW_CREATED. While an app drew wherever it
// liked that was invisible: the app's own paint after take ran
// unrestricted. Now that a window paints only inside the region it
// has been given, it paints nothing, and because the region never arrives a later
// raise of an already-front window does not resend it either, so
// files/info/clock opened from the dock stay black. Applying those
// messages to this window here is the layer's job, not each app's.
static z_win_t *zwin_live;

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

// The "draw nothing" region: one empty rectangle. Zero rectangles
// would mean UNRESTRICTED to zgfx (see zgfx.c's essay) -- the exact
// opposite, and the same trap Z_WM_SET_CLIP's own comment documents.
static const z_clip_t zwin_clip_none = { 0, 0, -1, -1 };
// 64, matching WM_TITLE_MAX in sw/apps/wm/wm.c and create_title
// above.
//
// At 32 this silently cut "Wikipedia, the free encyclopedia" to
// "Wikipedia, the free encyclopedi" -- and raising the window
// manager's limit alone changed nothing, because the string was
// already truncated before it was ever sent. Three buffers carry a
// title between an app and the titlebar; all three have to agree.
static char title_text[Z_WIN_TITLE_SLOTS][64];
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

	// A windowed process is born invisible: until wm sends this window
	// its region (and win_use_clip() loads it), nothing it draws may
	// reach the glass. zgfx's start-up state is unrestricted, which is
	// correct for drawers that own the whole screen -- wm's chrome, a
	// game-mode framebuffer -- and wrong for a window whose first
	// SET_CLIP is still in flight: that paint used to land on whatever
	// was above it. Loading the empty rectangle here covers even draws
	// that bypass every z_win_* call and go straight to z_fb_*.
	z_gfx_set_visible(&zwin_clip_none, 1);
	win->clip[0] = zwin_clip_none;
	win->clip_n = 1;

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

	zwin_live = win;
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
	win->geom_changed = 1;

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
static int zwin_region_minus(const z_clip_t *a, int na,
	const z_clip_t *b, int nb, z_clip_t *out, int max);
static int zwin_region_and(const z_clip_t *a, int na,
	const z_clip_t *b, int nb, z_clip_t *out, int max);

static void win_use_clip(const z_win_t *win) {
	if (!win) {
		z_gfx_clear_visible();
		return;
	}
	// Frozen (Z_WM_CLIP_FREEZE): nothing reaches the glass, and the
	// attempt is remembered for the thaw. clip is the empty rectangle
	// while frozen, so the normal path below would also draw nothing;
	// this branch exists for the flag. The cast: the caller's const is
	// about the window's geometry, not about this bookkeeping.
	if (win->frozen) {
		((z_win_t *)win)->drew_frozen = 1;
		z_gfx_set_visible(win->clip, win->clip_n);
		return;
	}
	// A REDRAW that named its damage (Z_WM_REDRAW_DAMAGE, zwm.h):
	// only those pixels. The rest of the window is already on the
	// glass, and drawing it again would be the curtain.
	//
	// An empty damage list is honoured as EMPTY, not as "no
	// restriction": one empty rectangle, so an app that repaints
	// itself regardless reaches nothing. Passing zero rectangles to
	// zgfx would mean unrestricted, which is the opposite -- the same
	// trap Z_WM_SET_CLIP's own comment documents.
	if (win->paint_set) {
		if (win->paint_n > 0) z_gfx_set_visible(win->paint, win->paint_n);
		else z_gfx_set_visible(&zwin_clip_none, 1);
		return;
	}
	// Never told a region (clip_n == 0): draw NOTHING, not everything.
	// Unrestricted is the right default for a drawer that owns the
	// whole screen and never gets a region -- wm's chrome, a game-mode
	// framebuffer -- but a window's first SET_CLIP can still be in
	// flight while it paints, and unrestricted then means painting over
	// every window above it. wm sends the region ahead of the first
	// REDRAW, so nothing is lost by staying invisible until it arrives.
	if (win->clip_n <= 0) z_gfx_set_visible(&zwin_clip_none, 1);
	else z_gfx_set_visible(win->clip, win->clip_n);
}

bool z_win_apply_clip(z_win_t *win, z_obj_t *obj) {

	if (!obj || obj->type != Z_BLOB) return false;

	uint32_t len = z_blob_len(obj);
	const z_wm_cliprect_t *r = z_blob_data(obj);

	if (!r || (len % sizeof(z_wm_cliprect_t)) != 0) return false;

	int n = (int)(len / sizeof(z_wm_cliprect_t));

	/* Whose region is this? (Z_WM_CLIP_WINDOW, zwm.h)
	 *
	 * Messages arrive per PROCESS, and a process with a dialog owns
	 * two windows. wm names the window in a leading control rectangle;
	 * one that is not ours belongs to the other window and must be
	 * left for whoever holds it -- returning false without acking, so
	 * the caller forwards it rather than this window swallowing a
	 * region wm is waiting on an ack for.
	 *
	 * A payload without the leading rectangle is taken as ours: that
	 * is what every message looked like before this existed, and a
	 * single-window app is the case that cannot be ambiguous. */
	if (n > 0 && r[0].x0 == Z_WM_CLIP_CTL && r[0].y0 == Z_WM_CLIP_WINDOW) {
		if (win && (int)r[0].y1 != win->id) return false;
		r++;
		n--;
	}

	if (n > Z_WM_MAX_CLIP) n = Z_WM_MAX_CLIP;

	z_clip_t rects[Z_WM_MAX_CLIP];
	for (int i = 0; i < n; i++) {
		rects[i].x0 = r[i].x0;
		rects[i].y0 = r[i].y0;
		rects[i].x1 = r[i].x1;
		rects[i].y1 = r[i].y1;
	}

	// A control region (zwm.h). FREEZE: stop drawing, keep the glass.
	// The clip becomes the empty rectangle with NONE of the clearing a
	// narrowing does below -- what is on screen stays, by design: it
	// is the still image under wm's drag band. clip_was is held
	// exactly as a narrowing holds it, so the thawing region's REDRAW
	// paints newly visible pixels against the last PAINT, not against
	// the frozen empty clip. Idempotent. Acked like any region.
	if (n == 1 && r[0].x0 == Z_WM_CLIP_CTL) {
		if (win && r[0].y0 == Z_WM_CLIP_FREEZE) {
			// clip_was is deliberately NOT intersected with the
			// empty freeze clip: nothing left the glass, the window
			// is simply not allowed to draw. Intersecting would say
			// every pixel is stale and make the thaw repaint the
			// whole desktop.
			if (!win->clip_was_held) {
				win->clip_was_n = win->clip_n;
				for (int i = 0; i < win->clip_n; i++)
					win->clip_was[i] = win->clip[i];
				win->clip_was_held = 1;
			}
			win->clip[0].x0 = 0;
			win->clip[0].y0 = 0;
			win->clip[0].x1 = -1;
			win->clip[0].y1 = -1;
			win->clip_n = 1;
			win->paint_n = 0;
			win->paint_set = 0;
			win->frozen = 1;
			win->drew_frozen = 0;
		}
		uint32_t wmpid_c = resolve_wm_pid();
		if (wmpid_c)
			z_msg_new_send(wmpid_c, Z_WM_CLIP_DONE, 0,
				z_obj_uint32(win ? (uint32_t)win->id : 0));
		return true;
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
	uint32_t ack_flags = 0;
	if (win) {
		// A real region thaws a frozen window. Its clip is the empty
		// rectangle, so nothing below is cleared: the pixels it shows
		// now belong to whoever gained them, and THEY repaint. If it
		// tried to draw while frozen, say so in the ack.
		int thawed_dirty = 0;
		if (win->frozen) {
			win->frozen = 0;
			if (win->drew_frozen) {
				ack_flags |= Z_WM_CLIP_DONE_DREW;
				thawed_dirty = 1;
			}
			win->drew_frozen = 0;
		}
		// The damage of the last redraw is over; anything drawn from
		// here (whatever the app does next) is against the window's
		// own region again.
		win->paint_n = 0;
		win->paint_set = 0;

		// The pixels this window is giving up are NOT erased here.
		// wm repairs the overlap itself on every path that narrows a
		// region (wipe_raise_overlap(), repair_region(), the drag
		// sweep), so the erase only duplicated that work -- and it
		// did it with the region deliberately escaped
		// (z_gfx_clear_visible() + raw z_fb_hw_fill_rect()) from the
		// app's own message loop, whenever the app happened to drain
		// the message. A busy app applies the narrowing LATE: the
		// erase then lands after wm's repair, on pixels that already
		// belong to the window that was raised, and blacks its fresh
		// chrome. Rapid focus cycling makes that systematic: measured,
		// 100-150 ms clicks between three windows reliably killed
		// the raised window's titlebar until it was raised again.
		// clip_was is what is KNOWN GOOD on the glass, and a region
		// change can only take away from it: the pixels we are
		// giving up stop being ours, and the ones we are gaining
		// have never been painted. So intersect, do not snapshot.
		//
		// Snapshotting the old clip on the first change since the
		// last paint was right only while EVERY region change came
		// with a REDRAW, because the redraw's ack reset it. Now that
		// a window which merely narrows is not asked to repaint
		// (wm's send_clip_ex), that reset never comes: a window
		// lowered and then raised again compared its new full region
		// against the full region it had BEFORE being lowered, got
		// an empty damage, and painted nothing at all -- the raised
		// terminal came back blank. Intersecting gets it right by
		// construction, and coalesced regions accumulate the damage
		// the way the snapshot was meant to.
		int i;
		uint32_t area_was = 0, area_now = 0;
		if (!win->clip_was_held) {
			// Never told a region: nothing on the glass is ours yet.
			win->clip_was_n = 0;
			win->clip_was_held = 1;
		} else {
			z_clip_t keep[Z_WM_MAX_CLIP];
			int nk = zwin_region_and(win->clip_was, win->clip_was_n,
				rects, n, keep, Z_WM_MAX_CLIP);
			if (nk < 0) nk = 0;   // too many pieces: repaint it all
			for (i = 0; i < nk; i++) win->clip_was[i] = keep[i];
			win->clip_was_n = nk;
		}
		for (i = 0; i < win->clip_was_n; i++) {
			int w = win->clip_was[i].x1 - win->clip_was[i].x0 + 1;
			int h = win->clip_was[i].y1 - win->clip_was[i].y0 + 1;
			if (w > 0 && h > 0)
				area_was += (uint32_t)w * (uint32_t)h;
		}
		for (i = 0; i < n; i++) {
			win->clip[i] = rects[i];
			int w = rects[i].x1 - rects[i].x0 + 1;
			int h = rects[i].y1 - rects[i].y0 + 1;
			if (w > 0 && h > 0)
				area_now += (uint32_t)w * (uint32_t)h;
		}
		win->clip_n = n;
		win->clip_widened = (area_now > area_was);

		// A window that tried to draw while frozen believes pixels
		// are on the glass that never got there. wm answers the DREW
		// ack with a REDRAW; drop clip_was so that REDRAW is a full
		// one. Without this the damage would work out empty -- the
		// thawing region is the pre-freeze region -- and the window
		// would repaint nothing at all.
		if (thawed_dirty)
			win->clip_was_n = 0;
	}

	uint32_t wmpid = resolve_wm_pid();
	if (wmpid)
		z_msg_new_send(wmpid, Z_WM_CLIP_DONE, 0,
			z_obj_uint32((win ? (uint32_t)win->id : 0) | ack_flags));

	return true;

}

static bool zwin_rect_empty(const z_clip_t *r) {
	return r->x1 < r->x0 || r->y1 < r->y0;
}

static bool zwin_rect_overlaps(const z_clip_t *a, const z_clip_t *b) {
	return !(a->x1 < b->x0 || b->x1 < a->x0 ||
		 a->y1 < b->y0 || b->y1 < a->y0);
}

static bool zwin_rect_subtract(const z_clip_t *r, const z_clip_t *cut,
	z_clip_t *out, int *n, int max)
{
	if (!zwin_rect_overlaps(r, cut)) {
		if (*n >= max) return false;
		out[(*n)++] = *r;
		return true;
	}
	if (cut->y0 > r->y0) {
		if (*n >= max) return false;
		out[(*n)++] = (z_clip_t){ r->x0, r->y0, r->x1, cut->y0 - 1 };
	}
	if (cut->y1 < r->y1) {
		if (*n >= max) return false;
		out[(*n)++] = (z_clip_t){ r->x0, cut->y1 + 1, r->x1, r->y1 };
	}
	{
		int ty0 = cut->y0 > r->y0 ? cut->y0 : r->y0;
		int ty1 = cut->y1 < r->y1 ? cut->y1 : r->y1;
		if (cut->x0 > r->x0 && ty0 <= ty1) {
			if (*n >= max) return false;
			out[(*n)++] = (z_clip_t){ r->x0, ty0, cut->x0 - 1, ty1 };
		}
		if (cut->x1 < r->x1 && ty0 <= ty1) {
			if (*n >= max) return false;
			out[(*n)++] = (z_clip_t){ cut->x1 + 1, ty0, r->x1, ty1 };
		}
	}
	return true;
}

// Pixels in both `a` and `b`. Returns -1 if the result needs more
// than `max` rectangles; the caller treats that as "nothing", which
// costs a repaint and never a hole.
static int zwin_region_and(const z_clip_t *a, int na,
	const z_clip_t *b, int nb, z_clip_t *out, int max)
{
	int i, j, n = 0;
	for (i = 0; i < na; i++) {
		if (zwin_rect_empty(&a[i])) continue;
		for (j = 0; j < nb; j++) {
			z_clip_t r;
			if (zwin_rect_empty(&b[j])) continue;
			r.x0 = a[i].x0 > b[j].x0 ? a[i].x0 : b[j].x0;
			r.y0 = a[i].y0 > b[j].y0 ? a[i].y0 : b[j].y0;
			r.x1 = a[i].x1 < b[j].x1 ? a[i].x1 : b[j].x1;
			r.y1 = a[i].y1 < b[j].y1 ? a[i].y1 : b[j].y1;
			if (zwin_rect_empty(&r)) continue;
			if (n >= max) return -1;
			out[n++] = r;
		}
	}
	return n;
}

static int zwin_region_minus(const z_clip_t *a, int na,
	const z_clip_t *b, int nb, z_clip_t *out, int max)
{
	z_clip_t cur[Z_WM_MAX_CLIP], nxt[Z_WM_MAX_CLIP];
	int ncur = 0, i, j;

	for (i = 0; i < na && ncur < Z_WM_MAX_CLIP; i++)
		if (!zwin_rect_empty(&a[i]))
			cur[ncur++] = a[i];

	for (i = 0; i < nb; i++) {
		int nnxt = 0;
		if (zwin_rect_empty(&b[i])) continue;
		for (j = 0; j < ncur; j++) {
			if (!zwin_rect_subtract(&cur[j], &b[i], nxt, &nnxt,
					Z_WM_MAX_CLIP))
				return -1;
		}
		for (j = 0; j < nnxt; j++) cur[j] = nxt[j];
		ncur = nnxt;
		if (ncur == 0) break;
	}

	int n = 0;
	for (i = 0; i < ncur; i++) {
		if (zwin_rect_empty(&cur[i])) continue;
		if (n >= max) return -1;
		out[n++] = cur[i];
	}
	return n;
}

void z_win_apply_redraw(z_win_t *win, uint32_t packed) {
	win->x = Z_WM_UNPACK_X(packed);
	win->y = Z_WM_UNPACK_Y(packed);
	win->paint_n = 0;
	win->paint_set = 0;

	// Drain compositor messages BEFORE computing damage, so the
	// clip in force is the newest one. Draining after left paint[]
	// describing the old (wider) region: view's PNG decode is the
	// measured case -- create queued SET_CLIP(full)+REDRAW, files
	// was raised during the decode, a narrowing SET_CLIP sat
	// behind the REDRAW, and win_use_clip then loaded that stale
	// full damage, so the blit landed on files. Anything that is
	// not a compositor message is pushed back.
	{
		z_msg_t extra;
		while (z_msg_read(&extra) == Z_OK) {
			if (extra.subject == Z_WM_SET_CLIP) {
				// Another window's region (a dialog's, or its
				// parent's -- Z_WM_CLIP_WINDOW in zwm.h) is not
				// ours to swallow: wm is waiting on an ack for it.
				// Push it back, like anything else that is not
				// addressed here.
				if (z_win_apply_clip(win, &extra.obj)) continue;
				z_msg_unread(&extra);
				break;
			}
			if (extra.subject == Z_WM_WINDOW_MOVED) {
				z_win_parse_rect(win, &extra.obj);
				continue;
			}
			if (extra.subject == Z_WM_WINDOW_RESIZED) {
				z_win_apply_resized(win, &extra.obj);
				continue;
			}
			if (extra.subject == Z_WM_REDRAW &&
			    extra.obj.type == Z_UINT32 &&
			    z_win_redraw_id(extra.obj.val.uint32) == win->id) {
				// A later full REDRAW upgrades this one:
				// damage computed against the newest clip
				// would otherwise throw the full request
				// away (the previous drain skipped it).
				if (!(extra.obj.val.uint32 & Z_WM_REDRAW_DAMAGE))
					packed &= ~Z_WM_REDRAW_DAMAGE;
				continue;
			}
			z_msg_unread(&extra);
			break;
		}
	}

	// No Z_WM_REDRAW_DAMAGE: wm means "everything you can see".
	// It cleared pixels itself (repair_region(), Z_WM_REPAINT), or
	// this window drew where nothing reached the glass. Neither is
	// expressible as "what your region gained" -- the region often
	// did not change at all -- so the honest answer is a full
	// repaint, and it is what every sender before this flag meant.
	if (!(packed & Z_WM_REDRAW_DAMAGE)) {
		win->paint_full = 1;
		return;
	}

	// A full redraw is already outstanding: it cannot be narrowed by
	// one that arrived after it.
	if (win->paint_full)
		return;

	// A move or a resize: clip_was is in the OLD screen coordinates,
	// so subtracting it from the new clip skips the wrong pixels --
	// the terminal goes illegible. The whole window is damaged.
	if (win->geom_changed) {
		win->geom_changed = 0;
		win->paint_full = 1;
		return;
	}

	// Never told a region, so there is no "before" to compare
	// against. Repaint everything rather than guess.
	if (!win->clip_was_held) {
		win->paint_full = 1;
		return;
	}

	{
		int pn;
		// Damage = newly visible pixels. Overflow (too many
		// fragments) used to return a PARTIAL list -- the
		// missing pieces stayed black, and whether that
		// happened depended on how the windows overlapped
		// ("a veces sale, a veces no"). Fall back to a full
		// repaint of the current clip so the landing cannot
		// punch holes.
		pn = zwin_region_minus(win->clip, win->clip_n,
			win->clip_was, win->clip_was_n, win->paint,
			Z_WM_MAX_CLIP);
		if (pn < 0) {
			win->paint_full = 1;
			return;
		}
		win->paint_n = pn;
		win->paint_set = 1;
	}
}

void z_win_damage_ignore(z_win_t *win) {
	if (!win) return;
	win->paint_n = 0;
	win->paint_set = 0;
	win->paint_full = 1;
}

// See zwin.h: "am I allowed to put anything on the glass right now?".
// Deliberately reads the flag rather than testing the clip for
// emptiness -- a fully occluded window has an empty clip too, and that
// is a different situation (its pixels belong to someone else, and a
// REDRAW will come when it gets them back).
bool z_win_frozen(const z_win_t *win) {
	return win && win->frozen;
}

int z_win_damage_rects(const z_win_t *win, z_clip_t *out, int max) {
	int i, n;
	if (!win || !win->paint_set) return -1;
	n = win->paint_n;
	if (n > max) n = max;
	for (i = 0; i < n; i++) out[i] = win->paint[i];
	return n;
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

void z_win_redraw_done(z_win_t *win) {
	if (win) {
		int i;
		win->paint_n = 0;
		win->paint_set = 0;
		win->paint_full = 0;
		// The window has just painted everything that was asked of
		// it, so its whole clip is now known good on the glass. Next
		// time, the damage is measured from here.
		for (i = 0; i < win->clip_n; i++)
			win->clip_was[i] = win->clip[i];
		win->clip_was_n = win->clip_n;
		win->clip_was_held = 1;
	}
	uint32_t wmpid_r = resolve_wm_pid();
	if (wmpid_r)
		z_msg_new_send(wmpid_r, Z_WM_REDRAW_DONE, 0,
			z_obj_uint32(win ? (uint32_t)win->id : 0));
}

static z_clip_t paint_content;

void z_win_paint_begin(const z_win_t *win)
{
	z_win_content_rect(win, &paint_content);
	z_gfx_paint_begin();
}

int z_win_paint_next_rect(void)
{
	return z_gfx_paint_next_rect(&paint_content);
}

int z_win_paint_current(z_clip_t *out)
{
	return z_gfx_paint_current(out);
}

void z_win_paint_end(const z_win_t *win)
{
	(void)win;
	z_gfx_paint_end();
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

void z_win_resize(const z_win_t *win, uint32_t w, uint32_t h) {

	if (win->id < 0) return;

	uint32_t wmpid = resolve_wm_pid();
	if (!wmpid) return;

	// A packed word, not a map: nothing borrowed, nothing to keep alive
	// until wm reads it.
	z_msg_new_send(wmpid, Z_WM_RESIZE, 0,
		z_obj_uint32(Z_WM_PACK_RESIZE(win->id, w, h)));

}

void z_win_set_max_size(const z_win_t *win, uint32_t w, uint32_t h) {

	if (win->id < 0) return;

	uint32_t wmpid = resolve_wm_pid();
	if (!wmpid) return;

	z_msg_new_send(wmpid, Z_WM_SET_LIMITS, 0,
		z_obj_uint32(Z_WM_PACK_RESIZE(win->id, w, h)));

}

void z_win_doc_title(char *t, int cap, const char *path, bool modified,
	const char *untitled) {

	if (cap <= 0) return;

	const char *base = (path && path[0]) ? path : untitled;
	if (!base) base = "";
	for (const char *p = base; *p; p++)
		if (*p == '/') base = p + 1;

	int n = 0;
	if (modified && cap > 1) t[n++] = '*';
	z_utf8_copy(t + n, (size_t)(cap - n), base);

}

// UTF-8 forms of the two above -- see z_fb_draw_utf8() in zgfx.c.
void z_win_draw_utf8(const z_win_t *win, int x, int y, const char *s, int color, const z_font_t *font) {

	win_use_clip(win);

	z_clip_t clip;
	z_win_content_rect(win, &clip);

	z_fb_draw_utf8(clip.x0 + x, clip.y0 + y, s, color, font, &clip);

}

void z_win_draw_utf8_2(const z_win_t *win, int x, int y, const char *s,
	int fg_color, int bg_color, const z_font_t *font) {

	win_use_clip(win);

	z_clip_t clip;
	z_win_content_rect(win, &clip);

	z_fb_draw_utf8_2(clip.x0 + x, clip.y0 + y, s, fg_color, bg_color,
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

// Apply a compositor message to `win` so a wait for something else
// (the launch argument, a clipboard fetch) cannot throw away the
// first region. Returns true if the message was consumed.
static bool zwin_absorb(z_win_t *win, z_msg_t *msg) {

	if (!win || !msg) return false;

	switch (msg->subject) {

	case Z_WM_SET_CLIP:
		return z_win_apply_clip(win, &msg->obj);

	case Z_WM_WINDOW_MOVED:
		return z_win_parse_rect(win, &msg->obj);

	case Z_WM_WINDOW_RESIZED:
		return z_win_apply_resized(win, &msg->obj);

	case Z_WM_REDRAW:
		if (msg->obj.type != Z_UINT32) return false;
		if (z_win_redraw_id(msg->obj.val.uint32) != win->id)
			return false;
		z_win_apply_redraw(win, msg->obj.val.uint32);
		return true;

	default:
		return false;

	}

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
	for (;;) {
		if (z_msg_read(&reply) != Z_OK) {
			z_proc_wait(1);
			continue;
		}
		if (reply.subject == Z_WM_ARG && reply.tag == 0)
			break;
		zwin_absorb(zwin_live, &reply);
	}

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
	if (zwin_live == win) zwin_live = NULL;
	uint32_t wmpid_d = resolve_wm_pid();
	if (wmpid_d)
		z_msg_new_send(wmpid_d, Z_WM_DESTROY_WINDOW, 0, z_obj_uint32((uint32_t)win->id));
}
