/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * See zport.h.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zobj.h"
#include "zmsg.h"
#include "zport.h"

// Z_PORT_CONNECT_TIMEOUT_TICKS is declared in zport.h (public) -- see
// that file's own comment for why: this is the right default for "is
// the provider process even up", the wrong one for a provider whose
// own CONNECT handling involves a slow, async operation before it
// can reply either way (e.g. net.c's telnet port: it doesn't send
// CONNECTED/REFUSED until an actual TCP handshake to a remote server
// resolves, which can legitimately take up to tcp.c's own worst-case
// retry budget -- TCP_RTO_TICKS_BASE/_MAX_SHIFT/_MAX_RETRIES there
// sum to ~31.5s before giving up). A real bug on real hardware: this
// constant's 2s was shorter than that 31.5s worst case, so `term`'s
// own telnet connect always timed out on this side before net's TCP
// layer ever got a chance to reply either way -- see
// z_port_connect_arg_timeout() below for the fix (an explicit,
// per-call override), and term.c's connect_port() for where the
// telnet-specific call actually uses it.

z_rv z_port_connect(z_port_t *port, uint32_t provider_pid) {
	return z_port_connect_arg(port, provider_pid, z_obj_none());
}

z_rv z_port_connect_arg(z_port_t *port, uint32_t provider_pid, z_obj_t arg) {
	return z_port_connect_arg_timeout(port, provider_pid, arg,
		Z_PORT_CONNECT_TIMEOUT_TICKS);
}

// see zport.h's own comment for the full reasoning on why this needed
// to become an explicit, per-call parameter instead of one blanket
// constant every connect used.
z_rv z_port_connect_arg_timeout(z_port_t *port, uint32_t provider_pid,
	z_obj_t arg, uint32_t timeout_ticks) {

	port->peer_pid = provider_pid;
	port->conn_id = 0;
	port->connected = false;

	z_msg_new_send(provider_pid, Z_PORT_CONNECT, 0, arg);

	uint32_t start = z_uptime_ticks();

	while ((z_uptime_ticks() - start) < timeout_ticks) {

		z_msg_t msg;
		if (z_msg_read(&msg) != Z_OK) continue;

		if (msg.subject == Z_PORT_CONNECTED && msg.tag == 0) {
			port->conn_id = msg.obj.val.uint32;
			port->connected = true;
			return Z_OK;
		}

		if (msg.subject == Z_PORT_REFUSED && msg.tag == 0) {
			// diagnostic: distinguish an explicit refusal (the
			// provider is alive, ran, and rejected the connection for
			// a real reason) from the timeout below (the provider
			// never replied at all) -- callers like term.c's
			// connect_port() currently print the same generic "no
			// port provider answered" message for both Z_FAIL cases,
			// which made a real bug (net.c's telnet_on_closed() never
			// actually running -- see tcp.c's notify()/
			// reset_to_closed() ordering fix) look identical to a
			// genuine timeout from the console alone.
			if (msg.obj.type == Z_STR && msg.obj.val.str)
				printf("zport: connect to pid %ld refused: %s\n",
					(long)provider_pid, msg.obj.val.str);
			else
				printf("zport: connect to pid %ld refused (no reason given)\n",
					(long)provider_pid);
			return Z_FAIL;
		}

		// not a reply to our CONNECT -- discard and keep waiting, same
		// as z_msg_wait() (zeitlos.c) does for any RPC-style exchange

	}

	printf("zport: connect to pid %ld timed out after %ld ticks -- "
		"provider never replied\n",
		(long)provider_pid, (long)timeout_ticks);
	return Z_FAIL;	// timed out -- provider likely isn't running

}

z_rv z_port_send(z_port_t *port, const void *data, uint32_t len) {

	if (!port->connected) return Z_FAIL;

	// backpressure BEFORE allocating anything -- see zport.h's own
	// Z_PORT_MAX_PENDING_SENDS/`pending` comments. A peer that's
	// stopped acking (crashed, or has just fallen far behind) makes
	// z_port_send() itself refuse further sends this way, rather than
	// letting them leak without bound the way this function used to --
	// see this function's own header comment (zport.h) for the full
	// before/after story.
	if (port->pending_count >= Z_PORT_MAX_PENDING_SENDS) return Z_FAIL;

	z_obj_t obj = z_obj_blob(data, len);

	// z_obj_blob() (zobj.c) returns Z_NONE instead of a broken object
	// when its internal malloc() fails (see that function's own
	// comment), but z_msg_send() below would still report Z_OK for a
	// Z_NONE payload -- from the caller's perspective this call would
	// look like it "succeeded" while silently sending nothing at all.
	// Surface it here instead of leaving it invisible -- this is what
	// caught a real Z_PROC_STACK_SIZE-too-small bug (sw/os/kernel.c)
	// on real hardware, where even a 2-byte allocation failed.
	if (obj.type != Z_BLOB) {
		if (len > 0)
			printf("zport: z_obj_blob() failed to allocate %lu bytes -- "
				"heap likely exhausted\n", (unsigned long)len);
		return Z_FAIL;
	}

	z_rv rv = z_msg_new_send(port->peer_pid, Z_PORT_DATA, port->conn_id, obj);

	if (rv != Z_OK) {
		// never delivered (most likely: the peer's mailbox is full) --
		// no ack will ever arrive for this one, so free it right here
		// instead of leaking it forever. The one case
		// z_port_handle_ack() can't cover on its own -- see that
		// function's own comment. Deliberately NOT pushed onto
		// `pending` below -- pushing it would leave a permanent gap
		// in the FIFO with no ack ever coming to pop it, misaligning
		// every later entry.
		z_obj_free(&obj);
		return rv;
	}

	// pushed onto the TAIL of *port's own FIFO -- z_port_handle_ack()
	// pops from the HEAD, on the strength of the ordering guarantee
	// its own comment (zport.h) explains, once the peer's own
	// z_port_send_ack() (called from ITS handler for this exact
	// Z_PORT_DATA, once it's genuinely done reading the payload)
	// reports it's safe to.
	z_blob_t *b = (z_blob_t *)obj.val.ptr;
	uint32_t tail = (port->pending_head + port->pending_count) % Z_PORT_MAX_PENDING_SENDS;
	port->pending[tail].header = b;
	port->pending_count++;

	return Z_OK;

}

