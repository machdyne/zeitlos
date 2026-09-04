/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * The Zeitlos Scheme API -- Files. See zapi.h and docs/scheme_api.md.
 *
 * Every procedure here has exactly the shape ms.c's own bi_* builtins
 * do (ms_val *(*)(ms_val *args), args pre-evaluated by ms_eval()
 * before this ever runs) -- nothing Zeitlos-specific about the
 * calling convention itself, see ms_api.h's own comment on
 * ms_builtin. Bad argument types raise a real Scheme panic
 * (ms_log(MS_PANIC, ...), caught by repl.c's existing
 * setjmp/longjmp recovery around every eval), same as ms's own
 * "expected a string"/"expected a number" builtins already do --
 * deliberately not a second, quieter error convention just for
 * Zeitlos-added procedures.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>	// snprintf() -- see zapi_read_form() below
#include <unistd.h>	// sbrk() -- see the `free` procedure below

#include "ms_api.h"
#include "../../common/zfsapp.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zfont.h"
#include "../../common/zdns.h"
#include "../../common/znet.h"
#include "../../common/zstream.h"
#include "../../common/zproc.h"
#include "../../common/zrtc.h"	// the wall clock -- see (current-time)/
								// (current-date) below
#include "../../common/zrng.h"	// the system CSPRNG -- see (random) below
#include "../../common/zpad.h"	// gamepads -- see (gamepad ...) below
#include "../../common/zuart.h"	// UART1 -- see (uart1-* ...) below
#include "../../common/zi2c.h"	// bit-bang I2C -- see (i2c-* ...) below
#include "../../common/zspi.h"	// bit-bang SPI -- see (spi-* ...) below
#include "../../common/zgpio.h"	// the GPIO ports -- see (gpio-* ...)
								// below. Direct MMIO, no syscall: this is
								// a peripheral an app touches itself, same
								// as zpad.h above. See docs/gpio.md.
#include "../../common/zsoc.h"	// Z_VIDEO_MODE_*, z_video_mode_name(),
								// z_video_mode_from_name() -- the naming
								// helpers only. The actual get/set go
								// through zeitlos.h's syscall wrappers
								// (z_video_mode_get/_set), not zsoc.h's
								// direct-MMIO inlines.
#include "zapi.h"

// -- small shared helpers --

static const char *zapi_arg_str(ms_val *v, const char *who) {
	if (!ms_is_str(v)) ms_log(MS_PANIC, "%s: expected a string", who);
	return ms_str_val(v);
}

static int zapi_arg_int(ms_val *v, const char *who) {
	if (!ms_is_num(v)) ms_log(MS_PANIC, "%s: expected a number", who);
	return (int)ms_num_val(v);
}

// Is this argument a "yes"? Used by the GPIO setters below.
//
// ms_api.h exports no boolean predicate -- only ms_mk_bool() -- and
// this file is deliberately the place where new procedures live
// WITHOUT touching the ms submodule (see docs/scheme_api.md). It does
// not need one: #t and #f are singletons in ms.c, so ms_mk_bool(false)
// hands back the one and only #f object and identity against it is the
// entire test. That is precisely ms.c's own truthy().
//
// WITH ONE DELIBERATE DEVIATION: a NUMBER is judged by its value, so 0
// is false here where Scheme says every number is true. This is for
// hardware, and `(gpio-set 0 3 0)` meaning "drive it high" would be an
// afternoon lost -- especially since the shell's own `gpio 0 3 0`
// (sw/os/sh.c) means low, and somebody moving between the two should
// not have to hold two rules. #f is still false, so both spellings
// work and neither surprises.
static bool zapi_arg_truthy(ms_val *v) {
	if (ms_is_num(v)) return ms_num_val(v) != 0;
	return v != ms_mk_bool(false);
}

// -- Files --

// (ls) or (ls "path") -- list of filenames (each a full "/"-prefixed
// path, e.g. "/WM") as strings. An empty or missing directory just
// returns an empty list, not an error -- consistent with fs_list()'s
// own "NULL means nothing to show" convention (zfsapp.h).
static ms_val *zapi_ls(ms_val *args) {

	const char *path = NULL;
	if (!ms_is_nil(args)) path = zapi_arg_str(ms_car(args), "ls");

	uint32_t count = 0;
	char **names = fs_list(path, 0, &count);

	if (!names) return ms_mk_str_list(NULL, 0);

	// ms_mk_str_list() takes ownership of each STRING (frees them
	// itself, eventually, via GC) but not of the ARRAY fs_list()
	// itself malloc'd to hold the pointers -- that's this function's
	// own to free, same as any other fs_list() caller would need to.
	ms_val *result = ms_mk_str_list(names, (int)count);
	free(names);
	return result;

}

// (file-size "name") -- size in bytes, or #f if missing (or
// genuinely empty -- fs_size()'s own documented ambiguity, zfsapp.h).
static ms_val *zapi_file_size(ms_val *args) {
	const char *name = zapi_arg_str(ms_car(args), "file-size");
	int sz = fs_size((char *)name);
	if (sz <= 0) return ms_mk_bool(false);
	return ms_mk_num(sz);
}

// (read-file "name") -- whole file contents as a string, or #f if
// missing/unreadable.
static ms_val *zapi_read_file(ms_val *args) {

	const char *name = zapi_arg_str(ms_car(args), "read-file");

	int sz = fs_size((char *)name);
	if (sz <= 0) return ms_mk_bool(false);

	char *raw = fs_mallocfile((char *)name);
	if (!raw) return ms_mk_bool(false);

	// fs_mallocfile() returns exactly `sz` raw bytes, NOT
	// NUL-terminated (sw/ext/te/te.c's own te_load(), the other
	// current caller, tracks the size separately rather than relying
	// on a terminator -- see its own for-loop). ms's T_STR values
	// ARE plain NUL-terminated C strings throughout ms.c (strlen()
	// etc.), so a fresh, terminated copy is needed here --
	// ms_mk_str() takes ownership of whatever it's handed, so this
	// copy becomes that value's storage for its whole lifetime, no
	// further copy beyond this one.
	//
	// A file containing an embedded NUL byte will read back
	// truncated at that byte (ms strings have no embedded-NUL
	// support) -- fine for ordinary text, not a safe way to read an
	// arbitrary binary file. Worth knowing, not fixed here.
	char *s = malloc((size_t)sz + 1);
	if (!s) { free(raw); return ms_mk_bool(false); }
	memcpy(s, raw, (size_t)sz);
	s[sz] = 0;
	free(raw);

	return ms_mk_str(s);

}

// (write-file "name" "contents") -- creates/truncates + writes;
// #t/#f. Same embedded-NUL caveat as read-file above, in reverse:
// strlen() is what decides how much of `contents` gets written.
static ms_val *zapi_write_file(ms_val *args) {

	const char *name = zapi_arg_str(ms_car(args), "write-file");
	const char *content = zapi_arg_str(ms_car(ms_cdr(args)), "write-file");

	int len = (int)strlen(content);
	int written = fs_write_file((char *)name, (char *)content, len);

	return ms_mk_bool(written == len);

}

// (delete-file "name") -- #t/#f.
static ms_val *zapi_delete_file(ms_val *args) {
	const char *name = zapi_arg_str(ms_car(args), "delete-file");
	return ms_mk_bool(fs_unlink((char *)name) != 0);
}

// -- Windows --
//
// A window created here is owned by `repl`'s OWN pid, not whichever
// `term` connection happened to type the command -- z_win_create()
// runs inside repl's process regardless of which connection's line
// triggered it, so this falls out for free (docs/scheme_api.md \S4,
// "Window ownership"). A window's lifetime is tied to repl itself,
// not to any one connection -- it survives a `term` disconnect the
// same way a Scheme `define` already does (both live in the same
// shared ms_global_env/process state), and there's deliberately no
// cleanup-on-Z_PORT_CLOSE the way te_bridge.c's single editor session
// needs.
//
// KNOWN LIMITATION, not fixed in this revision: repl's own main
// message loop (repl.c) doesn't read or respond to Z_WM_REDRAW at
// all -- a window created here doesn't participate in the normal
// occlusion/z-order redraw protocol (docs/window_manager.md). Content
// drawn via the hardware-accelerated calls below writes straight into
// the real framebuffer and stays there indefinitely as long as
// nothing else overdraws that screen region, so a window that's never
// occluded works fine forever with no redraw needed -- but if another
// window is moved on top of it and then away again, wm.c's own
// wait_for_redraw_done() (wm.c) will time out waiting for an ack
// this window never sends, and the previously-covered area won't
// automatically repaint. Bounded (wm.c's own timeout, not an
// indefinite stall) and cosmetic, not a correctness/safety issue --
// worth fixing (repl's own message loop would need to recognize
// Z_WM_REDRAW and re-issue whatever this window last drew, which
// means tracking draw history per window, not attempted here) if real
// usage shows it matters.

// small, fixed-size, bounded table -- same "small on purpose" spirit
// as Z_REPL_MAX_CONNS (repl.c) itself. Maps a window's own wm-
// assigned id (exposed to Scheme as a plain number, see
// docs/scheme_api.md \S4's "handles are plain numbers" note) to the
// z_win_t (x/y/w/h) z_win_fill_rect()/z_win_draw_text()/z_win_hw_*()
// all need to compute the right clip/offset. Raise if real usage
// needs more than a handful of windows open via Scheme at once.
#define ZAPI_WIN_MAX 8

typedef struct {
	bool	used;
	z_win_t	win;
} zapi_win_slot_t;

static zapi_win_slot_t zapi_windows[ZAPI_WIN_MAX];

static z_win_t *zapi_find_window(int id) {
	for (int i = 0; i < ZAPI_WIN_MAX; i++)
		if (zapi_windows[i].used && zapi_windows[i].win.id == id)
			return &zapi_windows[i].win;
	return NULL;
}

// like zapi_find_window(), but raises a real Scheme error instead of
// silently returning NULL -- used by every drawing/clear procedure
// below, where a missing window is virtually always a usage mistake
// (wrong id, a window id that belongs to some OTHER app entirely --
// e.g. `term`'s own window, id 1, owner != repl's own pid -- or one
// this process already destroyed) rather than a legitimate, expected
// outcome the caller would want to branch on quietly. This is
// deliberately NOT how the Files procedures handle "not found"
// (file-size/read-file return #f for a missing file, no panic) --
// a missing FILE is common and often exactly what a caller is
// checking for ("does this exist yet"); a missing WINDOW handed to a
// drawing call essentially never is, there's nothing useful to draw
// into. win-destroy is the one exception (still returns #f, not a
// panic, on an already-gone id) -- destroying something twice is a
// common, often-intentional pattern (e.g. an unconditional cleanup
// step) worth tolerating quietly, unlike trying to draw into nothing.
static z_win_t *zapi_win_or_panic(int id, const char *who) {
	z_win_t *win = zapi_find_window(id);
	if (!win)
		ms_log(MS_PANIC,
			"%s: no window with id %d -- create one with win-create "
			"first, or it may already be destroyed (window ids from "
			"other apps, e.g. term's own window, don't work here --"
			" only ones this process created itself)", who, id);
	return win;
}

// converts WINDOW-relative coordinates -- (0,0) is the window's own
// content top-left, i.e. already past the 1px frame border AND the
// 1px breathing-room margin z_win_content_rect() (zwin.c) insets by
// on every edge -- into absolute framebuffer coordinates, and hands
// back the same clip rect every drawing call needs to stay inside
// this window's own bounds.
//
// Deliberately NOT going through z_win_hw_line()/z_win_hw_box()/
// z_win_draw_text() (zwin.c) for this: those three are inconsistent
// with each other about what "0,0" even means. z_win_hw_line()/
// z_win_hw_box() take screen coordinates outright (clipped to the
// window, but never offset by win->x/win->y at all) -- passing them a
// small x0/y0 draws near the SCREEN origin, nowhere near this window,
// unless it happens to already be near (0,0) on screen. z_win_draw_
// text() DOES offset, but by `win->x` on the X axis specifically
// (the window's outer, BORDER-inclusive edge) while using `clip.y0`
// (the true content-area edge, past the margin) on Y -- so text at
// x=0 has its leftmost ~2px silently clipped, since clip.x0 is
// win->x+2, past where x=0 actually lands.
//
// Going straight to z_fb_hw_line()/z_fb_hw_box()/z_fb_draw_text()
// (zgfx.h) here sidesteps all of that: every zapi_* drawing procedure
// below uses the exact same (0,0) = content top-left convention,
// documented in docs/scheme_api.md, with no cross-axis inconsistency
// -- and without touching zwin.c itself, so nothing else that already
// calls z_win_hw_line()/z_win_hw_box()/z_win_draw_text() (e.g.
// sw/apps/hello_win) has its existing behavior changed underneath it.
static void zapi_win_rect(const z_win_t *win, z_clip_t *clip) {
	z_win_content_rect(win, clip);
}

// (win-create) or (win-create "title") or (win-create "title" w h) or
// (win-create "title" w h x y) -- title/w/h/x/y all optional, but
// positional (can't skip w/h to give just x/y) -- x/y (both or
// neither) place the window at that exact screen position instead of
// letting the wm auto-cascade it (z_win_create_ex(), zwin.c/.h) --
// see docs/scheme_api.md's own "window placement" note for why this
// exists and why it's safe with no dependency on the wm's redraw-ack
// protocol at all (creation is exempt from it). Returns the new
// window's id (a plain number) or #f (window-table full, or the wm
// refused/didn't reply).
static ms_val *zapi_win_create(ms_val *args) {

	const char *title = "";
	uint32_t w = 0, h = 0;
	int32_t x = -1, y = -1;

	if (!ms_is_nil(args)) {
		title = zapi_arg_str(ms_car(args), "win-create");
		args = ms_cdr(args);
	}
	if (!ms_is_nil(args)) {
		w = (uint32_t)zapi_arg_int(ms_car(args), "win-create");
		args = ms_cdr(args);
	}
	if (!ms_is_nil(args)) {
		h = (uint32_t)zapi_arg_int(ms_car(args), "win-create");
		args = ms_cdr(args);
	}
	if (!ms_is_nil(args)) {
		x = zapi_arg_int(ms_car(args), "win-create");
		args = ms_cdr(args);
	}
	if (!ms_is_nil(args)) {
		y = zapi_arg_int(ms_car(args), "win-create");
	}

	int slot = -1;
	for (int i = 0; i < ZAPI_WIN_MAX; i++) {
		if (!zapi_windows[i].used) { slot = i; break; }
	}
	if (slot < 0) return ms_mk_bool(false);

	z_win_t win;
	// Z_WIN_FLAG_CLOSE_ICON only -- NOT Z_WIN_FLAG_CLOSE_KILLS_OWNER.
	// A single repl process can own several of these at once (that's
	// the entire point of zapi_windows[] being a table, not a single
	// slot), so clicking one window's close icon must not take repl
	// itself, or any of its OTHER windows, down with it -- see
	// Z_WIN_FLAG_CLOSE_KILLS_OWNER's own comment in zwm.h. wm instead
	// sends Z_WM_CLOSE for just this window id, handled in repl.c's
	// main loop by zapi_win_close() below (destroys this one table
	// entry, same bookkeeping as an explicit (win-destroy id) call).
	z_rv rv = z_win_create_flags(&win, title, w, h, x, y, Z_WIN_FLAG_CLOSE_ICON);
	if (rv != Z_OK) return ms_mk_bool(false);

	zapi_windows[slot].used = true;
	zapi_windows[slot].win = win;

	return ms_mk_num(win.id);

}

