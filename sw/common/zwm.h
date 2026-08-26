#ifndef ZWM_H
#define ZWM_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Window manager protocol -- shared between the WM (sw/apps/wm) and
 * any app that wants a window. See docs/window_manager.md.
 */

// the window manager's well-known pid. sh.c (the kernel shell) runs
// as pid 0; starting the WM next (before any client apps) reserves
// it pid 1 -- same convention the ping/pong messaging demo used for
// pong. there's no dynamic discovery yet (see docs/messaging.md),
// so this is a hard assumption until a real registry exists.
#define Z_PID_WM   1

// height, in pixels, of the titlebar area at the top of every window
// -- shared between wm.c (which draws it) and zwin.c (which uses it
// to compute where a window's content area starts). keep these in
// sync if you change the window chrome.
//
// NOTE on the "12 pixels" this used to be, and why it's 11 now: the
// titlebar separator line (wm.c's draw_window_box()) is drawn AT row
// Z_WM_TITLEBAR_H, i.e. Z_WM_TITLEBAR_H rows exist above it -- but
// row 0 of those is the window's own TOP BORDER (part of the outer
// box, drawn by the same function), not titlebar interior. So the
// space actually available for titlebar content (title text, the
// close icon -- wm.c's draw_titlebar_content()) is Z_WM_TITLEBAR_H-1
// rows, not Z_WM_TITLEBAR_H -- easy to miss since nothing enforced
// this distinction until vertically centering an icon made a
// mis-sized interior visible as an off-by-one (the icon sat ~1px
// higher than centered). 11 makes that interior exactly 10 rows,
// which centers an 8px icon (Z_ICON_H, zicon.h) or 8px-tall text
// (z_font_5x8.h) with an even, symmetric 1px gap on both sides --
// see wm.c's own Z_WM_TITLEBAR_CONTENT_H/titlebar_content_y().
#define Z_WM_TITLEBAR_H   11

// -- message subjects --

// app -> wm request: obj is a Z_MAP with optional "title" (Z_STR),
// "w" (Z_UINT32), "h" (Z_UINT32), "x"/"y" (Z_UINT32, both or
// neither -- see zwin.c's z_win_create_ex()), and "flags" (Z_UINT32,
// Z_WIN_FLAG_* below) keys. any missing key falls back to a
// WM-chosen default (no exact placement, flags=0 -- no close icon).
#define Z_WM_CREATE_WINDOW    100

// -- window flags (the "flags" key on Z_WM_CREATE_WINDOW) --
//
// see zwin.h's z_win_create_flags() for the app-facing entry point,
// and wm.c's draw_titlebar_content()/handle_close_click() for how wm
// itself interprets these.

// show a small hollow-box close icon on the right side of this
// window's titlebar (see sw/apps/wm/win_icons.h -- Z_ICON_CLOSE).
// Ignored on a no-titlebar window (there's no titlebar to put it in --
// see wm.c's own `no_titlebar`, currently only the dock). With this
// bit clear (the default -- every existing caller that predates this
// flag), no icon is drawn and the titlebar can't be clicked closed at
// all, same as before this feature existed.
#define Z_WIN_FLAG_CLOSE_ICON         (1u << 0)

// what clicking the close icon actually does -- meaningless without
// Z_WIN_FLAG_CLOSE_ICON also set.
//
//   clear (default): wm sends Z_WM_CLOSE (below) to the window's
//   owner and otherwise does nothing -- the window stays open, and
//   interactive, until/unless the owner itself calls z_win_destroy()
//   on it (zwin.h). This is the right choice for any app that can own
//   MORE THAN ONE window at a time (e.g. repl's Scheme `win-create`,
//   docs/scheme_api.md) -- clicking one sub-window's close icon must
//   not take the others, or the owning process itself, down with it.
//
//   set: wm destroys this window itself AND kills the owning process
//   outright (z_proc_kill(), zeitlos.h) -- no message round trip, no
//   chance for the app to ignore it. Only appropriate for an app that
//   owns exactly one window for its entire lifetime (term, hello_win,
//   gpu3d, gpudemo, ...) -- setting this on one of SEVERAL windows
//   sharing a pid takes every one of that pid's windows down the
//   instant any single one's close icon is clicked, not just the one
//   that was actually clicked.
#define Z_WIN_FLAG_CLOSE_KILLS_OWNER  (1u << 1)

