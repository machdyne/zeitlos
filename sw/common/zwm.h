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
#define Z_WM_TITLEBAR_H   12

// -- message subjects --

// app -> wm request: obj is a Z_MAP with optional "title" (Z_STR),
// "w" (Z_UINT32), "h" (Z_UINT32) keys. any missing key falls back to
// a WM-chosen default. the WM chooses the window's position itself.
#define Z_WM_CREATE_WINDOW    100

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

#endif