// destroys the Scheme-owned window with this wm window id, if this
// process still has one open under it -- same bookkeeping
// zapi_win_destroy() (the Scheme-facing (win-destroy id), just above
// it in the source) does, just callable directly from C. Added for
// repl.c's Z_WM_CLOSE handling (zwm.h) -- the titlebar close icon on
// a Scheme-created window (zapi_win_create() above) was clicked, and
// wm is leaving the decision of what that means up to repl, per
// Z_WM_CLOSE's own contract; "destroy the window Scheme itself was
// tracking under this id" is the obvious, minimal thing to do with
// that notification without inventing a second, Scheme-visible event
// system just for this. A window id repl doesn't currently own
// (already destroyed some other way, or was never one of repl's --
// shouldn't happen in practice, but Z_WM_CLOSE carries no ownership
// guarantee of its own) is silently ignored, same as
// zapi_win_destroy()'s own "not found" case.
void zapi_win_close(int id) {

	for (int i = 0; i < ZAPI_WIN_MAX; i++) {
		if (zapi_windows[i].used && zapi_windows[i].win.id == id) {
			z_win_destroy(&zapi_windows[i].win);
			zapi_windows[i].used = false;
			return;
		}
	}

}

// (win-destroy id) -- #t/#f.
static ms_val *zapi_win_destroy(ms_val *args) {

	int id = zapi_arg_int(ms_car(args), "win-destroy");

	for (int i = 0; i < ZAPI_WIN_MAX; i++) {
		if (zapi_windows[i].used && zapi_windows[i].win.id == id) {
			z_win_destroy(&zapi_windows[i].win);
			zapi_windows[i].used = false;
			return ms_mk_bool(true);
		}
	}

	return ms_mk_bool(false);

}

// (win-clear id) -- #t, or raises an error for an id this process
// didn't create (see zapi_win_or_panic()'s own comment).
static ms_val *zapi_win_clear(ms_val *args) {
	z_win_t *win = zapi_win_or_panic(zapi_arg_int(ms_car(args), "win-clear"), "win-clear");
	z_win_clear(win);
	return ms_mk_bool(true);
}

// (line id x0 y0 x1 y1 color) -- hardware-accelerated
// (z_fb_hw_line(), the GPU line rasterizer). Coordinates are WINDOW-
// relative -- (0,0) is this window's own content top-left (see
// zapi_win_rect()'s own comment). #t, or raises an error for an id
// this process didn't create (zapi_win_or_panic()'s own comment).
static ms_val *zapi_line(ms_val *args) {

	int id = zapi_arg_int(ms_car(args), "line"); args = ms_cdr(args);
	int x0 = zapi_arg_int(ms_car(args), "line"); args = ms_cdr(args);
	int y0 = zapi_arg_int(ms_car(args), "line"); args = ms_cdr(args);
	int x1 = zapi_arg_int(ms_car(args), "line"); args = ms_cdr(args);
	int y1 = zapi_arg_int(ms_car(args), "line"); args = ms_cdr(args);
	int color = zapi_arg_int(ms_car(args), "line");

	z_win_t *win = zapi_win_or_panic(id, "line");

	z_clip_t clip;
	zapi_win_rect(win, &clip);

	z_fb_hw_line(clip.x0 + x0, clip.y0 + y0, clip.x0 + x1, clip.y0 + y1, color, &clip);
	return ms_mk_bool(true);

}

// (box id x0 y0 x1 y1 color) -- hardware-accelerated filled box
// (z_fb_hw_box()). Window-relative coordinates, same as `line` above.
// #t, or raises an error (zapi_win_or_panic()'s own comment).
static ms_val *zapi_box(ms_val *args) {

	int id = zapi_arg_int(ms_car(args), "box"); args = ms_cdr(args);
	int x0 = zapi_arg_int(ms_car(args), "box"); args = ms_cdr(args);
	int y0 = zapi_arg_int(ms_car(args), "box"); args = ms_cdr(args);
	int x1 = zapi_arg_int(ms_car(args), "box"); args = ms_cdr(args);
	int y1 = zapi_arg_int(ms_car(args), "box"); args = ms_cdr(args);
	int color = zapi_arg_int(ms_car(args), "box");

	z_win_t *win = zapi_win_or_panic(id, "box");

	z_clip_t clip;
	zapi_win_rect(win, &clip);

	z_fb_hw_box(clip.x0 + x0, clip.y0 + y0, clip.x0 + x1, clip.y0 + y1, color, &clip);
	return ms_mk_bool(true);

}

// (text id x y "s" color) -- z_font_6x12 (dense/compact, a reasonable
// default for arbitrary program output; nothing yet exposes a way to
// pick a different one -- z_font_8x16/_5x7/_5x8 all exist, zfont.h,
// if a future revision wants a size argument). Window-relative
// coordinates, same as `line`/`box` above. #t, or raises an error
// (zapi_win_or_panic()'s own comment).
static ms_val *zapi_text(ms_val *args) {

	int id = zapi_arg_int(ms_car(args), "text"); args = ms_cdr(args);
	int x = zapi_arg_int(ms_car(args), "text"); args = ms_cdr(args);
	int y = zapi_arg_int(ms_car(args), "text"); args = ms_cdr(args);
	const char *s = zapi_arg_str(ms_car(args), "text"); args = ms_cdr(args);
	int color = zapi_arg_int(ms_car(args), "text");

	z_win_t *win = zapi_win_or_panic(id, "text");

	z_clip_t clip;
	zapi_win_rect(win, &clip);

	z_fb_draw_text(clip.x0 + x, clip.y0 + y, s, color, &z_font_6x12, &clip);
	return ms_mk_bool(true);

}

// -- Messaging --

// (getpid) -- this process's own pid.
static ms_val *zapi_getpid(ms_val *args) {
	(void)args;
	return ms_mk_num(z_getpid());
}

// (pid-lookup "name") -- resolves a registered name ("wm0", "net0",
// ...) to a pid, or #f.
static ms_val *zapi_pid_lookup(ms_val *args) {
	const char *name = zapi_arg_str(ms_car(args), "pid-lookup");
	uint32_t pid;
	if (!z_pid_lookup(name, &pid)) return ms_mk_bool(false);
	return ms_mk_num(pid);
}

// (msg-send pid subject tag "data") -- fire-and-forget Z_STR message.
// #t/#f. Deliberately Z_STR-only for v1 (matches Z_REPL_EVAL's own
// Z_STR-first convention, sw/common/zrepl.h) -- richer payloads
// (Z_MAP/Z_BLOB) are a natural follow-up once something actually
// needs one, not built speculatively ahead of a real caller.
static ms_val *zapi_msg_send(ms_val *args) {

	int pid = zapi_arg_int(ms_car(args), "msg-send"); args = ms_cdr(args);
	int subject = zapi_arg_int(ms_car(args), "msg-send"); args = ms_cdr(args);
	int tag = zapi_arg_int(ms_car(args), "msg-send"); args = ms_cdr(args);
	const char *data = zapi_arg_str(ms_car(args), "msg-send");

	z_rv rv = z_msg_new_send((uint32_t)pid, (uint32_t)subject, (uint32_t)tag,
		z_obj_str(data));

	return ms_mk_bool(rv == Z_OK);

}

// polls for a message matching (subject, tag), for up to
// `timeout_ticks` (z_uptime_ticks() units, ~732Hz) -- shared by
// `msg-wait`'s own optional-timeout case below AND `tput` (see its
// own comment for why it needs exactly this same loop). There's no
// z_msg_wait_timeout() reachable from app code (sw/os/msg.h's own
// version references the kernel's raw z_kernel_ticks global directly
// and is compiled only into kernel.elf -- not something an app can
// link against) and no sleep/yield primitive in this OS at all, so
// this is a genuine busy-wait: non-blocking z_msg_read() calls,
// checked against elapsed z_uptime_ticks(), discarding any
// non-matching message along the way -- exactly what z_msg_wait()
// itself already documents doing for the no-timeout case, just with
// a deadline added. Burns real cycles on real hardware for however
// long nothing matching arrives, though it doesn't change how long
// repl is unresponsive to its OTHER connections either way (already
// blocked for the same duration regardless of whether it's spinning
// or sleeping). Worth a real OS-level timeout primitive if this
// proves costly in practice; not built here.
static bool zapi_msg_wait_timeout(z_msg_t *msg, uint32_t subject, uint32_t tag,
	uint32_t timeout_ticks) {

	uint32_t start = z_uptime_ticks();

	while (z_uptime_ticks() - start < timeout_ticks) {
		if (z_msg_read(msg) != Z_OK) continue;
		if (msg->subject == subject && msg->tag == tag) return true;
		// not the one we're waiting for -- discard, same as
		// z_msg_wait()'s own documented behavior
	}

	return false;

}

// (msg-wait subject tag) or (msg-wait subject tag timeout-ms) --
// blocks for a matching reply, returns its payload as a string (or
// #f if the reply wasn't a Z_STR, or on timeout). With no timeout
// given, this can block indefinitely -- z_msg_wait()'s own contract
// exactly, same accepted-blocking-call tradeoff class this process
// already has for `te`/`tget`/`tput` (docs/editor.md,
// docs/scheme_api.md's own Networking section). With a timeout given,
// see zapi_msg_wait_timeout()'s own comment for what that actually
// costs.
static ms_val *zapi_msg_wait(ms_val *args) {

	int subject = zapi_arg_int(ms_car(args), "msg-wait"); args = ms_cdr(args);
	int tag = zapi_arg_int(ms_car(args), "msg-wait"); args = ms_cdr(args);

	bool has_timeout = !ms_is_nil(args);
	int timeout_ms = has_timeout ? zapi_arg_int(ms_car(args), "msg-wait") : 0;

	z_msg_t msg;
	bool found;

	if (!has_timeout) {
		found = (z_msg_wait(&msg, (uint32_t)subject, (uint32_t)tag) == Z_OK);
	} else {
		uint32_t timeout_ticks = (uint32_t)(((int64_t)timeout_ms * 732) / 1000);
		found = zapi_msg_wait_timeout(&msg, (uint32_t)subject, (uint32_t)tag, timeout_ticks);
	}

	if (!found) return ms_mk_bool(false);
	if (msg.obj.type != Z_STR || !msg.obj.val.str) return ms_mk_bool(false);

	char *s = strdup(msg.obj.val.str);
	if (!s) return ms_mk_bool(false);

	return ms_mk_str(s);

}

// -- Networking --
//
// `tget`/`tput` are a direct port of `sh.c`'s own existing commands
// (sw/os/sh.c) -- the TFTP-over-zstream plumbing they use already
// exists and works, this just makes it reachable from Scheme. See
// docs/scheme_api.md's own "Networking" section for the full
// writeup, including the accepted tradeoff both share with `te`
// (docs/editor.md): both block this whole process for the duration
// of the transfer (matching zstream.h's own "the consumer side IS
// allowed to block... a consumer with nothing else to do while it
// waits (e.g. the shell)" reasoning -- Scheme code running inside
// repl IS exactly that shell), and `tput` specifically also
// discards any unrelated message that arrives mid-transfer (its own
// producer loop reads repl's mailbox directly, same as sh.c's own
// version already does) rather than queueing it for later.

static uint32_t zapi_net_pid_cache;
static bool zapi_net_pid_resolved = false;

// Returns 0 if net isn't running. Same lookup-then-cache pattern as
// zwin.c's resolve_wm_pid(); see zdns.c's copy for why there is no
// fallback to the fixed Z_PID_NET constant any more.
//
// A failed lookup is not cached: repl may start before net has
// registered.
static uint32_t zapi_resolve_net_pid(void) {
	if (!zapi_net_pid_resolved) {
		if (!z_pid_lookup("net0", &zapi_net_pid_cache)) return 0;
		zapi_net_pid_resolved = true;
	}
	return zapi_net_pid_cache;
}

// a fresh tag per tput call, not a constant 0 -- same reasoning as
// sh.c's own next_tftp_tag(): otherwise a stale reply from a timed-
// out call could get matched to a later, unrelated one.
static uint32_t zapi_next_tftp_tag(void) {
	static uint32_t tag = 0;
	return ++tag;
}

// how long tput waits for net to open its return stream, and
// separately for net's own Z_NET_TFTP_PUT_REPLY once the transfer
// itself finishes -- same 10s sh.c's own TFTP_REPLY_TIMEOUT_TICKS
// uses (z_uptime_ticks() is ~732Hz).
#define ZAPI_TFTP_TIMEOUT_TICKS (10 * 732)

