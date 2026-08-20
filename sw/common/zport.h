#ifndef ZPORT_H
#define ZPORT_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Ports -- a small client/provider protocol so an app like `term`
 * doesn't need to know or care whether it's talking to a real
 * hardware UART, a telnet-over-UDP proxy, or a test harness. See
 * docs/ports.md for the full design writeup (why this exists, why
 * it's plain z_msg_t and not zstream.h, the flow-control tradeoff)
 * -- this header is deliberately just the protocol + thin helpers,
 * not a repeat of that reasoning.
 *
 * A provider can be reached at a fixed, documented pid (the
 * Z_PID_PORTDEMO convention below), started before any client that
 * wants it -- or, better, registered by name (sw/os/pidreg.h) and
 * looked up by whoever wants to connect, same migration Z_PID_WM/
 * Z_PID_NET already went through (see docs/networking.md). The fixed
 * pid still exists as a fallback for whichever path a given provider/
 * client pair doesn't (yet) use.
 *
 *   CONNECT   client -> provider   tag=0         obj=Z_NONE
 *   CONNECTED provider -> client   tag=0         obj=Z_UINT32(conn_id)
 *   REFUSED   provider -> client   tag=0         obj=Z_STR(reason)
 *   DATA      either direction     tag=conn_id   obj=Z_BLOB
 *   CLOSE     either direction     tag=conn_id   obj=Z_NONE
 *
 * DATA/CLOSE aren't wrapped in a blocking helper -- a connected app's
 * own message loop should recognize Z_PORT_DATA/Z_PORT_CLOSE by
 * subject directly (same as it already does for e.g. Z_WM_KEY),
 * using z_blob_data()/z_blob_len() (zobj.h) on a DATA message's
 * payload, and msg.tag to find which z_port_t it belongs to (matters
 * for a provider juggling more than one connection; a client with
 * exactly one connection can just check msg.tag == port.conn_id, or
 * skip the check if there's genuinely only ever one).
 */

#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zobj.h"
#include "zmsg.h"

#define Z_PORT_CONNECT    120
#define Z_PORT_CONNECTED  121
#define Z_PORT_REFUSED    122
#define Z_PORT_DATA       123
#define Z_PORT_CLOSE      124

// fallback pid for the demo virtual port (sw/apps/portdemo) if name
// lookup ("portdemo0") fails -- same convention as Z_PID_WM (zwm.h) /
// Z_PID_NET (znet.h). Stale as an actual fallback since `repl`
// replaced portdemo in sh.c's init() boot sequence (see
// sw/apps/repl/repl.c, sw/common/zrepl.h's Z_PID_REPL -- 3, the same
// slot this constant documents, since repl now starts where portdemo
// used to): portdemo is no longer started automatically at boot, so
// nothing actually lands here anymore unless you `run portdemo`
// manually, at whatever pid happens to be free at the time -- this
// constant is only meaningful again if something goes back to
// starting portdemo at a fixed, predictable point in the boot order.
// Left in place, not removed -- still accurate documentation of the
// convention itself, just not a live guarantee right now.
#define Z_PID_PORTDEMO   3

typedef struct {
	uint32_t peer_pid;	// who DATA/CLOSE go to -- the provider if
						// we're the client, the client if we're the
						// provider (z_port_connect()/z_port_accept()
						// each set this to the right one)
	uint32_t conn_id;	// used as the message tag for DATA/CLOSE
	bool connected;
} z_port_t;

// -- client side --

// ~2 seconds at the kernel tick rate (~732Hz -- see sw/os/kernel.c's
// z_kernel_ticks comment). Public (not zport.c-private) specifically
// so a caller needing a longer timeout for one particular connect
// (z_port_connect_arg_timeout() below) can still reference this exact
// default for every other connect it makes, rather than having to
// duplicate the number. See z_port_connect_arg_timeout()'s own
// comment for why one blanket timeout doesn't fit every provider.
#define Z_PORT_CONNECT_TIMEOUT_TICKS (732 * 2)

// connects to a provider at a well-known pid. blocks briefly (a
// bounded ~2 second timeout, NOT forever -- unlike z_msg_wait()'s own
// unbounded blocking, used elsewhere in this codebase for RPCs like
// z_win_create(), a port provider isn't guaranteed to even be running,
// so hanging indefinitely isn't acceptable here) waiting for
// CONNECTED or REFUSED. Like z_win_create(), call this during
// startup, before your own main message loop -- any unrelated message
// that arrives while waiting is silently discarded (same accepted
// limitation z_msg_wait() already has), so this isn't safe to call
// once you're also expecting other messages to arrive.
z_rv z_port_connect(z_port_t *port, uint32_t provider_pid);

// same as z_port_connect(), but lets the caller send a
// provider-specific argument as CONNECT's payload instead of the
// default Z_NONE -- see the protocol sketch above ("obj=Z_NONE (or
// provider-specific args)"). First use: sw/apps/net's telnet port
// provider, which needs to know a target IP before it can decide
// whether to accept the connection at all -- see docs/ports.md and
// sw/common/zterm.h's Z_TERM_SET_PORT (which carries this argument
// from whoever originated the request, e.g. `repl`'s `telnet <ip>`
// command, through to term's own z_port_connect_arg() call). Same
// borrowed-payload lifetime rule as any other message payload
// (docs/messaging.md) applies to `arg` -- it only needs to stay valid
// until this call returns (it's read once, synchronously, when
// building the CONNECT message).
//
// Uses the default ~2 second timeout (zport.c's own
// Z_PORT_CONNECT_TIMEOUT_TICKS) -- fine for a provider that's simply
// slow to get scheduled, wrong for one whose own CONNECT handling
// involves a slow async operation before it can reply either way. Use
// z_port_connect_arg_timeout() below instead for a provider like
// that (net.c's telnet port is exactly this case -- see that
// function's own comment).
z_rv z_port_connect_arg(z_port_t *port, uint32_t provider_pid, z_obj_t arg);

// same as z_port_connect_arg(), but with an explicit timeout instead
// of the default ~2 seconds -- for a provider whose own CONNECT
// handling can legitimately take a while before it knows whether to
// reply CONNECTED or REFUSED, rather than one that either answers
// almost immediately or isn't running at all. net.c's telnet port is
// the motivating case: it doesn't reply until an actual TCP handshake
// to a remote server resolves one way or the other, which can take up
// to tcp.c's own worst-case retry budget (~31.5s, TCP_RTO_TICKS_BASE/
// _MAX_SHIFT/_MAX_RETRIES there) -- the default 2s timeout meant
// `term`'s own connect always gave up locally before net's TCP layer
// ever got a chance to answer, even once net was working correctly on
// its own end. Found and fixed on real hardware: this is what that
// looked like from the outside (a telnet connect that always "timed
// out", regardless of how long tcp.c's own retries were given).
z_rv z_port_connect_arg_timeout(z_port_t *port, uint32_t provider_pid,
	z_obj_t arg, uint32_t timeout_ticks);

// -- client OR provider side, once connected --

// sends a chunk of data. fire-and-forget -- see docs/ports.md's "Flow
// control: an explicit, deliberate gap for v1" for why, and what a
// caller should do about it (nothing, for now -- z_msg_send() failing
// because the peer's mailbox is full is the only backpressure that
// exists yet, and this just surfaces that as Z_FAIL rather than
// hiding it).
//
// Internally builds a Z_BLOB (z_obj_blob(), zobj.c -- a real heap
// copy of `data`) and DELIBERATELY NEVER FREES IT -- same accepted,
// intentional leak `pong`'s own reply strings already use
// (docs/messaging.md's ping/pong example), now relied on directly
// here too rather than fought. An earlier version tried to free the
// PREVIOUS call's blob on each new call, reasoning that the peer must
// have had a scheduling slot to read it by then -- reverted after a
// real-hardware bug: that assumption breaks whenever a caller makes
// several z_port_send() calls back-to-back with nothing in between to
// force a scheduler switch (e.g. `repl`'s own handle_connect(),
// banner then prompt immediately) -- the second call's free ran
// before the peer had necessarily read the first message at all, and
// the very next z_obj_blob() call reused that just-freed memory, so
// the first message resolved to the SECOND message's bytes by the
// time the peer actually read it. See docs/messaging.md's "Known
// limitations" for the general version of this problem -- a real fix
// needs the receiver's own read to prove safety, not the sender's
// next unrelated action, which is a bigger mechanism than this file
// takes on right now. Relies instead on Z_PROC_STACK_SIZE
// (sw/os/kernel.c) being large enough to absorb this leak for a
// realistic session length -- raised specifically because of this.
z_rv z_port_send(z_port_t *port, const void *data, uint32_t len);

// tells the peer this connection is done. does not wait for any
// acknowledgment.
void z_port_close(z_port_t *port);

// -- provider side --

// call from a provider's own message-handling loop when a
// Z_PORT_CONNECT arrives. conn_id is the provider's own choice of how
// to identify this connection internally (e.g. an index into its own
// connection table) -- it becomes the tag on every DATA/CLOSE for
// this connection from here on, in both directions.
void z_port_accept(z_port_t *out_port, const z_msg_t *connect_msg, uint32_t conn_id);

// call instead of z_port_accept() to decline a connection (e.g. a
// provider that only supports one client at a time, already in use).
void z_port_refuse(const z_msg_t *connect_msg, const char *reason);

#endif
