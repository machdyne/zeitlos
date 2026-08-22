/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * te_bridge -- see te_bridge.h for the full design writeup.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zport.h"
#include "../../common/zline.h"		// Z_LINE_MAX -- sizes filename_buf below
#include "../../common/zfsapp.h"		// fs_size() -- the size-ceiling check
#include "te_bridge.h"
#include "te_support/te_host_io.h"

// -- te.c's own public API -- there's no te.h (see this submodule's
// own single-file-library design, its top-of-file forward-declaration
// block); these are the only pieces this bridge needs, declared here
// rather than pulling in the whole file.
extern int te_edit_start(char *filename);	// returns 0/1, see te.c
extern int te_yield(void);					// returns 0/1, see te.c
extern void te_status_bar(int enabled);	// see te.c's own comment

// how large a file this build will open with `te` -- deliberately
// small, and deliberately NOT sized as a simple fraction of repl's
// 64KB heap (Z_PROC_STACK_SIZE_LARGE, sw/os/kernel.c). te.c's own
// line-list representation (a struct te_line_t + a separately
// malloc'd text buffer PER LINE, sw/ext/te/te.c) costs several times
// a file's raw size for ordinary prose, and MUCH more for a
// pathological many-short-lines file (a few hundred bytes of blank
// lines can cost tens of KB in per-line allocator overhead alone) --
// see docs/editor.md for the actual arithmetic this default was
// chosen against. Overridable at build time (`make
// TE_MAX_FILE_SIZE=4096`) once real usage on real hardware says a
// particular board's headroom allows more -- start conservative,
// widen empirically (this project's own stated preference, see
// docs/networking.md's TFTP staged-bringup notes for the same
// philosophy applied elsewhere).
#ifndef TE_MAX_FILE_SIZE
#define TE_MAX_FILE_SIZE 2048
#endif

// -- single-session-process-wide state -- see te_bridge.h's own
// header comment for why there's exactly one of these, not one per
// connection.
static bool te_active = false;
static z_port_t *te_port = NULL;

// filename needs to outlive the whole session -- te.c keeps its own
// `te_filename` pointer, it does NOT copy the string it's given
// (te_edit_start() just does `te_filename = filename;`). The caller
// (repl.c's dispatch_line(), via the `te` command) only ever hands us
// a pointer into that connection's own z_line_t.buf (Z_LINE_MAX+1
// bytes, sw/common/zline.h) -- which repl.c's handle_data() resets
// (z_line_reset()) on the very same event-loop iteration te_bridge_
// start() returns from, so we copy it into our OWN static storage
// here rather than relying on the caller's buffer staying untouched.
static char te_filename_buf[Z_LINE_MAX + 1];

// staging buffer for te_host_write() -- see te_bridge.h's own comment
// on why writes are batched rather than sent one z_port_send() per
// printf()-equivalent call. Sized for comfortably more than one full
// screen's worth of VT100 output at te.c's own TE_DEFAULT_ROWS/COLS
// (see this app's Makefile for the exact values this build uses) --
// a full redraw is roughly rows * (cols + ~10 bytes of escape
// sequence overhead), plus the status line; deliberately NOT sized
// much larger than that, since every byte here is a byte of repl's
// own tight 64KB heap ALSO briefly duplicated inside z_obj_blob()
// (zobj.c) for the duration of one z_port_send() call, on top of
// whatever te.c's own document representation is already holding.
#define TE_IOBUF_SIZE 3072
static char te_iobuf[TE_IOBUF_SIZE];
static uint32_t te_iobuf_len = 0;

// the one byte currently being fed to te_yield() -- see
// te_bridge_feed()'s own comment for why this works: te_yield() calls
// TE_GETCH() (-> te_host_getch(), under -DTE_HOST_IO) exactly ONCE
// per invocation, at its very top, so staging exactly one byte here
// before each te_yield() call is a correct, exact byte-for-byte feed,
// matching the same "one byte at a time" discipline
// sw/common/zline.h's own z_line_feed() already requires callers to
// use for the same underlying reason (a single Z_PORT_DATA message
// can bundle more than one byte -- a pasted block, or, the common
// case here, a multi-byte VT100 escape sequence like an arrow key).
static int te_pending_byte = -1;