// (tget ip-or-host remote-file) or (tget ip-or-host remote-file
// local-file) -- fetches a file via TFTP (needs `run net`), returns
// the number of bytes written on success. Any failure (DNS/host
// resolution, opening the remote or local file, a transfer error)
// raises a real Scheme error describing what went wrong -- a bare
// #f here would lose exactly the information that matters most for
// a network operation (see zapi_win_or_panic()'s own comment on the
// same reasoning applied to windows).
static ms_val *zapi_tget(ms_val *args) {

	const char *ip_str = zapi_arg_str(ms_car(args), "tget"); args = ms_cdr(args);
	const char *remote = zapi_arg_str(ms_car(args), "tget"); args = ms_cdr(args);
	const char *local = remote;
	if (!ms_is_nil(args)) local = zapi_arg_str(ms_car(args), "tget");

	uint32_t ip;
	char err[64];
	if (!z_resolve_host(ip_str, &ip, err, sizeof(err)))
		ms_log(MS_PANIC, "tget: %s", err);

	z_obj_t req = z_obj_map(2);
	z_map_set(&req, "ip", z_obj_uint32(ip));
	z_map_set(&req, "filename", z_obj_str(remote));
	// note: `req` intentionally never freed -- same borrowed-payload
	// reasoning docs/messaging.md documents throughout, and sh.c's
	// own tget already relies on for this exact call.

	zstream_consumer_t cons;
	if (!zstream_open(&cons, zapi_resolve_net_pid(), req, err, sizeof(err)))
		ms_log(MS_PANIC, "tget: failed to open: %s", err);

	int handle = fs_open_write(local);
	if (handle < 0) {
		zstream_abort(&cons);
		ms_log(MS_PANIC, "tget: failed to open '%s' for writing", local);
	}

	uint32_t total = 0;

	while (1) {

		const uint8_t *data;
		uint32_t len;
		zstream_result_t r = zstream_pull(&cons, &data, &len, err, sizeof(err));

		if (r == ZSTREAM_EOF) break;

		if (r == ZSTREAM_ERROR) {
			fs_close_handle(handle);
			ms_log(MS_PANIC, "tget: %s", err);
		}

		if (fs_write_chunk(handle, data, (int)len) != (int)len) {
			fs_close_handle(handle);
			zstream_abort(&cons);
			ms_log(MS_PANIC, "tget: local write failed");
		}

		total += len;

	}

	fs_close_handle(handle);

	return ms_mk_num(total);

}

// (tput ip-or-host local-file) or (tput ip-or-host local-file
// remote-file) -- sends a file via TFTP (needs `run net`). #t on
// success; any failure raises a real Scheme error, same reasoning as
// `tget` above.
static ms_val *zapi_tput(ms_val *args) {

	const char *ip_str = zapi_arg_str(ms_car(args), "tput"); args = ms_cdr(args);
	const char *local = zapi_arg_str(ms_car(args), "tput"); args = ms_cdr(args);
	const char *remote = local;
	if (!ms_is_nil(args)) remote = zapi_arg_str(ms_car(args), "tput");

	uint32_t ip;
	char err[64];
	if (!z_resolve_host(ip_str, &ip, err, sizeof(err)))
		ms_log(MS_PANIC, "tput: %s", err);

	int size = fs_size((char *)local);
	if (size <= 0)
		ms_log(MS_PANIC, "tput: local file '%s' not found/empty", local);

	int handle = fs_open_read(local);
	if (handle < 0)
		ms_log(MS_PANIC, "tput: failed to open '%s' for reading", local);

	uint32_t tag = zapi_next_tftp_tag();
	z_obj_t req = z_obj_map(2);
	z_map_set(&req, "ip", z_obj_uint32(ip));
	z_map_set(&req, "filename", z_obj_str(remote));
	z_msg_new_send(zapi_resolve_net_pid(), Z_NET_TFTP_PUT, tag, req);
	// note: `req` intentionally never freed -- same reasoning as
	// `tget` above.

	// act as a zstream *producer* now -- net is about to open a
	// stream back to us to pull this file's bytes. We have nothing
	// else to do while this runs (this whole call already blocks
	// repl), so a simple loop reading our own mailbox directly is
	// fine here -- exactly sh.c's own tput, down to reusing the same
	// timeout duration (ZAPI_TFTP_TIMEOUT_TICKS above).
	zstream_producer_t prod;
	bool have_stream = false;
	bool producer_ok = true;
	uint8_t chunk[ZSTREAM_CHUNK_SIZE_DEFAULT];
	uint32_t start = z_uptime_ticks();

	while (z_uptime_ticks() - start < ZAPI_TFTP_TIMEOUT_TICKS) {

		z_msg_t msg;
		if (z_msg_read(&msg) != Z_OK) continue;

		if (!have_stream) {
			if (msg.subject != Z_STREAM_OPEN) continue;	// discard anything else while waiting to start
			zstream_accept(&prod, msg.from, msg.tag);
			have_stream = true;
			start = z_uptime_ticks();
			continue;
		}

		if (msg.subject == Z_STREAM_ABORT) {
			producer_ok = false;
			break;
		}

		if (msg.subject != Z_STREAM_PULL) continue;

		if (zstream_producer_handle(&prod, &msg) != ZSTREAM_EVENT_PULL)
			continue;	// stale/retry pull, already handled internally

		int n = fs_read_chunk(handle, chunk, sizeof(chunk));

		if (n < 0) {
			zstream_send_error(&prod, "local read failed");
			producer_ok = false;
			break;
		}

		if (n == 0) {
			zstream_send_eof(&prod);
			break;	// our part is done -- net finishes talking to the server on its own
		}

		zstream_send_chunk(&prod, chunk, (uint32_t)n);
		start = z_uptime_ticks();

	}

	fs_close_handle(handle);

	if (!have_stream)
		ms_log(MS_PANIC, "tput: no reply from net after 10s -- is it running? (`run net`)");

	if (!producer_ok)
		ms_log(MS_PANIC, "tput: failed sending local data");

	z_msg_t reply;
	if (!zapi_msg_wait_timeout(&reply, Z_NET_TFTP_PUT_REPLY, tag, ZAPI_TFTP_TIMEOUT_TICKS))
		ms_log(MS_PANIC, "tput: no reply from net after 10s -- is it running? (`run net`)");

	z_obj_t *ok = z_map_find(&reply.obj, "ok");
	if (ok && ok->val.uint32) return ms_mk_bool(true);

	z_obj_t *e = z_map_find(&reply.obj, "error");
	ms_log(MS_PANIC, "tput: failed: %s",
		(e && e->type == Z_STR && e->val.str) ? e->val.str : "unknown error");

	return ms_mk_bool(false);	// unreachable -- ms_log(MS_PANIC, ...) never returns

}

// -- returning structured (nested) values to Scheme --
//
// ms.c's embedder API (ms_api.h) hands out the scalar constructors
// (ms_mk_num/_str/_bool/_nil) plus exactly one composite,
// ms_mk_str_list() -- and its own comment is explicit that this is
// deliberate: building anything bigger than one cell from OUTSIDE ms.c
// means getting GC protection right by hand across several
// allocations, which is precisely what it doesn't want every embedder
// re-deriving. There is no exposed `cons`, so there is no way to build
// a list of lists with what's there.
//
// Rather than patch ms.c to expose PUSH/POP (or a builder API) purely
// so `ps` and `free` can return tables, these two build their result
// as Scheme SOURCE TEXT and hand it to ms_read() -- which is already
// public, already used by repl.c's own eval path, and returns exactly
// the parsed structure we want. ms_read() does all its own GC
// protection internally, so this is safe by construction rather than
// by careful hand-auditing.
//
// The tradeoffs, honestly: it costs a snprintf() pass and a parse over
// a small buffer, and it's only safe for data we ourselves format
// (every value below is a number or a fixed label -- no user-supplied
// string is ever interpolated, so nothing can inject syntax). For the
// sizes involved -- at most Z_PROCS_MAX rows of six numbers -- that is
// far cheaper in CODE SIZE than the alternative, which matters here:
// see docs/scheme_api.md's own note on this app's memory budget. If a
// future API needs to return large or user-derived nested data, this
// is the wrong tool and a real builder in ms.c is the right one.
static ms_val *zapi_read_form(const char *src) {
	const char *p = src;
	ms_val *v = ms_read(&p);
	// a parse failure here would mean this file formatted its own
	// buffer wrong (or overflowed it) -- an empty list is a safer
	// answer than NULL, which callers of a builtin never expect.
	return v ? v : ms_nil_val();
}

// -- Processes --

// how many process rows one (ps) call can report. Matches the kernel's
// own Z_PROCS_MAX (sw/os/kernel.h) -- redefined here rather than
// included, because kernel.h is KERNEL-side code (it pulls in the
// process table, the syscall handler declarations, and fs.h's
// kernel-native prototypes, which collide with the app-facing ones in
// zfsapp.h -- see zeitlos.h's own note on exactly that collision).
// If the kernel's table ever grows past this, the two must be updated
// together -- but so must every already-flashed binary, since the
// syscall's own arg struct is shared, so this is not a case where one
// side can drift silently.
#define ZAPI_PS_MAX 16

// (ps) -- a snapshot of the process table as a list of lists, one row
// per live process:
//
//   ((pid base size pc sp flags) ...)
//
// Same information sh.c's own `ps` prints (k_proc_dump(), which stays
// exactly as it is) -- but as data, so it can be filtered, sorted, or
// fed to (kill ...). Values are plain numbers, decimal rather than the
// hex the console dump uses: hex is right for reading addresses by
// eye, numbers are right for a caller doing arithmetic, and a caller
// that wants hex can format it.
//
// The pid is the process TABLE INDEX, which is what (kill ...),
// k_proc_base() and k_proc_kill() all take -- so a pid from here can
// go straight back into (kill ...) with no translation.
static ms_val *zapi_ps(ms_val *args) {

	(void)args;

	z_proc_info_t procs[ZAPI_PS_MAX];
	uint32_t truncated = 0;
	uint32_t n = z_proc_list(procs, ZAPI_PS_MAX, &truncated);
	// `truncated` is deliberately not surfaced to Scheme: it can only
	// be set if the kernel's own table outgrew ZAPI_PS_MAX, which the
	// comment on that constant explains is a rebuild-both situation,
	// not a runtime condition a script could usefully handle.
	(void)truncated;

	// worst case: Z_PROCS_MAX rows of six 10-digit numbers plus
	// separators. Sized generously and bounds-checked below anyway.
	char buf[ZAPI_PS_MAX * 72 + 8];
	uint32_t o = 0;

	buf[o++] = '(';

	for (uint32_t i = 0; i < n && o < sizeof(buf) - 80; i++) {
		o += (uint32_t)snprintf(buf + o, sizeof(buf) - o,
			"(%lu %lu %lu %lu %lu %lu)",
			(unsigned long)procs[i].pid, (unsigned long)procs[i].base,
			(unsigned long)procs[i].size, (unsigned long)procs[i].pc,
			(unsigned long)procs[i].sp, (unsigned long)procs[i].flags);
	}

	buf[o++] = ')';
	buf[o] = 0;

	return zapi_read_form(buf);

}

// (run "name") -- starts a process from a file on the filesystem (bare
// name, no path or extension: "net", not "/NET.BIN"), exactly as sh.c's
// own `run` and wm's dock do. Returns the new pid, or #f if it
// couldn't start (file missing/empty, no free process slot, or not
// enough memory in the pool -- (free) below is the thing to check
// next if this returns #f unexpectedly).
//
// #f rather than a raised error, deliberately: unlike the networking
// procedures below (where losing the specific reason costs the caller
// real diagnostic information), "it didn't start" is a single, plainly
// visible outcome that a script may well want to branch on -- the same
// distinction file-size/read-file already draw against the window
// procedures.
static ms_val *zapi_run(ms_val *args) {
	const char *name = zapi_arg_str(ms_car(args), "run");
	uint32_t pid = z_proc_run(name);
	if (!pid) return ms_mk_bool(false);
	return ms_mk_num(pid);
}

// (kill pid) -- #t/#f. No ownership check anywhere in this path: any
// process can kill any other, the same trust model the rest of this
// kernel has (see z_proc_kill()'s own comment, zeitlos.h).
//
// Killing repl's own pid works and is exactly as final as it sounds --
// there's no confirmation step here, and (getpid) is right there if a
// script wants to check first.
static ms_val *zapi_kill(ms_val *args) {
	int pid = zapi_arg_int(ms_car(args), "kill");
	if (pid <= 0) return ms_mk_bool(false);
	return ms_mk_bool(z_proc_kill((uint32_t)pid) == Z_OK);
}

// (uptime) -- ticks since boot, as a number.
//
// This replaces the old BUILTIN `uptime` command, which printed a
// fixed string and gave a caller nothing to compute with. Ticks rather
// than seconds because ticks are what the hardware actually counts
// (the KTIMER IRQ, ~732Hz -- see z_uptime_ticks(), zeitlos.c); dividing
// to seconds in Scheme is one obvious expression, while recovering
// ticks from a pre-rounded seconds value isn't possible at all. The
// counter is 32-bit and wraps after roughly 68 days of uptime.
static ms_val *zapi_uptime(ms_val *args) {
	(void)args;
	return ms_mk_num(z_uptime_ticks());
}

// (delay-ms n) -- busy-waits at least n milliseconds, then returns #t.
//
// BLOCKS THIS ENTIRE PROCESS, which on `repl` means every other
// connected `term` window stops being serviced for the duration, not
// just the one that ran this -- repl's single main loop drains one
// shared mailbox (see repl.c). There is no sleep/yield primitive in
// this OS at all, so this genuinely burns cycles rather than giving
// them up; delay_ms() (zeitlos.c) is a spin on z_uptime_ticks(). Fine
// for pacing a short animation or a retry loop, actively antisocial
// for anything long.
static ms_val *zapi_delay_ms(ms_val *args) {
	int ms = zapi_arg_int(ms_car(args), "delay-ms");
	if (ms > 0) delay_ms((uint32_t)ms);
	return ms_mk_bool(true);
}

// -- Memory --

