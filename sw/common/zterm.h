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
 *              caller -> term   tag=0 (unused)   obj=Z_MAP{"name":Z_STR, "arg":<any z_obj_t>}
 *
 * Two payload shapes: a bare Z_STR is just the provider name (the
 * original, still-supported form -- `repl`'s `port <name>` command
 * below uses this one). A Z_MAP additionally carries `arg`, forwarded
 * as-is into z_port_connect_arg() (sw/common/zport.h) as the CONNECT
 * message's own payload -- for a provider that needs more than a bare
 * "someone wants to connect" to decide whether to accept, e.g.
 * sw/apps/net's telnet port provider, which needs to know a target IP
 * *before* it can decide to accept at all (see docs/ports.md,
 * docs/networking.md). `repl`'s `telnet <ip>` command is the first
 * (and, as of this revision, only) sender of the Z_MAP form --
 * Z_MAP{"name":"net0", "arg":Z_UINT32(ip)}.
 *
 * No reply -- this is fire-and-forget, matching Z_WM_WINDOW_MOVED and
 * the other one-way notifications term already handles in its own
 * main loop, not an RPC like z_port_connect()/z_win_create(). The
 * caller finds out whether the switch actually worked the same way
 * anyone does with `term` -- watch its own printf() log on the serial
 * console (term.c's own convention already, e.g. "no port provider
 * answered") -- there's no message-based confirmation.
 *
 * First sender: `repl`'s own `port <name>` command (sw/apps/repl/repl.c)
 * -- typing `port portdemo0` at a `term` prompt connected to `repl`
 * sends this to THAT SPECIFIC term's own pid (repl already knows it
 * -- `z_port_t.peer_pid` on the connection the command arrived on),
 * asking it to leave `repl` and connect to `portdemo0` instead,
 * mainly useful for debugging the port protocol itself against
 * portdemo's simpler raw-echo behavior without needing a second
 * `term` window. `repl`'s `telnet <ip>` command is the second sender,
 * using the Z_MAP form above to also hand `net` the target IP.
 * Nothing about this message is `repl`-specific, though -- any
 * process that knows a target term's pid can send it.
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
