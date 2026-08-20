#ifndef ZLISP_H
#define ZLISP_H

#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zobj.h"
#include "zmsg.h"

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * A second, non-interactive way to reach `lisp` (sw/apps/lisp/lisp.c),
 * alongside its port-based interactive command line (zport.h,
 * docs/ports.md). The port protocol assumes a human at a `term`
 * window, typing one keystroke at a time (see zline.h's header
 * comment on why that's a real, deliberate constraint of ports as
 * they exist today) -- that's the wrong shape for a C-based app (a
 * text editor, most obviously -- see this project's own
 * planning notes) that already has a whole chunk of Scheme source
 * sitting in memory and just wants it evaluated, once, with the
 * answer handed back.
 *
 * LISP_EVAL is a plain one-shot request/reply, the same style
 * Z_NET_TFTP_PUT/Z_NET_TFTP_PUT_REPLY (znet.h) already use for a
 * similar "ask, get exactly one answer back" exchange -- not a
 * `zport.h`-style connection (no CONNECT, no ongoing conn_id, no
 * DATA/CLOSE), since there's no session here to hold open: each
 * request stands alone, evaluated against the SAME shared global
 * environment every port connection's REPL commands already run
 * against (see lisp.c's own header comment on why `lisp` uses one
 * shared environment rather than one per caller) -- so a definition
 * made by one LISP_EVAL request, or by someone typing at a `term`
 * connected to the same `lisp` instance, is visible to the next one,
 * same as two `term` windows already see each other's definitions.
 *
 *   LISP_EVAL    caller -> lisp   tag=caller's choice   obj=Z_STR or Z_BLOB (source text)
 *   LISP_RESULT  lisp -> caller   tag=echoed from request   obj=Z_STR (printed result)
 *   LISP_ERROR   lisp -> caller   tag=echoed from request   obj=Z_STR (error message)
 *
 * `tag` is round-tripped unchanged (same convention z_port_t's
 * CONNECT/CONNECTED use their own tag=0 for) purely so a caller that
 * ever has more than one request outstanding at once can match
 * replies -- z_lisp_eval() below only ever has one outstanding at a
 * time, so it doesn't need this, but the field's there for a future
 * caller that does.
 *
 * As of this writing (phase 1, see docs/ports.md-equivalent planning
 * for `lisp`) EVAL requests are handled by the exact same builtin
 * command dispatcher a completed port line goes through -- real
 * Scheme evaluation isn't wired in yet, so don't expect anything more
 * than the same handful of builtin commands (`ping`, `uptime`, ...)
 * to actually work through this path yet either. The wire protocol
 * and this client helper are meant to not need to change once real
 * evaluation lands behind them.
 */

#define Z_LISP_EVAL     130
#define Z_LISP_RESULT   131
#define Z_LISP_ERROR    132

// fallback pid for `lisp` (sw/apps/lisp) if name lookup ("lisp0")
// fails -- same convention as Z_PID_PORTDEMO (zport.h) and
// Z_PID_WM/Z_PID_NET before it. 3, not 4: sh.c's init() starts lisp
// in exactly the boot-order slot portdemo used to occupy (wm=1,
// net=2 reserved-not-started, then this) -- see init()'s own comment
// -- so 3 is where a fixed-order boot actually lands it, same
// reasoning Z_PID_PORTDEMO=3 was originally chosen for before this
// changed. Z_PID_PORTDEMO itself is now stale for this purpose
// (portdemo is no longer started automatically, see its own
// constant's comment in zport.h) -- nothing still depends on it for
// pid 3 specifically once this migration landed. Not a promise
// `lisp` actually landed here regardless -- see docs/ports.md's
// "Testing this" for why the fallback pid convention only works at
// all if things were started in a fixed order; prefer
// z_pid_lookup("lisp0", ...) whenever possible, same as term.c does.
#define Z_PID_LISP   3

// max response text z_lisp_eval() will accept into `out` -- matches
// lisp.c's own reply buffer size, kept here so a caller can size its
// own buffer correctly without reaching into lisp.c. A reply longer
// than this is truncated (still NUL-terminated), not rejected.
#define Z_LISP_EVAL_REPLY_MAX 256

// sends `code` to `lisp_pid` as a LISP_EVAL request and blocks for the
// reply -- bounded (~`timeout_ticks`, see z_uptime_ticks()), NOT
// forever, same reasoning as z_port_connect()'s own timeout: `lisp`
// isn't guaranteed to be running, and unlike z_win_create()'s RPC to
// `wm` (which the whole system depends on staying up), a caller here
// should be able to give up cleanly. Like z_port_connect(), call this
// before your own message loop is also expecting other messages --
// any unrelated message that arrives while waiting is discarded, same
// accepted limitation.
//
// returns Z_OK if `lisp` replied at all (check *is_error to tell
// LISP_RESULT from LISP_ERROR -- `out` holds the reply text either
// way, truncated to Z_LISP_EVAL_REPLY_MAX/out_cap if needed). returns
// Z_FAIL on timeout (lisp_pid not running, or genuinely wedged) --
// `out`/*is_error are untouched in that case.
z_rv z_lisp_eval(uint32_t lisp_pid, const char *code,
	char *out, uint32_t out_cap, uint32_t timeout_ticks, bool *is_error);

#endif