// (free) -- an association list of memory figures, all in BYTES except
// the two cell counts:
//
//   (("scheme-cells-used" N) ("scheme-cells-total" N)
//    ("scheme-bytes" N) ("c-heap" N) ("static" N)
//    ("mem-total" N) ("mem-used" N) ("mem-free" N)
//    ("mem-largest-free" N) ("mem-used-blocks" N)
//    ("mem-free-blocks" N) ("mem-blocks-used" N) ("mem-blocks-max" N))
//
// so (cadr (assoc "mem-free" (free))) gets one figure out.
//
// TWO DIFFERENT THINGS are reported here and conflating them is the
// easy mistake this layout exists to prevent:
//
//   - the "scheme-"/"c-heap"/"static" figures describe THIS PROCESS's
//     own footprint inside the block it was already given -- the same
//     numbers the old builtin `free` command showed.
//   - the "mem-" figures describe the KERNEL POOL (k_mem_alloc(),
//     sw/os/mem.c): the memory whole processes get carved out of, and
//     what sh.c's own `free` shows. This is what determines whether
//     the next (run ...) succeeds.
//
// A process can be comfortable while the pool is nearly exhausted, and
// vice versa; neither number predicts the other.
//
// If Scheme failed to initialize at boot the three "scheme-" figures
// are reported as 0 rather than omitted, so the shape of the returned
// list never varies and a caller's (assoc ...) can't suddenly fail.
static ms_val *zapi_free(ms_val *args) {

	(void)args;

	// _end/_start and sbrk(0): the same linker-provided symbols and
	// the same "just tell me where the break is" idiom the old builtin
	// used -- see docs/app_runtime.md. _end is the top of this
	// process's static footprint, _start its fixed base (0x80000000,
	// riscv-app.ld); the gap between sbrk(0) and _end is everything
	// malloc()'d since boot, which for repl is dominated by ms's own
	// T_STR/T_VECTOR payloads.
	extern char _end, _start;
	uint32_t static_footprint = (uint32_t)&_end - (uint32_t)&_start;
	uint32_t heap_grown = (uint32_t)sbrk(0) - (uint32_t)&_end;

	// No "is Scheme ready?" check here, unlike the old builtin `free`
	// command this replaces: that ran from repl.c's dispatcher, which
	// is reachable whether or not Scheme came up. This is a Scheme
	// PROCEDURE -- if it's executing at all, the interpreter it would
	// be reporting on is demonstrably working.
	long used = ms_heap_used();
	long total = MS_HEAP_SIZE;
	uint32_t cell_bytes = (uint32_t)ms_cell_size();

	z_mem_stats_args_t m;
	memset(&m, 0, sizeof(m));
	z_mem_stats(&m);	// all-zero figures on failure -- see above on
						// why the list's shape stays fixed regardless

	char buf[640];
	snprintf(buf, sizeof(buf),
		"((\"scheme-cells-used\" %ld) (\"scheme-cells-total\" %ld)"
		" (\"scheme-bytes\" %lu) (\"c-heap\" %lu) (\"static\" %lu)"
		" (\"mem-total\" %lu) (\"mem-used\" %lu) (\"mem-free\" %lu)"
		" (\"mem-largest-free\" %lu) (\"mem-used-blocks\" %lu)"
		" (\"mem-free-blocks\" %lu) (\"mem-blocks-used\" %lu)"
		" (\"mem-blocks-max\" %lu))",
		used, total,
		(unsigned long)((uint32_t)used * cell_bytes),
		(unsigned long)heap_grown, (unsigned long)static_footprint,
		(unsigned long)m.total, (unsigned long)m.used,
		(unsigned long)m.free, (unsigned long)m.largest_free,
		(unsigned long)m.used_blocks, (unsigned long)m.free_blocks,
		(unsigned long)m.blocks_used, (unsigned long)m.blocks_max);

	return zapi_read_form(buf);

}

// -- Files (continued) --

// (mkdir "path") -- #t/#f. (touch-file "path") -- creates an empty
// file, #t/#f.
//
// `touch-file`, not `touch`: R4RS has no `touch`, but "touch" is a
// tempting name for a caller to bind themselves, and more importantly
// the bare-word command syntax (docs/scheme_api.md \S1) means ANY bound
// callable becomes a typeable command -- so a short, generic name here
// is a name taken away from the user's own global environment for
// good. The `-file` suffix also matches the existing read-file/
// write-file/delete-file family, which is what a reader will expect
// this to sit alongside.
static ms_val *zapi_mkdir(ms_val *args) {
	const char *path = zapi_arg_str(ms_car(args), "mkdir");
	return ms_mk_bool(fs_mkdir(path) != 0);
}

static ms_val *zapi_touch_file(ms_val *args) {
	const char *path = zapi_arg_str(ms_car(args), "touch-file");
	return ms_mk_bool(fs_touch(path) != 0);
}

// (load "file.l") -- reads a file and evaluates EVERY form in it
// against the shared global environment, returning #t (or #f if the
// file couldn't be read).
//
// Note this is genuinely more than the (eval (read (read-file ...)))
// it replaces: that evaluates the FIRST form only and silently ignores
// the rest of the file, which is almost never what someone loading a
// script wants.
//
// Implemented with ms_load_string() (already public, see ms_api.h),
// NOT by enabling upstream ms.c's own `load`/`file->str`: those live
// inside `#ifndef LIX` and go through fopen()/fread(), which is why
// sw/common/ms_platform/fs.h is a deliberately empty stub in this
// build -- there is no stdio file layer here to enable. Routing them
// through Zeitlos's fs_* calls instead would mean putting
// Zeitlos-specific I/O inside ms.c, which is exactly the kind of
// change that makes a submodule harder to upstream rather than
// easier. So: no ms.c patch needed for this at all.
//
// A parse or evaluation error inside the loaded file raises a normal
// Scheme panic, caught by repl.c's existing recovery -- with whatever
// forms already ran having already taken effect. There is no
// transactional "load it all or none of it" behavior, same as any
// other Scheme's `load`.
static ms_val *zapi_load(ms_val *args) {

	const char *name = zapi_arg_str(ms_car(args), "load");

	int sz = fs_size((char *)name);
	if (sz <= 0) return ms_mk_bool(false);

	char *raw = fs_mallocfile((char *)name);
	if (!raw) return ms_mk_bool(false);

	// fs_mallocfile() returns exactly `sz` raw bytes and does NOT
	// NUL-terminate -- same contract read-file above documents, and
	// ms_load_string() needs a real C string.
	char *s = malloc((size_t)sz + 1);
	if (!s) { free(raw); return ms_mk_bool(false); }
	memcpy(s, raw, (size_t)sz);
	s[sz] = 0;
	free(raw);

	// ms_load_string() can panic (a malformed form, an error inside
	// the file). That longjmp's back to repl.c's recovery point,
	// skipping the free() below -- a bounded, one-off leak of this
	// file's text on a failed load, accepted rather than worked around
	// with a setjmp() here: ms_api.h's own contract is explicit that
	// the protected region has to live in the frame that goes on to
	// call ms_eval(), and adding a second recovery point inside a
	// builtin would fight the one repl.c already establishes.
	ms_load_string(s, ms_global_env);
	free(s);

	return ms_mk_bool(true);

}

// (print-console x) -- prints to the SERIAL CONSOLE, deliberately, no
// matter where this process's stdout is currently pointed.
//
// The console counterpart to ordinary `print`: since repl now
// redirects stdout to the requesting `term` connection while a command
// runs (z_stdout_hook, sw/common/zeitlos.h -- see repl.c's own
// repl_stdout_hook()), `display`/`print` correctly show text to the
// person who typed the command. This one deliberately does the
// opposite, which is what you want when the console is a second window
// onto a running system: tracing what a procedure does without that
// trace scrolling through the output you're trying to read, watching a
// long-running loop while a `term` window shows only its result, or
// getting anything at all out of code that runs with no connection
// attached.
//
// Semantics match `print` (ms.c's bi_print), NOT `display` -- the name
// sets the expectation and the behavior follows it: a trailing newline
// is added, non-string values print in READABLE form (strings inside a
// list come out quoted, the way `write` does), a string argument
// prints raw, and no argument at all emits just a newline. Returns #f,
// same as bi_print. The automatic newline is also the right default
// for the debugging this exists for -- a trace line that needs an
// explicit "\n" every time is a trace line that eventually won't have
// one.
//
// Implemented by writing to stderr rather than by temporarily removing
// the hook. Both would reach the UART, but the hook dance has a real
// hazard: stdout is line-buffered, so bytes from an earlier `display`
// may still be sitting inside libc, and flushing them with the hook
// removed would misroute that earlier output to the console. stderr is
// unbuffered and _write() only ever redirects fd 1, so this needs no
// coordination with the capture buffer at all and can't reorder or
// steal anything already in flight. (That path is already proven in
// this build -- ms_log() writes its [info]/[error]/[panic] lines to
// stderr.)
//
// Consequence worth knowing: this shares the console with those
// "[panic] ..." diagnostics. That's the intent -- one debugging
// stream, in the order things actually happened.
static ms_val *zapi_print_console(ms_val *args) {

	if (ms_is_nil(args)) {
		fputs("\n", stderr);
		fflush(stderr);
		return ms_mk_bool(false);
	}

	ms_val *x = ms_car(args);

	if (ms_is_str(x)) {
		fputs(ms_str_val(x), stderr);
	} else {
		// readable = true: `print`'s own convention (bi_print calls
		// ms_print(x, true)), as opposed to display form.
		char *s = ms_to_string(x, true);
		if (s) { fputs(s, stderr); free(s); }
	}

	fputs("\n", stderr);
	fflush(stderr);

	return ms_mk_bool(false);

}

// (df) -- filesystem capacity, as an association list, all figures in
// KILOBYTES:
//
//   (("total-kb" N) ("used-kb" N) ("free-kb" N))
//
// KB rather than bytes because these are 32-bit all the way down and a
// 32GB card's byte count overflows a uint32_t -- see z_fs_df_args_t
// (sw/common/zfs.h). A caller wanting bytes can multiply; a caller
// handed a pre-overflowed number could not have recovered it.
//
// This is the SD card. (free) above is main memory. The two are
// unrelated and the names deliberately match the shell commands
// (`df`/`free`, sw/os/sh.c) that report the same things.
//
// An absent or unmounted card reports all zeros rather than raising --
// "how much space is there" has a truthful answer of "none" in that
// state, and a script polling for a card shouldn't have to catch an
// error to find out it isn't there yet.
static ms_val *zapi_df(ms_val *args) {

	(void)args;

	uint32_t total = 0, freek = 0;
	fs_df(&total, &freek);

	char buf[160];
	snprintf(buf, sizeof(buf),
		"((\"total-kb\" %lu) (\"used-kb\" %lu) (\"free-kb\" %lu))",
		(unsigned long)total, (unsigned long)(total - freek),
		(unsigned long)freek);

	return zapi_read_form(buf);

}

// -- Time --
//
// The wall clock (rtl/rtc.v, sw/common/zrtc.h), as opposed to
// (uptime) further up, which counts ticks since boot. Both are here
// and they answer different questions: uptime is monotonic and is what
// you time things with, this has an epoch and can jump backwards the
// moment net's NTP client lands a correction. Using the wrong one is
// how you get a negative duration.
//
// Everything below is UTC. There is no timezone conversion anywhere in
// Zeitlos yet -- see sw/common/zrtc.h's own note on why that is a
// decision rather than an oversight.

// True only if the clock is both present in this bitstream and has
// actually been set. Both halves are needed and neither implies the
// other: a board can have an RTC nobody has told the time to (the
// normal state for the first few seconds after boot, and the permanent
// state with no network), and a board can have no RTC at all.
//
// z_rtc_available() rather than z_rtc_present(): the latter would HANG
// on a bitstream predating rtl/rtc.v, which is exactly the sort of
// machine somebody might type (current-time) at. See zrtc.h.
static bool zapi_clock_ok(void) {
	return z_rtc_available() && z_rtc_valid();
}

// (current-time) -- seconds since the Unix epoch, UTC, as a number.
// #f if the clock is unavailable or has never been set.
//
// #f rather than a panic, deliberately, and this is the opposite call
// from the one (video-mode ...) makes for missing gateware. Setting
// the display is something the caller asked to DO, and failing it
// silently would hide a reflash they need to know about. Asking what
// time it is is a QUESTION, and "I don't know" is a real answer to it
// -- on a machine with no network that is the permanent, correct,
// entirely unexceptional answer, and blowing up a one-liner over it
// would be obnoxious. It also composes: (if (current-time) ... ).
//
// Seconds rather than the RTC's own sub-second units, unlike
// (uptime)'s ticks -- the reasoning there was that dividing to seconds
// is easy while recovering ticks isn't, and here the raw unit IS
// seconds. The 1/1024s fraction is deliberately not exposed: nothing
// in Scheme runs anywhere near that fast, and a two-element return
// would complicate every caller for the benefit of none of them.
static ms_val *zapi_current_time(ms_val *args) {
	(void)args;
	if (!zapi_clock_ok()) return ms_mk_bool(false);
	return ms_mk_num(z_rtc_seconds());
}

// (current-date) -- the clock broken into calendar fields, UTC:
//
//   (year month day hour minute second weekday yearday)
//
// e.g. (2026 8 27 14 31 2 4 238). month is 1-12 and day 1-31 (not the
// 0-based months a C struct tm uses -- this is a value people read,
// and a 1-based month is what they expect); weekday is 0 for Sunday;
// yearday is 0-365.
//
// #f if the clock is unavailable or unset, same as (current-time) and
// for the same reason.
//
// (current-date t) -- decode an arbitrary Unix timestamp instead of
// the current one. That makes this a general calendar function rather
// than only a clock reading, which matters for the obvious things --
// formatting a file's timestamp, working out what day some computed
// second falls on -- and it needs no RTC at all, so (current-date 0)
// answers (1970 1 1 0 0 0 4 0) even on a board with no clock. The
// optional-argument shape matches (ls)/(ls path) and
// (video-mode)/(video-mode m) elsewhere in this file.
//
// A positional list rather than an association list, unlike (free) and
// (df) above. Those return a dozen unrelated figures where a name is
// the only thing telling them apart; a date is eight fields in an
// order every calendar has used for a very long time, and
// (cadr (assoc "hour" (current-date))) would be a worse way to ask for
// the hour than (list-ref (current-date) 3).
//
// Numbers rather than a preformatted string, matching (ps) and
// (uptime): a string is one str-append away from a list of numbers,
// and a list of numbers cannot be recovered from a string without
// parsing it back.
static ms_val *zapi_current_date(ms_val *args) {

	uint32_t t;

	if (!ms_is_nil(args)) {
		int n = zapi_arg_int(ms_car(args), "current-date");
		if (n < 0)
			ms_log(MS_PANIC, "current-date: timestamp is before the "
				"epoch (want 0 or more)");
		t = (uint32_t)n;
	} else {
		if (!zapi_clock_ok()) return ms_mk_bool(false);
		t = z_rtc_seconds();
	}

	z_tm_t tm;
	z_time_to_tm(t, &tm);

	char buf[80];
	snprintf(buf, sizeof(buf), "(%ld %d %d %d %d %d %d %d)",
		(long)tm.year, tm.month, tm.day, tm.hour, tm.min, tm.sec,
		tm.wday, tm.yday);

	// Source text handed to ms_read(), the same way (ps)/(free)/(df)
	// build their results -- see zapi_read_form()'s own comment. Every
	// value here is a number this file formatted itself, so there is
	// nothing a caller could inject.
	return zapi_read_form(buf);

}

// -- registration --

