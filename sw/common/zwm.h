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

// -- additional titlebar icons --
//
// Each of these adds one more 8x8 icon to the titlebar, to the LEFT
// of the close icon (which keeps the exact position it always had --
// see wm.c's titlebar_icons()). Icons are laid out right-to-left in
// the order close, save, new, open, font, so an app that asks for
// new+save+close gets them reading "new save close" left to right,
// which is the order they're normally written in.
//
// Clicking one does NOT do anything by itself -- wm has no idea what
// "save" means for your app. It sends Z_WM_TITLEBAR_ICON (below) to
// the window's owner and nothing else, exactly like the non-killing
// form of Z_WIN_FLAG_CLOSE_ICON already does with Z_WM_CLOSE. Close
// keeps its own separate message rather than becoming a fifth kind
// here, so every existing app that handles Z_WM_CLOSE is unaffected.
//
// Ignored on a no-titlebar window, same as the close-icon flags.
//
// An app asking for more icons than its titlebar can hold simply
// gets the ones that fit (rightmost first) -- see titlebar_icons()'s
// own width check. That's a real case: these are drawn even on a
// window narrow enough that the title text disappears entirely.
#define Z_WIN_FLAG_NEW_ICON           (1u << 4)
#define Z_WIN_FLAG_SAVE_ICON          (1u << 5)
#define Z_WIN_FLAG_OPEN_ICON          (1u << 6)

// A font-size switch, for apps that can render their content at more
// than one size. wm draws it and reports the click like any other
// icon here; what "switch fonts" MEANS is entirely the app's
// business.
//
// Nothing sets this yet. The hardware glyph blitter can only draw
// from the single font currently in glyph memory (rtl/mem/glyph.v,
// loaded once by wm -- see zicon.h's header comment), so an app that
// wanted a second size today would have to render it in software.
// The flag, the icon and the click dispatch are all in place so that
// when zgfx grows a second font slot, the only thing left to write is
// the app's own handler. See docs/window_manager.md, "Second font".
#define Z_WIN_FLAG_FONT_ICON          (1u << 7)

// this window is MODAL with respect to its owner's other windows.
//
// While it exists, wm will not focus, raise, drag, resize or deliver
// pointer events to any OTHER window owned by the same process --
// clicking one of them raises and focuses the modal window instead.
// Windows belonging to other processes are completely unaffected:
// this is per-app modality, not a screen-wide grab. A modal dialog
// that froze the whole machine would be a much bigger hammer than
// anything here needs, and would make a wedged app unrecoverable.
//
// This exists because Z_WM_KEY carries no window id (see its own
// comment below): an app with two windows open cannot tell which one
// a keystroke was meant for. Modality resolves that by construction
// -- while a modal window is up, every key belongs to it -- which is
// what makes sw/common/zdialog.h's blocking dialogs possible at all.
//
// If a process somehow ends up with more than one modal window, the
// frontmost in z-order wins. That isn't a supported arrangement (the
// dialogs in zdialog.c are strictly one-at-a-time), just a defined
// outcome rather than an undefined one.
#define Z_WIN_FLAG_MODAL              (1u << 8)

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

// wm -> app: one of the extra titlebar icons (Z_WIN_FLAG_NEW_ICON /
// _SAVE_ICON / _OPEN_ICON / _FONT_ICON above) was clicked. obj is a
// packed Z_UINT32 carrying the window id AND which icon it was --
// Z_WM_PACK_TBICON() below.
//
// Purely a notification, exactly like the non-killing form of
// Z_WM_CLOSE: wm does nothing else on its own, because it has no idea
// what any of these mean for a given app. Note that close is NOT one
// of these kinds -- it keeps Z_WM_CLOSE, so no existing app has to
// learn a new message to keep working.
//
// A Z_MAP rather than a packed word would have been fine here (these
// fire at click rates, not pointer rates, so the heap pressure
// Z_WM_REDRAW's own comment warns about doesn't apply) -- packed
// anyway, purely so it matches the shape Z_WM_CLOSE already has and
// an app handling both doesn't need two parsing styles for two
// near-identical notifications.
#define Z_WM_TITLEBAR_ICON       110