// this window can be resized by dragging its lower-right corner (see
// wm.c's hit_resize_grip()/draw_resize_wire()). Ignored on a
// no-titlebar window, same as the close-icon flags -- the dock is a
// fixed-size fixture, not something the user resizes.
//
// With this bit clear (the default -- every caller that predates this
// flag) the window is exactly as fixed-size as it always was, and the
// corner is a normal part of the frame with no special hit testing at
// all.
//
// Resizing is a WIREFRAME operation, like dragging already is: only
// the prospective bottom and right edges are drawn while the mouse is
// down (an L, not a full box -- see draw_resize_wire()), so it reads
// visually as "you are moving these two edges" rather than "you are
// moving the whole window", which is what a full-frame highlight
// already means during a drag. Content stays frozen until release,
// same accepted tradeoff the drag path already documents.
#define Z_WIN_FLAG_RESIZABLE          (1u << 2)

// clamp this window's minimum size to whatever size it was CREATED
// at, instead of the global Z_WM_MIN_WIDTH/HEIGHT floor below.
// Meaningless without Z_WIN_FLAG_RESIZABLE also set.
//
// Exists because an app with fixed-size furniture inside its window --
// draw's tool column and pattern palette are exactly this -- has a
// size below which its own chrome stops fitting, and that size is
// generally not something wm can work out on its own. The app already
// knows it (it's why it asked for that size in the first place), so
// this lets it say "never smaller than what I asked for" in one bit
// rather than needing a whole separate minimum-size negotiation.
//
// Note this only sets a FLOOR, not a fixed size -- the window can
// still grow without limit (up to the screen). An app that wants a
// genuine fixed size just doesn't set Z_WIN_FLAG_RESIZABLE.
#define Z_WIN_FLAG_MIN_IS_CREATE      (1u << 3)

// wm -> app reply to a Z_WM_CREATE_WINDOW (same tag as the request):
// obj is a Z_MAP with "id" (Z_INT32, -1 on failure), "x", "y", "w",
// "h" (Z_UINT32) keys giving the window's actual allocated rect.
#define Z_WM_WINDOW_CREATED    101

// app -> wm: obj is a Z_UINT32 window id to destroy.
#define Z_WM_DESTROY_WINDOW    102

// wm -> app: sent after a drag completes. obj is a Z_MAP with the
// same "id"/"x"/"y"/"w"/"h" shape as Z_WM_WINDOW_CREATED, so the app
// knows where to redraw its content.
#define Z_WM_WINDOW_MOVED       103

// wm -> app: sent any time the wm has redrawn the screen (window
// created/destroyed, moved, or focus changed) and the app's content
// needs to be redrawn -- the wm's redraw is a full-screen clear, so
// it wipes app content along with the chrome. see
// docs/window_manager.md.
//
// unlike the other messages here, this one's payload is a packed
// Z_UINT32, not a Z_MAP -- it fires on every drag-position update
// (potentially many times a second), and allocating a fresh Z_MAP
// that often exhausts the wm's own heap (nothing frees these -- see
// docs/messaging.md -- which is fine for occasional messages but
// crashes the wm under sustained redraw traffic). width/height aren't
// included since there's no resize support yet, so an app that got
// them from Z_WM_WINDOW_CREATED already has them and they don't
// change.
#define Z_WM_REDRAW             104

// app -> wm: sent after an app finishes redrawing in response to
// Z_WM_REDRAW, so the wm knows it can move on to the next (more
// frontmost) overlapping window. obj is a Z_UINT32 window id
// (currently informational only -- matching is done by sender pid,
// not window id; see docs/window_manager.md "content z-order" for
// why this exists and its limits).
#define Z_WM_REDRAW_DONE         105

