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
}