// which icon a Z_WM_TITLEBAR_ICON refers to. Deliberately starts at 1
// so that 0 is never a valid kind -- see wm.c's hit_titlebar_icon(),
// which returns 0 for "no icon here" and would otherwise have to
// distinguish a miss from a hit on kind 0.
#define Z_WM_TBICON_NEW    1
#define Z_WM_TBICON_SAVE   2
#define Z_WM_TBICON_OPEN   3
#define Z_WM_TBICON_FONT   4

#define Z_WM_PACK_TBICON(id, kind) \
	((((uint32_t)(kind) & 0xFF) << 8) | ((uint32_t)(id) & 0xFF))
#define Z_WM_UNPACK_TBICON_ID(v)    ((v) & 0xFF)
#define Z_WM_UNPACK_TBICON_KIND(v)  (((v) >> 8) & 0xFF)

// app -> wm: change an existing window's titlebar text. obj is a
// Z_MAP with "id" (Z_INT32) and "title" (Z_STR) keys. No reply.
//
// Added for sw/apps/text, which shows the current filename (and
// whether it has unsaved changes) in its titlebar -- until now a
// window's title was fixed at creation, so an app whose title
// reflects its document had no way to say so. Any app with a
// document has the same need.
//
// A Z_MAP rather than a packed word because a title is a string and
// there is nowhere to pack one. That's fine here: this fires when a
// document is opened or first modified, not at pointer or keystroke
// rates, so the no-allocation reasoning behind Z_WM_REDRAW's packed
// payload doesn't apply.
//
// wm repairs the titlebar strip rather than the whole window, so
// retitling doesn't cost the owner a full content redraw -- see its
// handler in wm.c.
#define Z_WM_SET_TITLE           111

// -- launch arguments --
//
// There is no argv. z_proc_run() (zeitlos.h) takes a program name and
// nothing else, so a launcher that wants to say "open THIS file"
// has nowhere to put the filename.
//
// Rather than reserve space in every process table entry for
// something used once at startup, wm holds a SINGLE pending argument:
// the launcher sets it, starts the process, and the new process
// claims it on startup. Only one app is ever mid-launch at a time
// (wm's dock already assumes this), so one slot is enough, it is
// sized in one place, and growing it later for longer paths costs one
// constant rather than 16 process entries.
//
// The protocol:
//
//   launcher -> wm   Z_WM_SET_ARG   Z_STR, the argument
//   launcher         z_proc_run(app)
//   new app  -> wm   Z_WM_GET_ARG   (no payload)
//   wm       -> app  Z_WM_ARG       Z_STR, empty if there is none
//
// Claiming is destructive: wm marks the slot empty as soon as it
// answers, so a second app starting later gets nothing rather than
// re-opening the previous app's file.
//
// It also EXPIRES. Without that, an argument set for an app that
// never asks -- an older build, or one that dies before startup
// finishes -- would sit in the slot indefinitely and be collected by
// whatever the user happened to launch next, which is a genuinely
// confusing way for the wrong file to open. See Z_WM_ARG_TIMEOUT.
#define Z_WM_SET_ARG             112
#define Z_WM_GET_ARG             113
#define Z_WM_ARG                 114

// Longest launch argument, in bytes including the NUL. One buffer in
// wm, so this can be generous -- it needs to hold a full path, and
// z_flist_t's own Z_FLIST_PATH_MAX (zflist.h) is 64.
#define Z_WM_ARG_MAX             96

// How long a pending argument stays claimable, in kernel ticks
// (Z_TICK_HZ is 732, zsoc.h -- so this is roughly four seconds).
// Generous next to how long an app takes to reach its first message
// loop, short next to how long a user takes to click the next thing.
#define Z_WM_ARG_TIMEOUT         3000

