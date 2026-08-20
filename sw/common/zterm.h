#ifndef ZTERM_H
#define ZTERM_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * One control message, `term` (sw/apps/term/term.c) side only --
 * lets another process ask a specific, already-running `term`
 * instance to disconnect from whatever port provider it's currently
 * talking to and connect to a different one instead, by name
 * (sw/os/pidreg.h). Not part of the port protocol itself
 * (sw/common/zport.h, docs/ports.md) -- this is sent directly to
 * term's own pid, outside any z_port_t connection, since it's asking
 * term to change which connection it even has, not exchanging data
 * over one that already exists.
 *
 *   SET_PORT   caller -> term   tag=0 (unused)   obj=Z_STR (provider name, e.g. "portdemo0")
 *
 * No reply -- this is fire-and-forget, matching Z_WM_WINDOW_MOVED and
 * the other one-way notifications term already handles in its own
 * main loop, not an RPC like z_port_connect()/z_win_create(). The
 * caller finds out whether the switch actually worked the same way
 * anyone does with `term` -- watch its own printf() log on the serial
 * console (term.c's own convention already, e.g. "no port provider
 * answered") -- there's no message-based confirmation.
 *
 * First (and currently only) sender: `lisp`'s own `port <name>`
 * command (sw/apps/lisp/lisp.c) -- typing `port portdemo0` at a
 * `term` prompt connected to `lisp` sends this to THAT SPECIFIC
 * term's own pid (lisp already knows it -- `z_port_t.peer_pid` on the
 * connection the command arrived on), asking it to leave `lisp` and
 * connect to `portdemo0` instead, mainly useful for debugging the
 * port protocol itself against portdemo's simpler raw-echo behavior
 * without needing a second `term` window. Nothing about this message
 * is `lisp`-specific, though -- any process that knows a target
 * term's pid can send it.
 *
 * term.c closes its current connection (if any -- z_port_close(),
 * which does notify that peer, see zport.c) before attempting the
 * new one, exactly the same connect_port() helper it uses for its
 * own startup connection -- if the new name can't be found or
 * doesn't answer, term is left in the same "local echo only"
 * fallback state a failed startup connection already leaves it in,
 * NOT still connected to whatever it just left.
 */

#define Z_TERM_SET_PORT   140

#endif