void z_port_close(z_port_t *port) {
	if (!port->connected) return;
	z_msg_new_send(port->peer_pid, Z_PORT_CLOSE, port->conn_id, z_obj_none());
	port->connected = false;
}

// bounded retry window for z_port_send_ack() below -- short on
// purpose, see that function's own header comment (zport.h) for why:
// the only realistic failure mode is the receiving mailbox being
// transiently busy, which resolves within a scheduling slice or two,
// not something worth a long wait over. Same ~732Hz tick rate used
// throughout this codebase (docs/networking.md's "z_uptime_ticks()").
#define Z_PORT_ACK_RETRY_TICKS (732 / 2)	// ~0.5s

void z_port_send_ack(const z_msg_t *data_msg) {

	if (data_msg->obj.type != Z_BLOB) return;	// nothing to ack -- malformed DATA

	uint32_t start = z_uptime_ticks();

	do {

		if (z_msg_new_send(data_msg->from, Z_PORT_DATA_ACK, data_msg->tag,
			z_obj_none()) == Z_OK) return;

		// z_msg_send() failing here means data_msg->from's own mailbox
		// is full right now -- retrying immediately (rather than
		// giving up, the way z_port_send() itself does for the
		// original DATA send) is deliberate: see this function's own
		// header comment (zport.h) for why a lost ack is a much worse
		// failure than a lost DATA message. No explicit yield needed
		// between attempts -- this system schedules preemptively
		// (sw/os/kernel.c, KTIMER-driven round-robin), so even a tight
		// retry loop like this one gets interrupted regularly, giving
		// the peer real chances to drain its own mailbox in between.

	} while ((z_uptime_ticks() - start) < Z_PORT_ACK_RETRY_TICKS);

	// still failing after a genuine retry window -- something worse
	// than transient congestion is going on. z_port_send()'s own
	// backpressure (Z_PORT_MAX_PENDING_SENDS) is the correct backstop
	// from here, same as it already is for a peer that's stopped
	// acking entirely -- nothing more productive to do on this side.
	printf("zport: failed to deliver ack to pid %ld after retrying for "
		"%ld ticks -- its mailbox may be stuck\n",
		(long)data_msg->from, (long)Z_PORT_ACK_RETRY_TICKS);

}

void z_port_handle_ack(z_port_t *port, const z_msg_t *msg) {

	if (!port->connected || msg->tag != port->conn_id) return;
	if (port->pending_count == 0) return;	// stale/duplicate ack -- nothing outstanding

	// pops the OLDEST outstanding entry, unconditionally -- see
	// z_port_t's own comment on `pending` for why this is correct:
	// mailboxes are FIFO per sender/receiver pair, every current DATA
	// receiver acks exactly once, in read order, and z_port_send_ack()
	// itself guarantees that ack reliably arrives -- so the Nth ack
	// back always corresponds to the Nth still-outstanding send.
	uint32_t i = port->pending_head;

	// reuses zobj.c's own general-purpose Z_BLOB destructor (frees
	// both the header and its ->data) rather than duplicating that
	// logic here -- port->pending[i].header is exactly the val.ptr
	// z_obj_blob() originally returned.
	z_obj_t blob = { .type = Z_BLOB, .val.ptr = port->pending[i].header };
	z_obj_free(&blob);

	port->pending_head = (port->pending_head + 1) % Z_PORT_MAX_PENDING_SENDS;
	port->pending_count--;

}

void z_port_accept(z_port_t *out_port, const z_msg_t *connect_msg, uint32_t conn_id) {
	out_port->peer_pid = connect_msg->from;
	out_port->conn_id = conn_id;
	out_port->connected = true;
	z_msg_new_send(connect_msg->from, Z_PORT_CONNECTED, 0, z_obj_uint32(conn_id));
}

void z_port_refuse(const z_msg_t *connect_msg, const char *reason) {
	z_msg_new_send(connect_msg->from, Z_PORT_REFUSED, 0,
		z_obj_str(reason ? reason : ""));
}
