#ifndef TE_BRIDGE_H
#define TE_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zport.h"

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Bridges sw/ext/te (a small VT100-based text editor, git submodule,
 * built here with -DEMBEDDED -DTE_HOST_IO -- see that submodule's own
 * te_host_io.h) into repl's per-connection port protocol
 * (sw/common/zport.h, docs/ports.md), for the `te <filename>`
 * command. See docs/editor.md for the full writeup; summary of the
 * two things this module exists to handle that te.c itself has no
 * opinion on:
 *
 * - te.c's own state (the document, cursor, filename, ...) is ALL
 *   file-static globals inside te.c -- there is exactly one editing
 *   session possible per process, ever, not per-connection. This
 *   module enforces that at the repl level: only one of repl's
 *   several concurrent `term` connections (sw/apps/repl/repl.c's
 *   conns[]) can be "in te" at a time; a second `te` command from a
 *   different connection while one is already active is refused with
 *   a clear message rather than silently corrupting the first
 *   session's document.
 *
 * - te.c under -DTE_HOST_IO calls te_host_write()/te_host_getch()/
 *   te_host_flush() instead of talking to a single global stdout/
 *   getch() -- this module IMPLEMENTS those three (below the public
 *   API), routing them through whichever connection's z_port_t
 *   currently owns the live session, and batching every write into
 *   one z_port_send() per input byte processed (te.c's own
 *   te_redraw() issues many small printf()-equivalent calls per
 *   keystroke -- sending each individually would either blow
 *   Z_PORT_MAX_PENDING_SENDS or just be needlessly chatty; see
 *   zport.h's own "Flow control" notes).
 */

// starts a `te` session for `filename`, with output going to `port`.
// Enforces both the single-session-process-wide rule above AND a
// conservative file-size ceiling (TE_BRIDGE_MAX_FILE_SIZE, below) --
// see docs/editor.md for why that ceiling is much smaller than
// repl's own 64KB heap might suggest is safe (te.c's line-list
// representation can cost several times a file's raw size, worse for
// many-short-lines files than for a few long ones).
//
// on success: returns true, and has ALREADY sent the editor's own
// first screen down `port` -- caller should feed every subsequent
// byte from this connection to te_bridge_feed() (not the normal
// line-editing path) until it returns false.
//
// on failure: returns false and writes a short, human-readable reason
// into `out` (NUL-terminated, no trailing newline -- same convention
// dispatch_line() itself uses) -- caller should NOT call
// te_bridge_feed() in this case, nothing was started.
bool te_bridge_start(z_port_t *port, const char *filename,
	char *out, uint32_t out_cap);

// feeds one input byte to the live session (whichever connection owns
// it -- caller is responsible for only calling this for bytes from
// the connection te_bridge_start() most recently succeeded for, see
// its own comment). Returns true if the session is still live (keep
// feeding it bytes), false if it just ended (Esc :q was just
// processed) -- caller should stop calling this and return that
// connection to its own normal line-mode dispatch.
bool te_bridge_feed(uint8_t byte);

// true if a session is currently live (process-wide) -- lets a caller
// check without having to track its own duplicate bookkeeping.
bool te_bridge_active(void);

// forcibly ends whatever session is live, with NO attempt to warn
// about or save unsaved changes -- for a connection that disappeared
// (Z_PORT_CLOSE) while it owned the session. Safe to call even if
// nothing is currently active (a no-op in that case).
void te_bridge_abort(void);

#endif
