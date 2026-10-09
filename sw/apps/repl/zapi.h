#ifndef ZAPI_H
#define ZAPI_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * The Zeitlos Scheme API -- see docs/scheme_api.md for the full
 * design writeup. Every C-backed procedure Scheme code running inside
 * `repl` can call (beyond ms's own stdlib) is registered here, via
 * ms_def_builtin() (sw/ext/ms/ms.c). This file is deliberately the
 * ONLY place new procedures get added -- ms.c itself never needs
 * touching again once its own small registration/construction patch
 * (docs/scheme_api.md \S3) is in place.
 */

#include <stdbool.h>

#include "../../common/zmsg.h"	// z_msg_t -- see zapi_win_msg() below

// registers every zapi_* procedure below into ms_global_env. Call
// once, from main(), right after ms_init_lix() succeeds -- same
// ordering ms_stdlib.l's own load already needs (a zapi_* builtin
// could in principle be called from Scheme code loaded as part of a
// future stdlib addition, so this needs to be done before anything
// else gets a chance to eval).
void zapi_register(void);

// destroys the Scheme-owned window with this wm window id (if this
// process still has one open under it) -- see zapi.c's own comment
// for the full writeup. Called from repl.c's main loop on Z_WM_CLOSE
// (sw/common/zwm.h) -- the one piece of the window-close protocol
// that has to live in repl.c itself rather than zapi.c, since
// zapi.c's own procedures only ever run from an ms_eval() call, never
// from the message loop directly.
void zapi_win_close(int id);

// applies a compositor message (Z_WM_SET_CLIP, Z_WM_WINDOW_MOVED) to
// whichever Scheme-created window it names, and returns true if one
// of them took it. Called from repl.c's main loop, for the same
// reason zapi_win_close() is: repl can own several windows off one
// pid, so the routing has to happen where the table is -- see
// zapi.c's own comment.
bool zapi_win_msg(z_msg_t *msg);

// The term connection running the current command (repl.c sets it),
// or -1: a window remembers the one that created it, for its
// handler's output.
extern int repl_cur_conn;

// A wm message for one of repl's windows, as an event for Scheme
// (docs/repl.md): returns 1 with the expression to evaluate in `expr`
// and the connection its output belongs to in `*conn`, or 0 when there
// is nothing for Scheme (no handler, or a key a text field took).
// zapi_win_event_done() finishes what the event started -- the
// widgets drawn over a redraw, a closed window destroyed -- and is
// called after the expression has run, whether or not it failed.
int zapi_win_event(z_msg_t *msg, char *expr, int cap, int *conn);
void zapi_win_event_done(void);

#endif