// -- clipboard --
//
// System-wide cut/copy/paste, hosted by wm for the same reason the
// launch argument is: it needs to outlive the app that produced it,
// and wm is the one process guaranteed to be running whenever any of
// this matters.
//
//   app -> wm   Z_WM_CLIP_SET    Z_STR, the text to store
//   app -> wm   Z_WM_CLIP_GET    (no payload)
//   wm  -> app  Z_WM_CLIP_DATA   Z_STR, the stored text ("" if empty)
//
// Reads are NON-destructive, unlike the launch argument: pasting
// twice pastes the same thing twice, which is the whole point. There
// is no expiry either -- a clipboard that quietly emptied itself
// after a few seconds would be worse than useless.
//
// Text only, NUL-terminated on the wire. Embedded newlines are fine
// and expected (a multi-line selection is the common case); embedded
// NULs are not representable and would truncate.
#define Z_WM_CLIP_SET            115
#define Z_WM_CLIP_GET            116
#define Z_WM_CLIP_DATA           117

// app -> wm: repaint the WHOLE screen -- desktop background, every
// window's frame, and a Z_WM_REDRAW to every window's owner.
//
// For an app that has taken the framebuffer over entirely and is
// giving it back. A full-screen game (sw/apps/gamedemo) draws straight
// into VRAM over everything, including wm's own chrome and other
// apps' content, and none of those know it happened -- wm repaints on
// damage it caused itself, and this damage came from outside.
//
// Without this, quitting a full-screen app returns you to a desktop
// whose windows are all still ALIVE and still owned and still exactly
// where they were, but whose pixels are gone. The machine is fine and
// the screen is garbage, which is a confusing pair of facts to be
// handed.
//
// Deliberately no arguments -- not a rectangle. An app that overwrote
// the framebuffer generally cannot say what it damaged (a scrolling
// game touches every pixel over a few frames), and a wrong rectangle
// would leave debris that looks exactly like this bug not being fixed.
// The whole screen is the only honest answer, and it costs one
// repaint on an event that happens when a person quits an app.
#define Z_WM_REPAINT            118

// Clipboard capacity in bytes, including the terminating NUL.
//
// One static buffer in wm's .bss, so this is a fixed cost paid once
// rather than per process. 4KB is about a page of dense text, which
// is the size of thing this is for -- moving a paragraph between
// windows, not moving a file. Anything longer is TRUNCATED at the
// set, not refused: losing the tail of an over-large copy is a better
// failure than the paste doing nothing at all, and the app can see
// what it got back.
#define Z_WM_CLIP_MAX            4096

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

// How far, in CONTENT-relative coordinates, the resize grip reaches
// in from the content area's bottom-right corner -- i.e. the grip
// occupies content x >= (content_w - Z_WIN_GRIP_INSET) and content
// y >= (content_h - Z_WIN_GRIP_INSET).
//
// It is Z_WM_RESIZE_GRIP minus 2 rather than Z_WM_RESIZE_GRIP because
// the grip is measured from the WINDOW's outer corner while an app
// lays out against its CONTENT rect, and the two differ by the 2px
// border inset z_win_content_rect() (zwin.h) applies.
//
// An app with furniture down the right edge or along the bottom of a
// resizable window needs this, or that furniture silently swallows
// the grip and the window can no longer be resized at all. A vertical
// scrollbar is exactly this case: it wants
// `len = content_h - Z_WIN_GRIP_INSET`. Provided here, next to the
// constant it's derived from, so the subtraction isn't re-done (and
// eventually got wrong) in each app.
#define Z_WIN_GRIP_INSET        (Z_WM_RESIZE_GRIP - 2)

// absolute floor on a resizable window's size, used unless the window
// set Z_WIN_FLAG_MIN_IS_CREATE (above) to raise it. Not merely
// cosmetic: a window narrower than the titlebar's own furniture
// (title text plus close icon) draws garbage, and one shorter than
// the titlebar has a negative-height content area, which underflows
// to an enormous unsigned height in several places downstream.
#define Z_WM_MIN_WIDTH          64
#define Z_WM_MIN_HEIGHT         (Z_WM_TITLEBAR_H + 20)

#endif