// wm -> app: sent to the *focused* window's owner only, whenever a
// key is pressed or released (see sw/os/hid.c for the interrupt-driven
// USB HID capture this is built on, and sw/common/zkbd.h for the
// usage->keysym translation wm.c applies before packing this). Like
// Z_WM_REDRAW, this can fire at high frequency (every keystroke, plus
// a release for each), so it's a packed Z_UINT32, not a Z_MAP -- same
// no-heap-allocation reasoning as Z_WM_REDRAW.
#define Z_WM_KEY                106

// wm -> app: the titlebar close icon (Z_WIN_FLAG_CLOSE_ICON) was
// clicked on one of this process's windows, and that window's
// Z_WIN_FLAG_CLOSE_KILLS_OWNER bit was NOT set -- see that flag's own
// comment above for the full reasoning. obj is a Z_UINT32 window id
// (the SPECIFIC window that was clicked closed, not necessarily this
// process's only one -- see repl's zapi.c/repl.c for the sub-window
// case this exists for). Purely a notification: wm does nothing else
// on its own here, the window stays open and fully interactive unless
// or until the owner calls z_win_destroy() (zwin.h) on this id
// itself, same as any other window destruction. Fire-and-forget, no reply
// expected -- same convention as Z_WM_REDRAW/Z_WM_KEY.
#define Z_WM_CLOSE               107

// wm -> app: sent once after a resize drag completes (not on every
// intermediate size -- same reasoning Z_WM_WINDOW_MOVED's own
// release-only delivery has). obj is a Z_MAP with the same
// "id"/"x"/"y"/"w"/"h" shape as Z_WM_WINDOW_CREATED, so the app can
// just run it through z_win_parse_rect() (zwin.h) exactly like it
// already does for the creation reply -- there is deliberately no new
// parsing path for this.
//
// Sent BEFORE the Z_WM_REDRAW that follows the resize repair, so an
// app that processes its queue in order always has the new w/h in
// hand by the time it's asked to redraw at that size. That ordering
// is load-bearing: Z_WM_REDRAW carries only x/y (it's a packed
// Z_UINT32 with no room for w/h -- see its own comment), so an app
// that saw the redraw first would redraw itself at its OLD size into
// a window that is no longer that size.
#define Z_WM_WINDOW_RESIZED      108

// wm -> app: pointer position/button state, sent to the window that
// currently owns the pointer -- normally the focused window while the
// cursor is over it, or, once a button has been pressed inside a
// window, that window until the button is released (see wm.c's
// mouse_capture, and "pointer capture" in docs/window_manager.md).
//
// Capture is what makes drag-style interaction work at all: a paint
// stroke, a slider, a rubber-banded rectangle all need to keep
// receiving events after the cursor has wandered outside the window
// it started in. Without it, every such gesture would silently cut
// off at the window edge.
//
// Like Z_WM_REDRAW/Z_WM_KEY this is a packed Z_UINT32, not a Z_MAP,
// and for exactly the same reason: it fires at pointer-movement
// rates, and a fresh Z_MAP per event exhausts wm's heap (nothing
// frees these -- see docs/messaging.md).
//
// wm COALESCES these -- one is sent only when the position or button
// state actually differs from the last one sent to that window, so a
// stationary mouse costs nothing. It does NOT rate-limit them beyond
// that, so an app should drain its whole queue and act on the LAST
// mouse message it finds rather than processing every one in turn;
// otherwise a slow redraw path makes the app fall progressively
// further behind the real cursor. See sw/apps/draw for that pattern.
//
// Coordinates are ABSOLUTE SCREEN coordinates, matching z_win_hw_line()
// and friends (zwin.h) rather than the window-relative convention
// z_win_draw_text() uses. z_win_mouse_content_xy() (zwin.h) converts
// to content-relative when that's what's wanted.
#define Z_WM_MOUSE               109

