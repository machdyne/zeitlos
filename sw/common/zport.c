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

	z_obj_t obj = z_obj_blob(data, len);

	// z_obj_blob() (zobj.c) returns Z_NONE instead of a broken object
	// when its internal malloc() fails (see that function's own
	// comment), but z_msg_send() below would still report Z_OK for a
	// Z_NONE payload -- from the caller's perspective this call would
	// look like it "succeeded" while silently sending nothing at all.
	// Surface it here instead of leaving it invisible -- this is what
	// caught a real Z_PROC_STACK_SIZE-too-small bug (sw/os/kernel.c)
	// on real hardware, where even a 2-byte allocation failed.
	if (len > 0 && obj.type != Z_BLOB)
		printf("zport: z_obj_blob() failed to allocate %lu bytes -- "
			"heap likely exhausted\n", (unsigned long)len);

	// Deliberately never freed here -- see z_port_t's own comment
	// (zport.h) for why an earlier "free the previous send's blob on
	// the next send" scheme was tried and reverted: it assumed the
	// peer had a scheduling slot to read the previous message before
	// the next z_port_send() call, which is FALSE whenever a caller
	// makes several sends back-to-back with no yield in between --
	// e.g. `repl`'s own handle_connect() (banner, then prompt,
	// immediately) or its per-line response (text, then "\r\n", then
	// the next prompt). Freeing the banner's blob before `term` had
	// necessarily read it, followed immediately by the prompt's
	// z_obj_blob() call reusing that exact just-freed memory, meant
	// the banner message resolved to the PROMPT's own bytes by the
	// time `term` actually read it -- confirmed on real hardware:
	// `term` never saw the banner's true length at all, only two
	// prompt-sized DATA messages in a row. Reverted to the same
	// intentional, accepted leak `pong`'s own reply strings already
	// use (docs/messaging.md's ping/pong example) -- now backed by a
	// much larger per-process heap (Z_PROC_STACK_SIZE, kernel.c,
	// raised specifically because of this) rather than a free that
	// turned out to be unsafe. See docs/messaging.md's "Known
	// limitations" for the general version of this problem -- a real
	// fix needs the receiver's own read to be what proves safety, not
	// the sender's next unrelated action.
	return z_msg_new_send(port->peer_pid, Z_PORT_DATA, port->conn_id, obj);

}

void z_port_close(z_port_t *port) {
	if (!port->connected) return;
	z_msg_new_send(port->peer_pid, Z_PORT_CLOSE, port->conn_id, z_obj_none());
	port->connected = false;
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
