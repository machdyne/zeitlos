/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * See zport.h.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zeitlos.h"
#include "zobj.h"
#include "zmsg.h"
#include "zport.h"

// ~2 seconds at the kernel tick rate (~732Hz -- see sw/os/kernel.c's
// z_kernel_ticks comment). generous enough that a provider that's
// merely slow to get scheduled still connects fine, short enough that
// a provider that isn't running at all doesn't hang the caller.
#define Z_PORT_CONNECT_TIMEOUT_TICKS (732 * 2)

z_rv z_port_connect(z_port_t *port, uint32_t provider_pid) {

	port->peer_pid = provider_pid;
	port->conn_id = 0;
	port->connected = false;

	z_msg_new_send(provider_pid, Z_PORT_CONNECT, 0, z_obj_none());

	uint32_t start = z_uptime_ticks();

	while ((z_uptime_ticks() - start) < Z_PORT_CONNECT_TIMEOUT_TICKS) {

		z_msg_t msg;
		if (z_msg_read(&msg) != Z_OK) continue;

		if (msg.subject == Z_PORT_CONNECTED && msg.tag == 0) {
			port->conn_id = msg.obj.val.uint32;
			port->connected = true;
			return Z_OK;
		}

		if (msg.subject == Z_PORT_REFUSED && msg.tag == 0)
			return Z_FAIL;

		// not a reply to our CONNECT -- discard and keep waiting, same
		// as z_msg_wait() (zeitlos.c) does for any RPC-style exchange

	}

	return Z_FAIL;	// timed out -- provider likely isn't running

}

z_rv z_port_send(z_port_t *port, const void *data, uint32_t len) {
	if (!port->connected) return Z_FAIL;
	return z_msg_new_send(port->peer_pid, Z_PORT_DATA, port->conn_id,
		z_obj_blob(data, len));
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