// x/y: absolute screen coordinates, 0-1023 each (the same 10-bit
// fields rtl/usb_hid.v's cursor register already uses -- see
// docs/user_input.md). buttons: the raw 4-bit button mask, bit 0 =
// left. inside: 1 if the cursor is within the receiving window's own
// content rect right now, 0 if it isn't -- which happens while a
// capture is active and the cursor has left the window. An app that
// draws on drag should keep drawing when this is 0 (clipping handles
// the rest); an app doing hover highlighting should stop.
#define Z_WM_PACK_MOUSE(x, y, buttons, inside) \
	((((inside) ? 1u : 0u) << 24) | \
	 (((uint32_t)(buttons) & 0xF) << 20) | \
	 (((uint32_t)(y) & 0x3FF) << 10) | \
	 ((uint32_t)(x) & 0x3FF))
#define Z_WM_UNPACK_MOUSE_X(v)        ((v) & 0x3FF)
#define Z_WM_UNPACK_MOUSE_Y(v)        (((v) >> 10) & 0x3FF)
#define Z_WM_UNPACK_MOUSE_BUTTONS(v)  (((v) >> 20) & 0xF)
#define Z_WM_UNPACK_MOUSE_INSIDE(v)   (((v) >> 24) & 1)

// left mouse button, as it appears in Z_WM_UNPACK_MOUSE_BUTTONS()'s
// result -- named here so apps stop open-coding `& 1`.
#define Z_MOUSE_BTN_LEFT   (1u << 0)
#define Z_MOUSE_BTN_RIGHT  (1u << 1)
#define Z_MOUSE_BTN_MIDDLE (1u << 2)

// keysym: 0x0000-0x7fff (see zkbd.h -- ASCII in 0x00-0x7f, named keys
// like arrows in 0x100+). modifiers: the raw USB HID modifier byte
// (zkbd.h's Z_KBD_MOD_* bits) at the time of this event. pressed: 1 =
// key down, 0 = key up.
#define Z_WM_PACK_KEY(keysym, modifiers, pressed) \
	((((uint32_t)(keysym) & 0x7FFF) << 9) | \
	 (((uint32_t)(modifiers) & 0xFF) << 1) | \
	 ((pressed) ? 1u : 0u))
#define Z_WM_UNPACK_KEY_KEYSYM(v)     (((v) >> 9) & 0x7FFF)
#define Z_WM_UNPACK_KEY_MODIFIERS(v)  (((v) >> 1) & 0xFF)
#define Z_WM_UNPACK_KEY_PRESSED(v)    ((v) & 1)

#define Z_WM_PACK_XY(id, x, y) \
	((((uint32_t)(id) & 0xFF) << 20) | \
	 (((uint32_t)(x) & 0x3FF) << 10) | \
	 ((uint32_t)(y) & 0x3FF))
#define Z_WM_UNPACK_ID(v)  (((v) >> 20) & 0xFF)
#define Z_WM_UNPACK_X(v)   (((v) >> 10) & 0x3FF)
#define Z_WM_UNPACK_Y(v)   ((v) & 0x3FF)

// defaults used when a Z_WM_CREATE_WINDOW request omits w/h
#define Z_WM_DEFAULT_WIDTH     140
#define Z_WM_DEFAULT_HEIGHT     100

// -- resize geometry --

// side length, in pixels, of the square grab area in a resizable
// window's lower-right corner. Shared between wm.c (which hit-tests
// and draws it) and any app that wants to keep its own content clear
// of it -- draw does exactly that, so the corner of its pattern
// palette isn't a dead zone that swallows clicks.
#define Z_WM_RESIZE_GRIP        12

// absolute floor on a resizable window's size, used unless the window
// set Z_WIN_FLAG_MIN_IS_CREATE (above) to raise it. Not merely
// cosmetic: a window narrower than the titlebar's own furniture
// (title text plus close icon) draws garbage, and one shorter than
// the titlebar has a negative-height content area, which underflows
// to an enormous unsigned height in several places downstream.
#define Z_WM_MIN_WIDTH          64
#define Z_WM_MIN_HEIGHT         (Z_WM_TITLEBAR_H + 20)

#endif
