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
 * Same well-known-pid convention Z_PID_WM/Z_PID_NET already use
 * (docs/messaging.md's "no dynamic pid discovery yet" limitation
 * applies here too) -- a provider runs at a documented, fixed pid,
 * started before any client that wants to reach it.
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

// well-known pid for the demo virtual port (sw/apps/portdemo) --
// same convention as Z_PID_WM (zwm.h) / Z_PID_NET (znet.h).
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

// -- client OR provider side, once connected --

// sends a chunk of data. fire-and-forget -- see docs/ports.md's "Flow
// control: an explicit, deliberate gap for v1" for why, and what a
// caller should do about it (nothing, for now -- z_msg_send() failing
// because the peer's mailbox is full is the only backpressure that
// exists yet, and this just surfaces that as Z_FAIL rather than
// hiding it).
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
