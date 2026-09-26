/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See sock.h.
 */

#include <string.h>
#include <stdio.h>

#include "sock.h"
#include "tcp.h"

static sock_established_handler_t app_on_established;
static sock_data_handler_t app_on_data;
static sock_closed_handler_t app_on_closed;

static bool established;

// -- outbound queue --
//
// Exists for the same reason telnet.c's does: tcp_send() accepts one
// unacknowledged segment at a time (tcp.h, "stop-and-wait"), while a
// caller can reasonably hand over a whole HTTP request in one go.
// Without this, every caller would have to implement the same
// hold-and-retry loop.
static uint8_t tx_queue[SOCK_TX_QUEUE_LEN];
static uint16_t tx_queue_len;

// The one socket's connection, or NULL. Dropped on CLOSED (tcp.h).
static tcp_conn_t *conn;

tcp_conn_t *sock_tcp(void) {
	return conn;
}

static void on_tcp_event(tcp_conn_t *c, tcp_event_t ev, const uint8_t *data, uint16_t len) {
	(void)c;

	switch (ev) {

	case TCP_EVENT_ESTABLISHED:
		established = true;
		if (app_on_established) app_on_established();
		break;

	case TCP_EVENT_DATA:
		// Straight through. No parsing, no rewriting, no buffering --
		// this layer's whole contract is that the byte stream it
		// delivers is the byte stream the peer sent.
		if (app_on_data && len) app_on_data(data, len);
		break;

	case TCP_EVENT_EOF:
		// never: this connection does not ask for half-close (tcp.h)
		break;

	case TCP_EVENT_CLOSED:
		established = false;
		tx_queue_len = 0;
		conn = NULL;
		if (app_on_closed) app_on_closed();
		break;

	}

}

bool sock_connect(uint32_t ip, uint16_t port,
	sock_established_handler_t on_established,
	sock_data_handler_t on_data,
	sock_closed_handler_t on_closed) {

	app_on_established = on_established;
	app_on_data = on_data;
	app_on_closed = on_closed;

	established = false;
	tx_queue_len = 0;

	if (conn) return false;
	conn = tcp_connect(ip, port, on_tcp_event, NULL);
	return conn != NULL;

}

bool sock_send(const uint8_t *data, uint16_t len) {

	if (!established) return false;
	if (len == 0) return true;

	// All or nothing. See sock.h: a partial write would leave the
	// caller tracking how much of a TLS record went out, which it
	// cannot meaningfully do.
	if ((uint32_t)tx_queue_len + len > SOCK_TX_QUEUE_LEN) return false;

	memcpy(tx_queue + tx_queue_len, data, len);
	tx_queue_len = (uint16_t)(tx_queue_len + len);

	return true;

}

void sock_close(void) {
	if (conn) tcp_close(conn);
}

void sock_abort(void) {
	tx_queue_len = 0;
	established = false;
	if (conn) tcp_abort(conn);
	conn = NULL;
}

void sock_poll(void) {

	uint16_t n;

	if (!established || tx_queue_len == 0) return;

	n = tx_queue_len > tcp_mss(conn) ? tcp_mss(conn) : tx_queue_len;

	if (tcp_send(conn, tx_queue, n)) {
		memmove(tx_queue, tx_queue + n, (size_t)(tx_queue_len - n));
		tx_queue_len = (uint16_t)(tx_queue_len - n);
	}
	// A false return means the previous segment is still
	// unacknowledged; the bytes stay queued and the next poll tries
	// again. Same shape as telnet_poll().

}
