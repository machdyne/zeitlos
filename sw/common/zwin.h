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
#include "zwm.h"		// Z_WM_MAX_CLIP, for the per-window region below

typedef struct {
	int32_t		id;	// -1 if creation failed
	uint32_t	x, y, w, h;

	// The part of this window not covered by the windows in front of
	// it, as last told to us by wm (Z_WM_SET_CLIP).
	//
	// PER WINDOW, not per process. zgfx's visible region is one
	// process-global thing -- there is one framebuffer and one
	// hardware scissor -- but an app owns MORE THAN ONE WINDOW as
	// soon as it opens a dialog, and the two have different visible
	// regions. Storing it globally meant whichever region arrived
	// last won, so a dialog drew against its parent's region: its
	// button outlines and list-box frame sat outside that region and
	// were clipped away.
	//
	// Every z_win_* drawing call loads this into zgfx before it
	// draws, so the region in force always belongs to the window
	// being drawn.
	//
	// clip_n == 0 means "not yet told", which zgfx reads as
	// unrestricted -- the right default before wm has said anything.
	z_clip_t	clip[Z_WM_MAX_CLIP];
	int			clip_n;
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

// like z_win_create_ex(), but also takes a Z_WIN_FLAG_* bitmask
// (sw/common/zwm.h) -- currently Z_WIN_FLAG_CLOSE_ICON and
// Z_WIN_FLAG_CLOSE_KILLS_OWNER, controlling whether wm draws a
// titlebar close icon for this window at all, and what clicking it
// does (see those flags' own comments in zwm.h for the full
// reasoning, especially around apps -- like repl's Scheme
// `win-create` -- that can own more than one window at a time).
// z_win_create()/z_win_create_ex() both just call this with flags=0
// (no close icon), so every existing caller is unaffected -- this is
// the one that actually sends "flags" on the wire; the other two
// exist only for source compatibility with code written before this
// parameter existed.
z_rv z_win_create_flags(z_win_t *win, const char *title, uint32_t w, uint32_t h,
	int32_t x, int32_t y, uint32_t flags);

// callback shape for z_win_create_cb() below
typedef void (*z_win_msg_cb)(z_msg_t *msg, void *user);

// like z_win_create_flags(), but hands every message that ISN'T the
// creation reply to `cb` instead of throwing it away.
//
// That difference matters more than it sounds like. All three
// functions above block on z_msg_wait() (zeitlos.h), and z_msg_wait()
// DISCARDS anything that doesn't match the subject it's waiting for.
// For an app creating its one window at startup that's harmless --
// nothing else is in flight yet. For an app creating a SECOND window
// while already running, it is not: a Z_WM_REDRAW sitting in the
// queue at that moment gets silently dropped, and wm is left waiting
// for an ack that will never come until REDRAW_ACK_TIMEOUT fires
// (wm.c). The visible symptom is the whole screen freezing for a
// moment and "wm: timed out waiting for pid N to ack a redraw" on the
// console, which looks nothing like "somebody opened a dialog".
//
// So: any app that creates a window after startup should use this
// one, with a callback that at minimum services Z_WM_REDRAW and acks
// it. sw/common/zdialog.c does exactly that, and is the reason this
// exists.
//
// `cb` may be NULL, in which case this behaves exactly like
// z_win_create_flags() (and has the same hazard).
z_rv z_win_create_cb(z_win_t *win, const char *title, uint32_t w, uint32_t h,
	int32_t x, int32_t y, uint32_t flags, z_win_msg_cb cb, void *user);

// the window id carried in a Z_WM_REDRAW payload.
//
// Z_WM_REDRAW is packed with Z_WM_PACK_XY(id, x, y) (zwm.h), so the
// id has always been in there -- z_win_apply_redraw() above just
// doesn't look at it, because an app with a single window has no use
// for it. An app with several (one main window plus a dialog, say)
// very much does: it's the only way to tell which window is being
// asked to repaint, since the payload is otherwise just coordinates.
//
// Exposed as a function rather than an inline macro so zwin.h doesn't
// have to include zwm.h, which it currently doesn't and which several
// of its users don't want pulled in transitively.
int z_win_redraw_id(uint32_t packed);

// parses a Z_MAP{id,x,y,w,h} object (as sent with Z_WM_WINDOW_CREATED
// or Z_WM_WINDOW_MOVED) into *win. returns false if obj doesn't have
// the expected shape.
bool z_win_parse_rect(z_win_t *win, z_obj_t *obj);

// applies a Z_WM_REDRAW payload (a packed Z_UINT32, not a Z_MAP --
// see zwm.h) to *win, updating x/y only. w/h are left untouched (they
// don't change -- there's no resize support yet).
// Applies a Z_WM_SET_CLIP and acknowledges it. Call from the app's
// message loop:
//
//     case Z_WM_SET_CLIP:
//         z_win_apply_clip(&win, &msg.obj);
//         break;
//
// The ack is not optional -- wm waits for it when a region narrows.
// See the definition in zwin.c.
bool z_win_apply_clip(z_win_t *win, z_obj_t *obj);

void z_win_apply_redraw(z_win_t *win, uint32_t packed);

// applies a Z_WM_WINDOW_RESIZED payload to *win. That message carries
// the same Z_MAP{id,x,y,w,h} shape as Z_WM_WINDOW_CREATED, so this is
// just z_win_parse_rect() under a name that says what it's for at the
// call site -- there is deliberately no second parsing path.
//
// Note this updates w/h as well as x/y, which is exactly what
// z_win_apply_redraw() above does NOT do (Z_WM_REDRAW has no room for
// them). An app that handles resizing must handle BOTH messages: this
// one to learn its new size, and the redraw that follows to know when
// to repaint at it.
bool z_win_apply_resized(z_win_t *win, z_obj_t *obj);

// converts a Z_WM_MOUSE payload's absolute screen coordinates into
// coordinates relative to the top-left of this window's CONTENT area
// -- i.e. the same origin z_win_draw_text()/z_win_fill_rect() use, so
// a hit test and the drawing that responds to it are expressed in one
// coordinate system rather than two.
//
// Writes *cx/*cy unconditionally (they can legitimately go negative,
// or past the content area's width/height, while a capture is active
// and the cursor has left the window -- see Z_WM_MOUSE in zwm.h).
// RETURNS whether the point is actually within the content area, so
// the common "ignore this unless it's over my content" case stays a
// single call.
bool z_win_mouse_content_xy(const z_win_t *win, uint32_t packed, int *cx, int *cy);

// width/height of the window's content area, in pixels -- what an app
// laying out its own furniture actually needs, as opposed to win->w/h
// which include the frame and titlebar. Derived from
// z_win_content_rect() so it can't drift from what the drawing calls
// clip to.
int z_win_content_w(const z_win_t *win);
int z_win_content_h(const z_win_t *win);

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
// Content-area rect, in screen coordinates.
//
// ALSO loads this window's visible region into zgfx, so that drawing
// straight to z_fb_* with the rect returned here is clipped to the
// part of the window actually on screen. See zwin.c for why this
// chokepoint carries that rather than every app remembering to.
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

// Shaded fill (zgfx.h's z_fb_hw_fill_shade), clipped to the window's
// content area. Asynchronous -- call z_fb_hw_sync() before reading the
// affected pixels with the CPU.
//
// One call per scanline is how a software triangle rasterizer draws a
// flat-shaded face, which is why this exists.
void z_win_hw_fill_shade(const z_win_t *win, int x, int y, int w, int h,
	int level);
void z_win_hw_box(const z_win_t *win, int x0, int y0, int x1, int y1, int color);

// changes this window's titlebar text (fire-and-forget, no reply --
// see Z_WM_SET_TITLE in zwm.h). Safe to call on a window that failed
// to be created (id < 0), same as z_win_destroy().
//
// Only the owner may retitle a window, so this always refers to
// `win` -- there is deliberately no "retitle some other window id"
// form.
void z_win_set_title(const z_win_t *win, const char *title);

// -- launch arguments --
//
// See Z_WM_SET_ARG in zwm.h for the protocol and why the pending
// argument lives in wm rather than in the process table.
//
// These live in zwin.c, next to the wm pid cache every other message
// helper here already uses, even though neither has anything to do
// with windows. Splitting them into their own object would add a file
// to every app Makefile for two functions.

// Sets the argument the NEXT process launched should pick up. Call
// this immediately before z_proc_run() (zeitlos.h).
//
// Fire-and-forget. It does not name a target process, and it cannot:
// z_proc_run() hasn't been called yet, so there is no pid. That is
// what makes claiming destructive and time-limited on wm's side.
void z_launch_arg_set(const char *arg);

// Claims the pending launch argument, if there is one, and writes it
// into `out`. Returns true if an argument was claimed, false if there
// was none (or wm isn't running).
//
// Call once, EARLY -- before creating a window. This blocks on wm's
// reply via z_msg_wait(), which discards anything else that arrives
// meanwhile; at startup nothing else is in flight yet, which is
// exactly why this is safe there and would not be later.
bool z_launch_arg_take(char *out, int outlen);

// -- clipboard --
//
// See Z_WM_CLIP_SET in zwm.h for the protocol and why the clipboard
// lives in wm. Same reasoning as the launch-argument helpers above
// for why these live in zwin.c despite having nothing to do with
// windows.

// Stores `len` bytes of `text` as the system clipboard, replacing
// whatever was there. `len` of -1 means "up to the NUL".
//
// The text is copied into a staging buffer here before sending,
// because the payload is borrowed by wm until it reads the message
// (docs/messaging.md) and the caller's own buffer -- typically a
// slice of a document that is about to be edited -- cannot be
// promised to sit still that long.
//
// That staging buffer is Z_WM_CLIP_MAX bytes of .bss, and it is why
// this is a separate function rather than inlined: an app that never
// copies never references it, and --gc-sections drops both. Only apps
// that actually use the clipboard pay for it.
//
// Truncated at Z_WM_CLIP_MAX-1 bytes. Fire-and-forget; there is no
// confirmation and nothing to wait for.
void z_clip_set(const char *text, int len);

// Fetches the system clipboard into `out`, NUL-terminated. Returns
// the number of bytes written, or 0 if the clipboard is empty or wm
// isn't running.
//
// BLOCKS on wm's reply via z_msg_wait(), which discards anything else
// that arrives meanwhile. Call it from a key or click handler, having
// just drained the queue -- the same constraint z_launch_arg_take()
// documents, for the same reason.
int z_clip_get(char *out, int outlen);

// tells the wm to destroy this window (fire-and-forget, no reply --
// see zwin.c's own comment). Not previously exposed as a client
// helper -- only the raw Z_WM_DESTROY_WINDOW message subject existed
// (zwm.h) -- added for the Zeitlos Scheme API's (win-destroy id),
// docs/scheme_api.md.
void z_win_destroy(const z_win_t *win);

#endif