// (video-mode) -- current virtual phosphor mode as a string, one of
// "white", "amber", "green", "paper".
//
// (video-mode "amber") or (video-mode 1) -- set it, returning the mode
// actually in effect afterwards (again as a string), NOT #t. Returning
// the resulting state rather than a success flag means the common
// interactive case reads the same either way -- (video-mode) and
// (video-mode "amber") both answer "what is the screen doing now" --
// and a caller that wants to check can compare against what it asked
// for.
//
// Both a name and a number are accepted because both are natural here:
// a person types the name, and generated or looping code ((video-mode
// (modulo n 4))) wants the number. An unrecognised name or an
// out-of-range number is a panic, like every other bad argument in
// this file, rather than silently falling back to white -- a typo that
// quietly reset the display would be a confusing thing to debug.
//
// One getter/setter procedure rather than a video-mode/video-mode!
// pair: this is a single register with no compound state, so there is
// nothing a separate setter would clarify. Not named `color` -- that
// word already means the 1-bit pixel value in (line ...), (box ...)
// and (text ...) just above, and reusing it for something screen-wide
// would be actively misleading.
static ms_val *zapi_video_mode(ms_val *args) {

	if (!ms_is_nil(args)) {

		ms_val *a = ms_car(args);
		uint32_t mode;

		if (ms_is_str(a)) {
			const char *name = ms_str_val(a);
			mode = z_video_mode_from_name(name);
			if (mode >= Z_VIDEO_MODE_COUNT)
				ms_log(MS_PANIC, "video-mode: unknown mode '%s' "
					"(want white, amber, green or paper)", name);
		}
		else if (ms_is_num(a)) {
			double n = ms_num_val(a);
			if (n < 0 || n >= (double)Z_VIDEO_MODE_COUNT)
				ms_log(MS_PANIC, "video-mode: mode out of range "
					"(want 0..%d)", (int)Z_VIDEO_MODE_COUNT - 1);
			mode = (uint32_t)n;
		}
		else {
			ms_log(MS_PANIC, "video-mode: expected a string or a number");
			return ms_mk_bool(false);	// not reached -- ms_log(MS_PANIC)
										// longjmps out, see this file's
										// own header comment
		}

		// A failure here is gateware that predates the register, not a
		// bad argument -- worth saying so explicitly, because the fix
		// is a reflash rather than anything the caller can change.
		if (!z_video_mode_set(mode))
			ms_log(MS_PANIC, "video-mode: this bitstream has no video "
				"mode register (needs `make flash`)");

	}

	// Read back rather than returning the requested mode, for the same
	// reason sh.c's `color` does: it reports the screen's actual state.
	char *s = strdup(z_video_mode_name(z_video_mode_get()));
	if (!s) ms_log(MS_PANIC, "video-mode: out of memory");

	return ms_mk_str(s);	// takes ownership

}

// -- Game mode and gamepads --

// (game-mode)         -- current mode as a string: "off", "on" or
//                        "wrap"
// (game-mode "wrap")  -- set it, returning the mode actually in effect
// (game-mode 2)       -- same, by number
//
// Deliberately the same SHAPE as (video-mode) above -- one
// getter/setter, a name or a number accepted, the resulting state
// returned rather than #t -- because it is the same kind of thing: a
// single screen-wide register with no compound state. Two procedures
// that behave alike are worth more than two that each fit their own
// register slightly better.
//
// Names rather than #t/#f even though the underlying enable is a
// single bit, because there are genuinely three states a person cares
// about here. Wrap is a separate bit in the hardware, but "game mode,
// clamped" and "game mode, toroidal" are different enough to a caller
// (see docs/game_mode.md) that folding them into one boolean and a
// second procedure would make the common case -- turning it on the way
// a scrolling game wants -- take two calls instead of one.
//
// Entering game mode from the REPL is a genuinely useful thing to do
// interactively; it is how you see what the viewport does without
// writing a program. It is also a good way to lose sight of the
// terminal you typed it into, since the REPL's own window may well be
// outside the 320x240 view. Alt+Esc (sw/apps/wm) toggles back
// regardless of what any program has done, which is exactly why that
// hotkey lives in the window manager rather than in a library.
#define ZAPI_GAME_OFF  0u
#define ZAPI_GAME_ON   1u
#define ZAPI_GAME_WRAP 2u
#define ZAPI_GAME_COUNT 3u

static const char *zapi_game_mode_name(uint32_t m) {
	switch (m) {
		case ZAPI_GAME_OFF:  return "off";
		case ZAPI_GAME_ON:   return "on";
		case ZAPI_GAME_WRAP: return "wrap";
		default: return "unknown";
	}
}

static ms_val *zapi_game_mode(ms_val *args) {

	if (!ms_is_nil(args)) {

		ms_val *a = ms_car(args);
		uint32_t mode;

		if (ms_is_str(a)) {
			const char *s = ms_str_val(a);
			if (s[0] == 'o' && s[1] == 'f' && s[2] == 'f' && s[3] == '\0')
				mode = ZAPI_GAME_OFF;
			else if (s[0] == 'o' && s[1] == 'n' && s[2] == '\0')
				mode = ZAPI_GAME_ON;
			else if (s[0] == 'w' && s[1] == 'r' && s[2] == 'a' &&
				s[3] == 'p' && s[4] == '\0')
				mode = ZAPI_GAME_WRAP;
			else {
				ms_log(MS_PANIC, "game-mode: unknown mode '%s' "
					"(want off, on or wrap)", s);
				return ms_mk_bool(false);	// not reached -- see this
											// file's header comment
			}
		}
		else if (ms_is_num(a)) {
			double n = ms_num_val(a);
			if (n < 0 || n >= (double)ZAPI_GAME_COUNT)
				ms_log(MS_PANIC, "game-mode: mode out of range "
					"(want 0..%d)", (int)ZAPI_GAME_COUNT - 1);
			mode = (uint32_t)n;
		}
		else {
			ms_log(MS_PANIC, "game-mode: expected a string or a number");
			return ms_mk_bool(false);	// not reached
		}

		// A failure is gateware without game mode, not a bad
		// argument -- worth saying so, because the fix is a reflash
		// rather than anything the caller can change. Same treatment
		// (video-mode) gives the same situation.
		if (!z_game_set_enabled(mode != ZAPI_GAME_OFF,
			mode == ZAPI_GAME_WRAP))
			ms_log(MS_PANIC, "game-mode: this bitstream has no game "
				"mode (needs `make flash`)");

	}

	// Read back from the hardware rather than returning what was
	// asked for, for the same reason (video-mode) does: it reports the
	// screen's actual state.
	{
		uint32_t cur = !z_game_enabled() ? ZAPI_GAME_OFF :
			(z_game_wrap_enabled() ? ZAPI_GAME_WRAP : ZAPI_GAME_ON);
		char *s = strdup(zapi_game_mode_name(cur));
		if (!s) ms_log(MS_PANIC, "game-mode: out of memory");
		return ms_mk_str(s);	// takes ownership
	}

}

// (game-view)       -- the viewport origin, as a list (x y)
// (game-view x y)   -- move it, returning the origin afterwards
//
// Coordinates are FRAMEBUFFER pixels -- the same space every drawing
// procedure in this file already uses, not viewport-relative ones.
//
// The value read back is what was WRITTEN, which is not always what is
// being scanned out: with wrap off the hardware clamps the origin to
// (320,240) so the viewport cannot hang off the edge, and that clamp
// is applied at scanout rather than on the write path. See
// docs/game_mode.md.
static ms_val *zapi_game_view(ms_val *args) {

	char buf[64];

	if (!ms_is_nil(args)) {

		ms_val *ax = ms_car(args);
		ms_val *rest = ms_cdr(args);
		double x, y;

		if (ms_is_nil(rest))
			ms_log(MS_PANIC, "game-view: expected two arguments (x y)");

		if (!ms_is_num(ax) || !ms_is_num(ms_car(rest)))
			ms_log(MS_PANIC, "game-view: expected numbers");

		x = ms_num_val(ax);
		y = ms_num_val(ms_car(rest));

		if (x < 0 || x > 639 || y < 0 || y > 479)
			ms_log(MS_PANIC, "game-view: out of range "
				"(want 0..639, 0..479)");

		if (!z_game_set_view((uint32_t)x, (uint32_t)y))
			ms_log(MS_PANIC, "game-view: this bitstream has no game "
				"mode (needs `make flash`)");

	}

	snprintf(buf, sizeof(buf), "'(%lu %lu)",
		(unsigned long)z_game_get_view_x(),
		(unsigned long)z_game_get_view_y());

	return zapi_read_form(buf);

}

// (game-frame) -- the display's own frame counter, 0..65535.
//
// Wraps every ~18 minutes at 60Hz. Compare for inequality or subtract;
// do not test with `>`.
static ms_val *zapi_game_frame(ms_val *args) {
	(void)args;
	return ms_mk_num(z_game_frame());
}

// (game-wait) -- block until the next frame boundary, returning #t.
//
// BLOCKS THIS ENTIRE PROCESS for up to 16.7ms, with the same
// consequences (delay-ms) has and for the same reason -- see its own
// comment. On `repl` that means every connected `term` window stops
// being serviced for the duration. Fine for a one-shot at a prompt;
// a Scheme loop calling this sixty times a second is not what this
// interpreter is for, and the answer to wanting that is a C program.
static ms_val *zapi_game_wait(ms_val *args) {
	(void)args;
	z_game_wait_frame();
	return ms_mk_bool(true);
}

// (gamepad-count) -- how many USB gamepads are attached right now, 0-2.
//
// Reads the hardware every call rather than caching, so plugging a pad
// in and asking again just works. See docs/gamepad.md on why nothing
// binds a device to a particular port.
static ms_val *zapi_gamepad_count(ms_val *args) {
	(void)args;
	return ms_mk_num(z_pad_count());
}

// (gamepad)   -- state of pad 0
// (gamepad n) -- state of pad n (0 or 1)
//
// Returns a list of the symbols currently pressed, e.g. (right a), or
// the empty list if nothing is. Returns #f -- NOT the empty list --
// when there is no such pad, because "no pad" and "a pad with nothing
// pressed" are genuinely different answers and a caller writing a
// "press start" screen needs to tell them apart.
//
// Symbols rather than a bitmask because this is the interactive layer:
// `(memq 'a (gamepad))` reads as what it means, whereas
// `(> (bitwise-and (gamepad) 16) 0)` requires a table to understand
// and this interpreter has no bitwise operations anyway.
//
// `n` is a PAD INDEX, not a port: pad 0 is whichever port currently
// holds a gamepad. Unplug the pad from port 0 while a second sits in
// port 1 and the survivor becomes pad 0.
static ms_val *zapi_gamepad(ms_val *args) {

	static const struct { uint32_t bit; const char *name; } names[] = {
		{ Z_PAD_LEFT,   "left"   },
		{ Z_PAD_RIGHT,  "right"  },
		{ Z_PAD_UP,     "up"     },
		{ Z_PAD_DOWN,   "down"   },
		{ Z_PAD_A,      "a"      },
		{ Z_PAD_B,      "b"      },
		{ Z_PAD_X,      "x"      },
		{ Z_PAD_Y,      "y"      },
		{ Z_PAD_SELECT, "select" },
		{ Z_PAD_START,  "start"  },
	};

	int pad = 0;
	uint32_t st;
	char buf[128];
	size_t n = 0;

	if (!ms_is_nil(args)) {
		ms_val *a = ms_car(args);
		if (!ms_is_num(a))
			ms_log(MS_PANIC, "gamepad: expected a pad number");
		double d = ms_num_val(a);
		if (d < 0 || d >= Z_PAD_MAX_PORTS)
			ms_log(MS_PANIC, "gamepad: pad out of range (want 0..%d)",
				Z_PAD_MAX_PORTS - 1);
		pad = (int)d;
	}

	if (!z_pad_present(pad)) return ms_mk_bool(false);

	st = z_pad_read(pad);

	// Built as source text and handed to ms_read(), the same way (ps)
	// and (df) build their results -- see zapi_read_form(). Every
	// character here comes from the table above, so there is nothing a
	// caller could inject.
	n += (size_t)snprintf(buf + n, sizeof(buf) - n, "'(");
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!(st & names[i].bit)) continue;
		n += (size_t)snprintf(buf + n, sizeof(buf) - n, "%s%s",
			(n > 2) ? " " : "", names[i].name);
	}
	snprintf(buf + n, sizeof(buf) - n, ")");

	return zapi_read_form(buf);

}

// -- GPIO --
//
// rtl/gpio.v, via sw/common/zgpio.h. See docs/gpio.md.
//
// PORTS AND PINS ARE NUMBERS, both 0-based, and every procedure here
// takes them as two separate arguments. That is not just consistency
// with the C API -- it is what makes bare-word syntax work without
// quoting:
//
//     gpio-set 0 3 #t
//
// reaches (gpio-set 0 3 #t) directly, because repl's translator passes
// a token that parses wholly as a number through unquoted (see
// docs/scheme_api.md's argument translation table). A single-token pin
// name would have arrived as the string "0.3" and needed unpacking in
// every procedure below.
//
// Names mirror the REGISTERS for the whole-port procedures -- (gpio-dir),
// (gpio-out), (gpio-in) -- because those are what docs/gpio.md
// documents and what someone reading a register dump is holding in
// their head. The per-pin procedures use plain verbs instead, since at
// that level nobody is thinking about registers.

// Shared argument checking. Range is checked against what this
// BITSTREAM actually built, not against the map's reserved maximum:
// a write to a port that does not exist is silently dropped by the
// hardware (rtl/gpio.v, and there is no way to report an error on that
// bus), so this is the only place a caller can find out. Panicking is
// right rather than harsh -- a Scheme user who typed the wrong port
// number wants to be told, not to watch nothing happen.
static void zapi_gpio_check(int port, int pin, const char *who) {

	uint32_t n = z_gpio_port_count();

	if (n == 0)
		ms_log(MS_PANIC, "%s: this bitstream has no gpio "
			"(needs different gateware -- see docs/gpio.md)", who);

	if (port < 0 || (uint32_t)port >= n)
		ms_log(MS_PANIC, "%s: no port %d on this board (have 0..%d)",
			who, port, (int)n - 1);

	// pin < 0 is the check that matters here; a pin is not a thing
	// that varies per board, so 0..7 is always the answer.
	if (pin < 0 || pin >= Z_GPIO_PINS_PER_PORT)
		ms_log(MS_PANIC, "%s: no pin %d (a port has 0..%d)",
			who, pin, Z_GPIO_PINS_PER_PORT - 1);

}

// Same, for the whole-port procedures, which have no pin.
static void zapi_gpio_check_port(int port, const char *who) {
	zapi_gpio_check(port, 0, who);
}

// (gpio-ports) -- how many GPIO ports this bitstream has pins for.
//
// 0 on a board without GPIO, which makes this the presence test too:
// there is no separate (gpio?), because "how many" already answers
// "any" and one procedure is easier to remember than two.
static ms_val *zapi_gpio_ports(ms_val *args) {
	(void)args;
	return ms_mk_num((double)z_gpio_port_count());
}

