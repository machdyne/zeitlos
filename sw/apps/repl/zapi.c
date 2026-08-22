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

#include "ms_api.h"
#include "../../common/zfsapp.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zfont.h"
#include "../../common/zdns.h"
#include "../../common/znet.h"
#include "../../common/zstream.h"
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

// same lookup-then-cache pattern as zwin.c's own resolve_wm_pid() --
// see its comment for the full reasoning (falls back to the fixed
// Z_PID_NET constant, znet.h, if the name lookup fails).
static uint32_t zapi_resolve_net_pid(void) {
	if (!zapi_net_pid_resolved) {
		if (!z_pid_lookup("net0", &zapi_net_pid_cache))
			zapi_net_pid_cache = Z_PID_NET;
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

// -- registration --

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
}