void te_host_flush(void) {

	if (!te_iobuf_len) return;

	if (te_port && te_port->connected) {
		if (z_port_send(te_port, te_iobuf, te_iobuf_len) != Z_OK)
			printf("repl: te_bridge flush failed (%lu bytes)\n",
				(unsigned long)te_iobuf_len);
	}

	te_iobuf_len = 0;

}

void te_host_write(const char *buf, int len) {

	if (len <= 0 || !buf) return;

	if ((uint32_t)len >= TE_IOBUF_SIZE) {
		// pathological: bigger than the whole staging buffer --
		// flush whatever's already queued (to keep output in order)
		// then send this one directly rather than drop any of it.
		te_host_flush();
		if (te_port && te_port->connected)
			z_port_send(te_port, buf, (uint32_t)len);
		return;
	}

	if (te_iobuf_len + (uint32_t)len > TE_IOBUF_SIZE)
		te_host_flush();

	memcpy(te_iobuf + te_iobuf_len, buf, (uint32_t)len);
	te_iobuf_len += (uint32_t)len;

}

int te_host_getch(void) {
	int c = te_pending_byte;
	te_pending_byte = -1;
	return c;	// -1 (EOF) if nothing was staged -- shouldn't happen
				// in practice, see te_bridge_feed()'s own comment,
				// but harmless either way: te_yield()'s own
				// `c == EOF || c == 0` early-return just makes that
				// call a no-op.
}

bool te_bridge_active(void) {
	return te_active;
}

bool te_bridge_start(z_port_t *port, const char *filename,
	char *out, uint32_t out_cap) {

	if (te_active) {
		snprintf(out, out_cap,
			"editor is already in use by another connection -- "
			"try again once it's closed (Esc :q or Esc :w then Esc :q)");
		return false;
	}

	// fs_size() returns 0 for "doesn't exist" too -- that's fine,
	// same as it is for te.c's own te_load() (see its README.md):
	// a 0-byte "file" here just means we're about to create one.
	int sz = fs_size((char *)filename);
	if (sz > TE_MAX_FILE_SIZE) {
		snprintf(out, out_cap,
			"'%s' is %d bytes -- larger than this build's %d byte "
			"limit (repl's heap is small and shared with Scheme -- "
			"see docs/editor.md, and try `free` to see current "
			"headroom)",
			filename, sz, TE_MAX_FILE_SIZE);
		return false;
	}

	snprintf(te_filename_buf, sizeof(te_filename_buf), "%s", filename);

	te_port = port;
	te_iobuf_len = 0;
	te_pending_byte = -1;

	// the coordinate counters (l/s/x/y) in te's own status line cost
	// very little on their own (~13 bytes) -- NOT the reason typing
	// felt sluggish over a port connection. That was te.c's own
	// full-screen redraw firing on every keystroke, fixed directly in
	// te.c itself (te_redraw_line(), used for the common single-line
	// edits -- see that submodule's own comment on it). This toggle
	// is still worth keeping off here regardless: every byte in a
	// redraw is a byte re-parsed by term's VT100 emulator AND briefly
	// duplicated in repl's own tight heap (z_obj_blob(), zport.c) for
	// the duration of one z_port_send() -- a shorter status line is a
	// small, free reduction in both, on every single keystroke.
	te_status_bar(0);

	if (!te_edit_start(te_filename_buf)) {
		snprintf(out, out_cap, "unable to load '%s'", filename);
		te_port = NULL;
		return false;
	}

	// te_edit_start() already called te_redraw()+te_status(), and
	// te_status() itself always ends with TE_FLUSH() (te.c) -- so the
	// editor's first screen has already been sent down `port` by the
	// time we get here. Nothing left to do.
	te_active = true;
	return true;

}

bool te_bridge_feed(uint8_t byte) {

	if (!te_active) return false;

	te_pending_byte = (int)byte;
	int alive = te_yield();

	if (!alive) {
		te_active = false;
		te_port = NULL;
	}

	return alive != 0;

}

void te_bridge_abort(void) {
	te_active = false;
	te_port = NULL;
	te_iobuf_len = 0;
	te_pending_byte = -1;
}