// (gpio-dir p)        -- the DIR register, 1 bit per pin, 1 = output
// (gpio-dir p mask)   -- set it, returning what it reads back as
static ms_val *zapi_gpio_dir(ms_val *args) {

	int port;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-dir: expected a port");

	port = zapi_arg_int(ms_car(args), "gpio-dir");
	zapi_gpio_check_port(port, "gpio-dir");

	args = ms_cdr(args);

	if (!ms_is_nil(args)) {
		int mask = zapi_arg_int(ms_car(args), "gpio-dir");
		if (mask < 0 || mask > 255)
			ms_log(MS_PANIC, "gpio-dir: mask out of range (want 0..255)");
		z_gpio_dir_set((uint32_t)port, (uint8_t)mask);
	}

	// Read back rather than echo, the same way (video-mode) and
	// (game-mode) do: it reports what the hardware is actually doing.
	return ms_mk_num((double)z_gpio_dir_get((uint32_t)port));

}

// (gpio-out p)       -- the OUT register
// (gpio-out p v)     -- set it, returning what it reads back as
//
// NOT the pin state. On a pin configured as an input this is the value
// staged for whenever it becomes an output; (gpio-in) is what the wire
// is actually doing.
static ms_val *zapi_gpio_out(ms_val *args) {

	int port;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-out: expected a port");

	port = zapi_arg_int(ms_car(args), "gpio-out");
	zapi_gpio_check_port(port, "gpio-out");

	args = ms_cdr(args);

	if (!ms_is_nil(args)) {
		int v = zapi_arg_int(ms_car(args), "gpio-out");
		if (v < 0 || v > 255)
			ms_log(MS_PANIC, "gpio-out: value out of range (want 0..255)");
		z_gpio_out_put((uint32_t)port, (uint8_t)v);
	}

	return ms_mk_num((double)z_gpio_out_get((uint32_t)port));

}

// (gpio-in p) -- the pins, all eight, as a number.
//
// Read-only, unlike the two above: there is no writing to a pin.
// With nothing connected this reads 255, because every pin has a weak
// pull-up (docs/gpio.md) -- that is a working port at rest, not a
// fault.
static ms_val *zapi_gpio_in(ms_val *args) {

	int port;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-in: expected a port");

	port = zapi_arg_int(ms_car(args), "gpio-in");
	zapi_gpio_check_port(port, "gpio-in");

	return ms_mk_num((double)z_gpio_in_get((uint32_t)port));

}

// (gpio-mode p n)          -- "in" or "out"
// (gpio-mode p n "out")    -- set it; also "in" and "od"
//
// Returns the mode read back from DIR, which is why asking after
// setting "od" answers "in": open drain is not a hardware mode and
// there is nowhere to record it (see sw/common/zgpio.h). A pin set to
// "od" is an input that (gpio-od) will drive low on demand, and that
// is exactly what DIR says about it.
static ms_val *zapi_gpio_mode(ms_val *args) {

	int port, pin;
	char *s;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-mode: expected a port");
	port = zapi_arg_int(ms_car(args), "gpio-mode");
	args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-mode: expected a pin");
	pin = zapi_arg_int(ms_car(args), "gpio-mode");
	args = ms_cdr(args);

	zapi_gpio_check(port, pin, "gpio-mode");

	if (!ms_is_nil(args)) {

		const char *m = zapi_arg_str(ms_car(args), "gpio-mode");

		if (!strcmp(m, "in"))
			z_gpio_mode((uint32_t)port, (uint32_t)pin, Z_GPIO_IN);
		else if (!strcmp(m, "out"))
			z_gpio_mode((uint32_t)port, (uint32_t)pin, Z_GPIO_OUT);
		else if (!strcmp(m, "od"))
			z_gpio_mode((uint32_t)port, (uint32_t)pin, Z_GPIO_OD);
		else
			ms_log(MS_PANIC, "gpio-mode: unknown mode '%s' "
				"(want in, out or od)", m);

	}

	s = strdup(z_gpio_mode_get((uint32_t)port, (uint32_t)pin) == Z_GPIO_OUT
		? "out" : "in");
	if (!s) ms_log(MS_PANIC, "gpio-mode: out of memory");

	return ms_mk_str(s);	// takes ownership

}

// (gpio-get p n) -- the pin, as #t or #f.
//
// Reads the PIN, not OUT: on an output this is normally what is being
// driven, but if something else is fighting it, this is what actually
// won.
static ms_val *zapi_gpio_get(ms_val *args) {

	int port, pin;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-get: expected a port");
	port = zapi_arg_int(ms_car(args), "gpio-get");
	args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-get: expected a pin");
	pin = zapi_arg_int(ms_car(args), "gpio-get");

	zapi_gpio_check(port, pin, "gpio-get");

	return ms_mk_bool(z_gpio_read((uint32_t)port, (uint32_t)pin));

}

// (gpio-set p n v) -- drive a pin. Returns the pin afterwards.
//
// Does NOT change the mode. A pin still configured as an input will
// not start driving because of this; the value is staged for whenever
// it does. That is why the return value is a read of the PIN rather
// than an echo of `v` -- calling this on an input and getting `v` back
// would be a lie, and it is a mistake worth having reported by the
// value rather than by a silent nothing.
static ms_val *zapi_gpio_set(ms_val *args) {

	int port, pin;
	bool v;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-set: expected a port");
	port = zapi_arg_int(ms_car(args), "gpio-set");
	args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-set: expected a pin");
	pin = zapi_arg_int(ms_car(args), "gpio-set");
	args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-set: expected a value");

	// Accepts #t/#f or 1/0 -- see zapi_arg_truthy(). Both spellings
	// exist in the wild here: #f is what (gpio-get) hands back, so a
	// pin can be mirrored with (gpio-set 0 3 (gpio-get 0 2)), and 1/0
	// is what somebody types who has just been using the shell.
	v = zapi_arg_truthy(ms_car(args));

	zapi_gpio_check(port, pin, "gpio-set");

	z_gpio_write((uint32_t)port, (uint32_t)pin, v);

	return ms_mk_bool(z_gpio_read((uint32_t)port, (uint32_t)pin));

}

// (gpio-toggle p n) -- flip OUT, returning the pin afterwards.
static ms_val *zapi_gpio_toggle(ms_val *args) {

	int port, pin;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-toggle: expected a port");
	port = zapi_arg_int(ms_car(args), "gpio-toggle");
	args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-toggle: expected a pin");
	pin = zapi_arg_int(ms_car(args), "gpio-toggle");

	zapi_gpio_check(port, pin, "gpio-toggle");

	z_gpio_toggle((uint32_t)port, (uint32_t)pin);

	return ms_mk_bool(z_gpio_read((uint32_t)port, (uint32_t)pin));

}

// (gpio-od p n v) -- open-drain write. Returns the pin afterwards.
//
// `v` is THE LEVEL THE LINE ENDS UP AT, not a direction: #f pulls it
// low, #t releases it and lets the pull-up (or whatever else is on the
// bus) decide. That is the mental model of an open-drain bus and it
// matches (gpio-set)'s argument, even though underneath it moves DIR
// rather than OUT.
//
// Which makes the return value more than a formality here: on a
// working bus with a pull-up, (gpio-od p n #t) reads back #t, and on a
// bus where another device is holding the line down it reads back #f.
// That is how you see a stuck I2C slave from a prompt.
//
// Assumes the pin's OUT bit is 0, which (gpio-mode p n "od") arranges
// and which is also the reset state of every pin -- so this is correct
// without any setup on a freshly booted machine.
static ms_val *zapi_gpio_od(ms_val *args) {

	int port, pin;
	bool v;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-od: expected a port");
	port = zapi_arg_int(ms_car(args), "gpio-od");
	args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-od: expected a pin");
	pin = zapi_arg_int(ms_car(args), "gpio-od");
	args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "gpio-od: expected a value");

	v = zapi_arg_truthy(ms_car(args));

	zapi_gpio_check(port, pin, "gpio-od");

	z_gpio_od_write((uint32_t)port, (uint32_t)pin, v);

	return ms_mk_bool(z_gpio_read((uint32_t)port, (uint32_t)pin));

}

// (led)    -- the board LED, as #t/#f
// (led v)  -- set it
//
// Not gated on (gpio-ports): this is rtl/gpio.v's word 0 and it exists
// on every board whether or not any port has pins. It is the block's
// oldest register -- sw/bios/bios.c writes it before anything else in
// the system is alive.
static ms_val *zapi_led(ms_val *args) {

	if (!ms_is_nil(args))
		z_led_set(zapi_arg_truthy(ms_car(args)));

	return ms_mk_bool((reg_gpio_led & 1u) != 0);

}

// (leds)   -- the `LED_DEBUG LED bar, as a number
// (leds v) -- set it
//
// Harmless on a board with no pins for it: the register exists on
// every build and the bits simply go nowhere. Check
// Z_FEATURE_LED_DEBUG (sw/common/zsoc.h) if you need to know whether
// anyone can see them.
static ms_val *zapi_leds(ms_val *args) {

	if (!ms_is_nil(args)) {
		int v = zapi_arg_int(ms_car(args), "leds");
		if (v < 0 || v > 255)
			ms_log(MS_PANIC, "leds: value out of range (want 0..255)");
		z_led_bar_set((uint8_t)v);
	}

	return ms_mk_num((double)(reg_gpio_leds & 0xffu));

}

// -- I2C and SPI (bit-banged over GPIO) --
//
// sw/common/zi2c.h and sw/common/zspi.h. See docs/i2c.md and
// docs/spi.md.
//
// -- Buses are handles, and handles are numbers --
//
// Same convention as window handles (\S4): (i2c-init ...) returns a
// small integer and every other procedure takes it. The alternative --
// passing the pins to every call -- would mean five arguments on every
// read and no place to keep the derived timing, and it would make
// reconfiguring a bus mean remembering to change it everywhere.
//
// -- Scheme buses live on ONE port --
//
// The C API lets each pin be on any port; these procedures take a
// single port and then pin numbers within it. That is not a
// simplification for its own sake: (spi-init) would otherwise need ten
// arguments, and a four-pin SPI device is plugged into one PMOD
// connector essentially always. A bus genuinely spanning two ports is
// a C program.

#define ZAPI_I2C_MAX 2
#define ZAPI_SPI_MAX 2

static z_i2c_t zapi_i2c[ZAPI_I2C_MAX];
static bool zapi_i2c_used[ZAPI_I2C_MAX];

static z_spi_t zapi_spi[ZAPI_SPI_MAX];
static bool zapi_spi_used[ZAPI_SPI_MAX];

static z_i2c_t *zapi_i2c_bus(ms_val *v, const char *who) {
	int h = zapi_arg_int(v, who);
	if (h < 0 || h >= ZAPI_I2C_MAX || !zapi_i2c_used[h])
		ms_log(MS_PANIC, "%s: %d is not an open i2c bus "
			"(use the handle (i2c-init) returned)", who, h);
	return &zapi_i2c[h];
}

static z_spi_t *zapi_spi_bus(ms_val *v, const char *who) {
	int h = zapi_arg_int(v, who);
	if (h < 0 || h >= ZAPI_SPI_MAX || !zapi_spi_used[h])
		ms_log(MS_PANIC, "%s: %d is not an open spi bus "
			"(use the handle (spi-init) returned)", who, h);
	return &zapi_spi[h];
}

// Next positional argument as an int, or `dflt` if the list ran out.
static int zapi_opt_int(ms_val **args, int dflt, const char *who) {
	int v;
	if (ms_is_nil(*args)) return dflt;
	v = zapi_arg_int(ms_car(*args), who);
	*args = ms_cdr(*args);
	return v;
}

// A Scheme list of numbers, or a string, into bytes.
//
// Strings are accepted because (i2c-write bus 60 "hello") is what
// anyone talking to a character display will want to type, and the
// alternative is a list of 5 numbers they have to look up.
static uint32_t zapi_bytes_in(ms_val *v, uint8_t *out, uint32_t cap,
	const char *who) {

	uint32_t n = 0;

	if (ms_is_str(v)) {
		const char *p = ms_str_val(v);
		while (*p && n < cap) out[n++] = (uint8_t)*p++;
		return n;
	}

	while (ms_is_pair(v)) {
		int b;
		if (n >= cap)
			ms_log(MS_PANIC, "%s: at most %lu bytes at a time",
				who, (unsigned long)cap);
		b = zapi_arg_int(ms_car(v), who);
		if (b < 0 || b > 255)
			ms_log(MS_PANIC, "%s: %d is not a byte (want 0..255)", who, b);
		out[n++] = (uint8_t)b;
		v = ms_cdr(v);
	}

	return n;

}

// Bytes back out as a Scheme list, built as source text and read --
// the same approach (ps), (df) and (gamepad) use, and the same
// reasoning: far cheaper in code size than a builder, and everything
// in the buffer is generated here so there is nothing to inject. See
// zapi_read_form().
#define ZAPI_BB_MAX 32

static ms_val *zapi_bytes_out(const uint8_t *b, uint32_t n) {

	char buf[8 + ZAPI_BB_MAX * 5];
	size_t k = 0;
	uint32_t i;

	k += (size_t)snprintf(buf + k, sizeof(buf) - k, "'(");
	for (i = 0; i < n; i++)
		k += (size_t)snprintf(buf + k, sizeof(buf) - k, "%s%u",
			i ? " " : "", (unsigned)b[i]);
	snprintf(buf + k, sizeof(buf) - k, ")");

	return zapi_read_form(buf);

}

// Every i2c procedure that can fail reports the same way: #f, with the
// reason left for (i2c-error). Not a panic, because "nothing answered"
// is an ordinary result on a bus -- it is what a scan is made of and
// what you get for a device that is still busy -- and a procedure that
// blew up the whole expression on a NACK would be unusable in a loop.
//
// Setup mistakes DO panic (a bad handle, a byte out of range), because
// those are the caller's error rather than the bus's.
static z_i2c_rv zapi_i2c_last = Z_I2C_OK;

static ms_val *zapi_i2c_result(z_i2c_rv rv) {
	zapi_i2c_last = rv;
	return ms_mk_bool(rv == Z_I2C_OK);
}

// (i2c-init port scl-pin sda-pin)        -- 100kHz
// (i2c-init port scl-pin sda-pin khz)
//
// Returns a bus handle, or #f if the bus is unusable right now -- both
// lines should be high on an idle bus, and if they are not, nothing
// this returns would work. (i2c-recover) is the thing to try.
static ms_val *zapi_i2c_init(ms_val *args) {

	int port, scl, sda, khz;
	int h;
	z_i2c_rv rv;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "i2c-init: expected a port");
	port = zapi_arg_int(ms_car(args), "i2c-init"); args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "i2c-init: expected an scl pin");
	scl = zapi_arg_int(ms_car(args), "i2c-init"); args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "i2c-init: expected an sda pin");
	sda = zapi_arg_int(ms_car(args), "i2c-init"); args = ms_cdr(args);

	khz = zapi_opt_int(&args, 100, "i2c-init");

	zapi_gpio_check(port, scl, "i2c-init");
	zapi_gpio_check(port, sda, "i2c-init");

	if (scl == sda)
		ms_log(MS_PANIC, "i2c-init: scl and sda cannot be the same pin");

	for (h = 0; h < ZAPI_I2C_MAX; h++) if (!zapi_i2c_used[h]) break;
	if (h == ZAPI_I2C_MAX)
		ms_log(MS_PANIC, "i2c-init: no free bus (at most %d)", ZAPI_I2C_MAX);

	zapi_i2c[h].scl_port = (uint8_t)port;
	zapi_i2c[h].scl_pin = (uint8_t)scl;
	zapi_i2c[h].sda_port = (uint8_t)port;
	zapi_i2c[h].sda_pin = (uint8_t)sda;
	zapi_i2c[h].khz = (uint32_t)(khz < 0 ? 0 : khz);
	zapi_i2c[h].timeout_us = 1000;

	rv = z_i2c_init(&zapi_i2c[h]);
	zapi_i2c_last = rv;

	if (rv != Z_I2C_OK) return ms_mk_bool(false);

	zapi_i2c_used[h] = true;

	return ms_mk_num((double)h);

}

// (i2c-error) -- why the last i2c call failed: "ok", "nack",
// "timeout" or "busy".
//
// The distinction that matters: "nack" means the bus works and nobody
// answered, "timeout" means a released line never came up, which is a
// missing pull-up or a short and no amount of retrying will fix it.
static ms_val *zapi_i2c_error(ms_val *args) {
	char *s;
	(void)args;
	s = strdup(z_i2c_strerror(zapi_i2c_last));
	if (!s) ms_log(MS_PANIC, "i2c-error: out of memory");
	return ms_mk_str(s);
}

// (i2c-scan bus) -- addresses that answered, as a list.
static ms_val *zapi_i2c_scan(ms_val *args) {

	z_i2c_t *b;
	uint8_t found[ZAPI_BB_MAX];
	int n;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "i2c-scan: expected a bus");
	b = zapi_i2c_bus(ms_car(args), "i2c-scan");

	n = z_i2c_scan(b, found, ZAPI_BB_MAX);
	if (n > ZAPI_BB_MAX) n = ZAPI_BB_MAX;

	zapi_i2c_last = Z_I2C_OK;

	return zapi_bytes_out(found, (uint32_t)n);

}

// (i2c-recover bus) -- unwedge a slave stuck holding SDA down.
static ms_val *zapi_i2c_recover(ms_val *args) {
	z_i2c_t *b;
	if (ms_is_nil(args)) ms_log(MS_PANIC, "i2c-recover: expected a bus");
	b = zapi_i2c_bus(ms_car(args), "i2c-recover");
	return zapi_i2c_result(z_i2c_recover(b));
}

// (i2c-write bus addr data) -- data is a list of bytes or a string.
// `addr` is the 7-BIT address (0x3c, not 0x78) as everywhere else.
static ms_val *zapi_i2c_write(ms_val *args) {

	z_i2c_t *b;
	int addr;
	uint8_t buf[ZAPI_BB_MAX];
	uint32_t n;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "i2c-write: expected a bus");
	b = zapi_i2c_bus(ms_car(args), "i2c-write"); args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "i2c-write: expected an address");
	addr = zapi_arg_int(ms_car(args), "i2c-write"); args = ms_cdr(args);
	if (addr < 0 || addr > 0x7f)
		ms_log(MS_PANIC, "i2c-write: %d is not a 7-bit address", addr);

	n = ms_is_nil(args) ? 0
		: zapi_bytes_in(ms_car(args), buf, ZAPI_BB_MAX, "i2c-write");

	return zapi_i2c_result(z_i2c_write(b, (uint8_t)addr, buf, n, true));

}

// (i2c-read bus addr n) -- a list of n bytes, or #f.
static ms_val *zapi_i2c_read(ms_val *args) {

	z_i2c_t *b;
	int addr, n;
	uint8_t buf[ZAPI_BB_MAX];
	z_i2c_rv rv;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "i2c-read: expected a bus");
	b = zapi_i2c_bus(ms_car(args), "i2c-read"); args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "i2c-read: expected an address");
	addr = zapi_arg_int(ms_car(args), "i2c-read"); args = ms_cdr(args);

	n = zapi_opt_int(&args, 1, "i2c-read");

	if (addr < 0 || addr > 0x7f)
		ms_log(MS_PANIC, "i2c-read: %d is not a 7-bit address", addr);
	if (n < 1 || n > ZAPI_BB_MAX)
		ms_log(MS_PANIC, "i2c-read: want 1..%d bytes", ZAPI_BB_MAX);

	rv = z_i2c_read(b, (uint8_t)addr, buf, (uint32_t)n, true);
	zapi_i2c_last = rv;

	if (rv != Z_I2C_OK) return ms_mk_bool(false);

	return zapi_bytes_out(buf, (uint32_t)n);

}

// (i2c-reg bus addr reg)     -- read one register, or #f
// (i2c-reg bus addr reg val) -- write it, returning #t or #f
//
// One procedure rather than two, on the pattern (gpio-dir) and
// (video-mode) already set here: the getter and the setter differ by
// one argument and reading like a getter is what a caller wants at a
// prompt.
//
// This is a write-then-REPEATED-START-read underneath, not a write,
// a stop and a read. Devices are entitled to forget a register
// pointer after a STOP and some do, intermittently, which is a
// miserable thing to debug.
static ms_val *zapi_i2c_reg(ms_val *args) {

	z_i2c_t *b;
	int addr, reg;
	uint8_t v;
	z_i2c_rv rv;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "i2c-reg: expected a bus");
	b = zapi_i2c_bus(ms_car(args), "i2c-reg"); args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "i2c-reg: expected an address");
	addr = zapi_arg_int(ms_car(args), "i2c-reg"); args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "i2c-reg: expected a register");
	reg = zapi_arg_int(ms_car(args), "i2c-reg"); args = ms_cdr(args);

	if (addr < 0 || addr > 0x7f)
		ms_log(MS_PANIC, "i2c-reg: %d is not a 7-bit address", addr);
	if (reg < 0 || reg > 255)
		ms_log(MS_PANIC, "i2c-reg: %d is not a register (want 0..255)", reg);

	if (!ms_is_nil(args)) {
		int val = zapi_arg_int(ms_car(args), "i2c-reg");
		if (val < 0 || val > 255)
			ms_log(MS_PANIC, "i2c-reg: %d is not a byte (want 0..255)", val);
		return zapi_i2c_result(z_i2c_reg_write8(b, (uint8_t)addr,
			(uint8_t)reg, (uint8_t)val));
	}

	rv = z_i2c_reg_read8(b, (uint8_t)addr, (uint8_t)reg, &v);
	zapi_i2c_last = rv;

	if (rv != Z_I2C_OK) return ms_mk_bool(false);

	return ms_mk_num((double)v);

}

// (i2c-khz bus) -- what the bus actually managed on the last transfer.
//
// Usually below what was asked for, and how far below is diagnostic:
// the delay loop cannot go under the cost of the bus transactions, and
// a weak pull-up adds real rise time to every released edge. Far under
// on a bus that should be fast means the pull-ups need help.
static ms_val *zapi_i2c_khz(ms_val *args) {
	z_i2c_t *b;
	if (ms_is_nil(args)) ms_log(MS_PANIC, "i2c-khz: expected a bus");
	b = zapi_i2c_bus(ms_car(args), "i2c-khz");
	return ms_mk_num((double)z_i2c_measured_khz(b));
}

// (spi-init port sck mosi miso cs)             -- mode 0, 1MHz
// (spi-init port sck mosi miso cs mode)
// (spi-init port sck mosi miso cs mode khz)
//
// -1 for miso or cs means the device has no such pin: a write-only
// display needs no MISO, and a lone device with CS tied low needs no
// chip select.
static ms_val *zapi_spi_init(ms_val *args) {

	int port, sck, mosi, miso, cs, mode, khz;
	int h;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "spi-init: expected a port");
	port = zapi_arg_int(ms_car(args), "spi-init"); args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "spi-init: expected an sck pin");
	sck = zapi_arg_int(ms_car(args), "spi-init"); args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "spi-init: expected a mosi pin");
	mosi = zapi_arg_int(ms_car(args), "spi-init"); args = ms_cdr(args);

	miso = zapi_opt_int(&args, -1, "spi-init");
	cs = zapi_opt_int(&args, -1, "spi-init");
	mode = zapi_opt_int(&args, 0, "spi-init");
	khz = zapi_opt_int(&args, 1000, "spi-init");

	zapi_gpio_check(port, sck, "spi-init");
	zapi_gpio_check(port, mosi, "spi-init");
	if (miso >= 0) zapi_gpio_check(port, miso, "spi-init");
	if (cs >= 0) zapi_gpio_check(port, cs, "spi-init");

	if (mode < 0 || mode > 3)
		ms_log(MS_PANIC, "spi-init: mode %d is not 0..3", mode);

	for (h = 0; h < ZAPI_SPI_MAX; h++) if (!zapi_spi_used[h]) break;
	if (h == ZAPI_SPI_MAX)
		ms_log(MS_PANIC, "spi-init: no free bus (at most %d)", ZAPI_SPI_MAX);

	zapi_spi[h].sck_port = (uint8_t)port;
	zapi_spi[h].sck_pin = (uint8_t)sck;
	zapi_spi[h].mosi_port = (uint8_t)port;
	zapi_spi[h].mosi_pin = (uint8_t)mosi;
	zapi_spi[h].miso_port = miso < 0 ? Z_SPI_NO_PIN : (uint8_t)port;
	zapi_spi[h].miso_pin = miso < 0 ? 0 : (uint8_t)miso;
	zapi_spi[h].cs_port = cs < 0 ? Z_SPI_NO_PIN : (uint8_t)port;
	zapi_spi[h].cs_pin = cs < 0 ? 0 : (uint8_t)cs;
	zapi_spi[h].mode = (uint8_t)mode;
	zapi_spi[h].lsb_first = false;
	zapi_spi[h].cs_active_high = false;
	zapi_spi[h].khz = (uint32_t)(khz < 0 ? 0 : khz);

	if (!z_spi_init(&zapi_spi[h]))
		ms_log(MS_PANIC, "spi-init: rejected (mode out of range?)");

	zapi_spi_used[h] = true;

	return ms_mk_num((double)h);

}

// (spi-select bus v) -- assert or release CS. A no-op with no CS pin.
//
// Separate from (spi-xfer) because almost every real device wants
// several transfers inside one selection -- a command, an address,
// then a burst -- and a select-per-transfer API cannot express that.
static ms_val *zapi_spi_select(ms_val *args) {

	z_spi_t *b;
	bool on;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "spi-select: expected a bus");
	b = zapi_spi_bus(ms_car(args), "spi-select"); args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "spi-select: expected a value");
	on = zapi_arg_truthy(ms_car(args));

	z_spi_select(b, on);

	return ms_mk_bool(on);

}

// (spi-xfer bus data) -- send bytes and return what came back.
//
// SPI is full duplex: every byte out produces a byte in, and this
// returns as many as were sent. To read without sending anything
// meaningful, send 255s -- which is what a device expects to see while
// it is talking, and is what (spi-xfer bus n) does if given a count
// instead of a list.
//
// CS IS NOT TOUCHED. Wrap this in (spi-select ...) calls; see there.
static ms_val *zapi_spi_xfer(ms_val *args) {

	z_spi_t *b;
	uint8_t buf[ZAPI_BB_MAX];
	uint32_t n;
	ms_val *d;

	if (ms_is_nil(args)) ms_log(MS_PANIC, "spi-xfer: expected a bus");
	b = zapi_spi_bus(ms_car(args), "spi-xfer"); args = ms_cdr(args);

	if (ms_is_nil(args)) ms_log(MS_PANIC, "spi-xfer: expected data or a count");

	d = ms_car(args);

	if (ms_is_num(d)) {
		int k = zapi_arg_int(d, "spi-xfer");
		if (k < 1 || k > ZAPI_BB_MAX)
			ms_log(MS_PANIC, "spi-xfer: want 1..%d bytes", ZAPI_BB_MAX);
		n = (uint32_t)k;
		for (uint32_t i = 0; i < n; i++) buf[i] = Z_SPI_TX_IDLE;
	} else {
		n = zapi_bytes_in(d, buf, ZAPI_BB_MAX, "spi-xfer");
	}

	// In place: z_spi_xfer() reads tx[i] before writing rx[i]
	// precisely so this works.
	z_spi_xfer(b, buf, buf, n);

	return zapi_bytes_out(buf, n);

}

// -- UART1 --
//
// sw/common/zuart.h. See docs/uart1.md.
//
// These talk to UART1 DIRECTLY, which means this process is competing
// with sw/apps/serial for it if that is running. Nothing arbitrates
// MMIO (docs/app_runtime.md), and the honest reading of that is: these
// procedures are for poking at a serial port from a prompt --
// send an AT command, see what a device says on power-up, check a baud
// rate -- and (serial) in repl is for actually USING one. Doing both
// at once produces interleaved bytes and two processes that each think
// they set the baud rate.
//
// UART0 is deliberately absent from this API and from zuart.h. It is
// the console: sw/bios/bios.c writes to it before anything else in the
// system exists. A Scheme one-liner that changed its baud rate would
// take the machine's only diagnostic channel with it.

// (uart1?) -- is there a general-purpose UART1 in this bitstream?
//
// #f on a ULX3S even though it has a UART1, because there that UART is
// soldered to the on-board ESP32 with no header and no second owner.
// See sw/common/zsoc.h's Z_FEATURE2_UART1.
static ms_val *zapi_uart1_p(ms_val *args) {
	(void)args;
	return ms_mk_bool(z_uart1_present());
}

// (uart1-open)       -- 115200, 8N1
// (uart1-open baud)
//
// Returns #t, or #f if there is no UART1 or the rate cannot be
// produced from this clock. That second case is not hypothetical: at
// 48MHz the 16550's divisor makes 921600 land on 1 Mbaud, 8.5% off and
// far outside what a UART tolerates. (uart1-baud-error) says how far.
static ms_val *zapi_uart1_open(ms_val *args) {

	int baud = 115200;

	if (!ms_is_nil(args)) baud = zapi_arg_int(ms_car(args), "uart1-open");

	if (baud < 50 || baud > 3000000)
		ms_log(MS_PANIC, "uart1-open: %d is outside 50..3000000", baud);

	return ms_mk_bool(z_uart1_open((uint32_t)baud));

}

// (uart1-baud-error baud) -- how far off `baud` would be, in percent.
//
// A real number, so 8.5 means 8.5%. Anything over 3 is refused by
// (uart1-open); a UART samples mid-bit and accumulates the error over
// ten bit times, so a few percent walks the sample point off the end
// of the byte.
//
// Worth asking BEFORE blaming a cable. The symptom of a bad divisor is
// a port that works at 115200 and produces garbage at 921600, which
// looks exactly like a hardware problem.
static ms_val *zapi_uart1_baud_error(ms_val *args) {

	int baud;

	if (ms_is_nil(args))
		ms_log(MS_PANIC, "uart1-baud-error: expected a baud rate");

	baud = zapi_arg_int(ms_car(args), "uart1-baud-error");
	if (baud < 1) ms_log(MS_PANIC, "uart1-baud-error: %d is not a rate", baud);

	return ms_mk_num((double)z_uart1_baud_error((uint32_t)baud) / 100.0);

}

// (uart1-close) -- stop. Leaves the divisor alone, so reopening at the
// same rate costs nothing.
static ms_val *zapi_uart1_close(ms_val *args) {
	(void)args;
	z_uart1_close();
	return ms_mk_bool(true);
}

// (uart1-write "text") or (uart1-write '(1 2 3)) -- returns how many
// bytes went out.
//
// BLOCKS until the transmitter has taken them all. Bounded by the
// length at the current baud rate, which at 9600 is about a
// millisecond per byte -- a 32-byte string is 33ms of this process not
// answering messages. Fine at a prompt; see zuart.h's
// z_uart1_write_nb() for the version a port server wants.
static ms_val *zapi_uart1_write(ms_val *args) {

	uint8_t buf[ZAPI_BB_MAX];
	uint32_t n;

	if (!z_uart1_present())
		ms_log(MS_PANIC, "uart1-write: no general-purpose uart1 in this "
			"bitstream (see docs/uart1.md)");

	if (ms_is_nil(args)) ms_log(MS_PANIC, "uart1-write: expected data");

	n = zapi_bytes_in(ms_car(args), buf, ZAPI_BB_MAX, "uart1-write");

	return ms_mk_num((double)z_uart1_write(buf, n));

}

// (uart1-read)    -- up to 32 bytes, as a list. () if nothing waiting.
// (uart1-read n)
//
// NEVER BLOCKS, and returns what is there rather than waiting for `n`.
// A blocking read at a prompt with nothing on the other end would hang
// the REPL with no way out, and the 16550's FIFO is 16 bytes deep so
// there is rarely more to wait for anyway. Call it again.
static ms_val *zapi_uart1_read(ms_val *args) {

	uint8_t buf[ZAPI_BB_MAX];
	int n = ZAPI_BB_MAX;
	uint32_t got;

	if (!z_uart1_present())
		ms_log(MS_PANIC, "uart1-read: no general-purpose uart1 in this "
			"bitstream (see docs/uart1.md)");

	if (!ms_is_nil(args)) {
		n = zapi_arg_int(ms_car(args), "uart1-read");
		if (n < 1 || n > ZAPI_BB_MAX)
			ms_log(MS_PANIC, "uart1-read: want 1..%d bytes", ZAPI_BB_MAX);
	}

	got = z_uart1_read(buf, (uint32_t)n);

	return zapi_bytes_out(buf, got);

}

// (uart1-ready?) -- is there at least one byte waiting?
static ms_val *zapi_uart1_ready(ms_val *args) {
	(void)args;
	return ms_mk_bool(z_uart1_rx_ready());
}

// (uart1-status) -- errors since the last call, as a list of symbols:
// (overrun), (framing), (parity), (break), or () if all is well.
//
// Sticky and cleared by reading, because the 16550's own error bits
// are cleared by any read of its status register and every data read
// goes past it -- an overrun between two (uart1-read) calls would
// otherwise be gone before anyone asked.
//
// OVERRUN MEANS BYTES WERE LOST, silently, from the middle of the
// stream. On a polled receiver at 115200 that is a real possibility:
// one scheduler slice is 15.7 bytes of arrival against a 16-byte FIFO.
// FRAMING usually means the baud rate is wrong rather than the wire
// being bad -- check (uart1-baud-error) first.
static ms_val *zapi_uart1_status(ms_val *args) {

	static const struct { uint32_t bit; const char *name; } names[] = {
		{ Z_UART1_OVERRUN, "overrun" },
		{ Z_UART1_FRAMING, "framing" },
		{ Z_UART1_PARITY,  "parity"  },
		{ Z_UART1_BREAK,   "break"   },
	};

	uint32_t st;
	char buf[64];
	size_t k = 0;
	size_t i;

	(void)args;

	st = z_uart1_status();

	// Built as source text and read back, same as (gamepad) and for
	// the same reason -- see zapi_read_form(). Every character comes
	// from the table above.
	k += (size_t)snprintf(buf + k, sizeof(buf) - k, "'(");
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		if (!(st & names[i].bit)) continue;
		k += (size_t)snprintf(buf + k, sizeof(buf) - k, "%s%s",
			(k > 2) ? " " : "", names[i].name);
	}
	snprintf(buf + k, sizeof(buf) - k, ")");

	return zapi_read_form(buf);

}

// -- Randomness --

// (random)    -- a real in [0, 1)
// (random n)  -- an integer in [0, n), for a positive integer n
//
// Both forms come from the system CSPRNG (sw/common/zrng.h), which is
// seeded from rtl/trng.v's ring oscillators where the bitstream has
// them and from cycle-counter jitter where it doesn't. `(random-secure?)`
// below is how a program tells which it got.
//
// The integer form is rejection sampled inside z_rng_below(), so it is
// uniform rather than `mod n` with its bias toward small values.
//
// The real form divides a 32-bit draw by 2^32. ms numbers are doubles,
// whose 53-bit mantissa holds that exactly, so no precision is lost --
// but only 2^32 distinct values can come out, which is worth knowing
// before using this to sample a continuous distribution finely.
//
// A negative or zero argument returns 0 rather than raising: `(random
// (length lst))` on an empty list is a natural thing to write and a
// panic there would be more annoying than useful.
static ms_val *zapi_random(ms_val *args) {

	int n;

	if (ms_is_nil(args))
		return ms_mk_num((double)z_rng_u32() / 4294967296.0);

	n = zapi_arg_int(ms_car(args), "random");
	if (n <= 0) return ms_mk_num(0);

	return ms_mk_num((double)z_rng_below((uint32_t)n));

}

// (random-hex n) -- n random BYTES as a 2n-character lowercase hex
// string.
//
// Bytes rather than characters because the callers that want this want
// a key, a token or a nonce, and those are measured in bytes. Hex
// rather than raw because ms strings are NUL-terminated C strings and
// cannot hold a zero byte.
//
// Capped at 256 bytes per call -- not a security limit, just a bound
// on a single allocation from a heap that repl shares with everything
// else it is running (see kernel.h's stack-tier comment on repl).
static ms_val *zapi_random_hex(ms_val *args) {

	static const char hex[] = "0123456789abcdef";
	uint8_t buf[256];
	char *out;
	int n, i;

	n = zapi_arg_int(ms_car(args), "random-hex");
	if (n <= 0) return ms_mk_str(strdup(""));
	if (n > 256) ms_log(MS_PANIC, "random-hex: at most 256 bytes");

	z_rng_bytes(buf, (uint32_t)n);

	out = malloc(2 * n + 1);
	if (!out) ms_log(MS_PANIC, "random-hex: out of memory");

	for (i = 0; i < n; i++) {
		out[2 * i]     = hex[(buf[i] >> 4) & 0xf];
		out[2 * i + 1] = hex[buf[i] & 0xf];
	}
	out[2 * n] = 0;

	// The buffer held key material a moment ago. Clearing a stack
	// array whose lifetime ends here is close to ceremonial, but it
	// costs nothing and the habit is worth more than the instance.
	memset(buf, 0, sizeof(buf));

	return ms_mk_str(out);

}

// (random-secure?) -- #t if the generator is seeded from a present and
// healthy hardware source, #f if it is running on the fallback.
//
// A program generating anything an attacker will see should check this
// and stop, exactly as sw/apps/net's SSH client does. It is #f on a
// bitstream without `TRNG, and also -- importantly -- on one that HAS
// the hardware but whose oscillators are not oscillating, which is the
// failure a toolchain can introduce without anyone touching the code.
// See docs/trng.md.
static ms_val *zapi_random_secure(ms_val *args) {
	(void)args;
	return ms_mk_bool(z_rng_secure());
}

// (random-stir s) -- mix a string into the entropy pool.
//
// For a program that has a source of unpredictability nothing else can
// see -- the timing of a person's keypresses, say. Absorption is
// one-way, so this cannot make the generator worse no matter what is
// passed in, and it never makes `(random-secure?)` true: nothing here
// can measure how much entropy a caller's bytes carried, and assuming
// generously is how a system ends up believing it is seeded when it
// isn't.
static ms_val *zapi_random_stir(ms_val *args) {
	const char *s = zapi_arg_str(ms_car(args), "random-stir");
	z_rng_stir(s, (uint32_t)strlen(s));
	return ms_mk_bool(true);
}

void zapi_register(void) {
	ms_def_builtin("ls", zapi_ls);
	ms_def_builtin("file-size", zapi_file_size);
	ms_def_builtin("read-file", zapi_read_file);
	ms_def_builtin("write-file", zapi_write_file);
	ms_def_builtin("delete-file", zapi_delete_file);
	ms_def_builtin("win-create", zapi_win_create);
	ms_def_builtin("win-destroy", zapi_win_destroy);
	ms_def_builtin("win-clear", zapi_win_clear);
	ms_def_builtin("line", zapi_line);
	ms_def_builtin("box", zapi_box);
	ms_def_builtin("text", zapi_text);
	ms_def_builtin("getpid", zapi_getpid);
	ms_def_builtin("pid-lookup", zapi_pid_lookup);
	ms_def_builtin("msg-send", zapi_msg_send);
	ms_def_builtin("msg-wait", zapi_msg_wait);
	ms_def_builtin("tget", zapi_tget);
	ms_def_builtin("tput", zapi_tput);
	ms_def_builtin("mkdir", zapi_mkdir);
	ms_def_builtin("touch-file", zapi_touch_file);
	ms_def_builtin("load", zapi_load);
	// `file->str` is upstream ms.c's own name for "whole file as a
	// string" (bi_file_to_str, compiled out under -DLIX along with the
	// rest of its stdio path). Bound to the SAME function read-file
	// already is, rather than a second implementation: code written
	// against either name works, and there's no behavior to keep in
	// sync because there's only one function.
	ms_def_builtin("file->str", zapi_read_file);
	ms_def_builtin("ps", zapi_ps);
	ms_def_builtin("run", zapi_run);
	ms_def_builtin("kill", zapi_kill);
	ms_def_builtin("uptime", zapi_uptime);
	ms_def_builtin("current-time", zapi_current_time);
	ms_def_builtin("current-date", zapi_current_date);
	ms_def_builtin("free", zapi_free);
	ms_def_builtin("delay-ms", zapi_delay_ms);
	ms_def_builtin("print-console", zapi_print_console);
	ms_def_builtin("df", zapi_df);
	ms_def_builtin("video-mode", zapi_video_mode);
	ms_def_builtin("game-mode", zapi_game_mode);
	ms_def_builtin("game-view", zapi_game_view);
	ms_def_builtin("game-frame", zapi_game_frame);
	ms_def_builtin("game-wait", zapi_game_wait);
	ms_def_builtin("gamepad", zapi_gamepad);
	ms_def_builtin("gamepad-count", zapi_gamepad_count);
	ms_def_builtin("random", zapi_random);
	ms_def_builtin("random-hex", zapi_random_hex);
	ms_def_builtin("random-secure?", zapi_random_secure);
	ms_def_builtin("random-stir", zapi_random_stir);
	ms_def_builtin("gpio-ports", zapi_gpio_ports);
	ms_def_builtin("gpio-dir", zapi_gpio_dir);
	ms_def_builtin("gpio-out", zapi_gpio_out);
	ms_def_builtin("gpio-in", zapi_gpio_in);
	ms_def_builtin("gpio-mode", zapi_gpio_mode);
	ms_def_builtin("gpio-get", zapi_gpio_get);
	ms_def_builtin("gpio-set", zapi_gpio_set);
	ms_def_builtin("gpio-toggle", zapi_gpio_toggle);
	ms_def_builtin("gpio-od", zapi_gpio_od);
	ms_def_builtin("led", zapi_led);
	ms_def_builtin("leds", zapi_leds);
	ms_def_builtin("i2c-init", zapi_i2c_init);
	ms_def_builtin("i2c-error", zapi_i2c_error);
	ms_def_builtin("i2c-scan", zapi_i2c_scan);
	ms_def_builtin("i2c-recover", zapi_i2c_recover);
	ms_def_builtin("i2c-write", zapi_i2c_write);
	ms_def_builtin("i2c-read", zapi_i2c_read);
	ms_def_builtin("i2c-reg", zapi_i2c_reg);
	ms_def_builtin("i2c-khz", zapi_i2c_khz);
	ms_def_builtin("spi-init", zapi_spi_init);
	ms_def_builtin("spi-select", zapi_spi_select);
	ms_def_builtin("spi-xfer", zapi_spi_xfer);
	ms_def_builtin("uart1?", zapi_uart1_p);
	ms_def_builtin("uart1-open", zapi_uart1_open);
	ms_def_builtin("uart1-close", zapi_uart1_close);
	ms_def_builtin("uart1-baud-error", zapi_uart1_baud_error);
	ms_def_builtin("uart1-write", zapi_uart1_write);
	ms_def_builtin("uart1-read", zapi_uart1_read);
	ms_def_builtin("uart1-ready?", zapi_uart1_ready);
	ms_def_builtin("uart1-status", zapi_uart1_status);
}
