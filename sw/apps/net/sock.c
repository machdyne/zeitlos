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

// One per socket. The connection's slot index rides on tcp.c's user
// pointer, so an event goes straight to its slot.
typedef struct {
	tcp_conn_t	*conn;
	bool		established;
	uint16_t	tx_queue_len;
	uint8_t		tx_queue[SOCK_TX_QUEUE_LEN];
} sock_slot_t;

static sock_slot_t slots[NET_SOCK_SLOTS];

static sock_established_handler_t app_on_established;
static sock_data_handler_t app_on_data;
static sock_closed_handler_t app_on_closed;

tcp_conn_t *sock_tcp(int s) {
	return (s >= 0 && s < NET_SOCK_SLOTS) ? slots[s].conn : NULL;
}

int sock_free_slot(void) {
	for (int i = 0; i < NET_SOCK_SLOTS; i++)
		if (!slots[i].conn) return i;
	return -1;
}

static void on_tcp_event(tcp_conn_t *c, tcp_event_t ev, const uint8_t *data, uint16_t len) {

	int s = (int)(intptr_t)tcp_user(c);
	sock_slot_t *k;

	if (s < 0 || s >= NET_SOCK_SLOTS || slots[s].conn != c) return;
	k = &slots[s];

	switch (ev) {

	case TCP_EVENT_ESTABLISHED:
		k->established = true;
		if (app_on_established) app_on_established(s);
		break;

	case TCP_EVENT_DATA:
		if (app_on_data && len) app_on_data(s, data, len);
		break;

	// The peer has finished sending. Nothing to do here: the receive
	// side is relayed as it arrives, and the connection stays open
	// until tcp.c reports it CLOSED.
	case TCP_EVENT_EOF:
		break;

	case TCP_EVENT_CLOSED:
		k->established = false;
		k->tx_queue_len = 0;
		k->conn = NULL;
		if (app_on_closed) app_on_closed(s);
		break;

	}

}

bool sock_connect(int s, uint32_t ip, uint16_t port,
	sock_established_handler_t on_established,
	sock_data_handler_t on_data,
	sock_closed_handler_t on_closed) {

	sock_slot_t *k;

	if (s < 0 || s >= NET_SOCK_SLOTS) return false;
	k = &slots[s];
	if (k->conn) return false;

	app_on_established = on_established;
	app_on_data = on_data;
	app_on_closed = on_closed;

	k->established = false;
	k->tx_queue_len = 0;
	k->conn = tcp_connect(ip, port, on_tcp_event, (void *)(intptr_t)s);
	return k->conn != NULL;

}

bool sock_send(int s, const uint8_t *data, uint16_t len) {

	sock_slot_t *k;

	if (s < 0 || s >= NET_SOCK_SLOTS) return false;
	k = &slots[s];
	if (!k->established) return false;
	if (len == 0) return true;
	if ((uint32_t)k->tx_queue_len + len > SOCK_TX_QUEUE_LEN) return false;

	memcpy(k->tx_queue + k->tx_queue_len, data, len);
	k->tx_queue_len = (uint16_t)(k->tx_queue_len + len);
	return true;

}

void sock_close(int s) {
	if (s >= 0 && s < NET_SOCK_SLOTS && slots[s].conn) tcp_close(slots[s].conn);
}

void sock_abort(int s) {
	sock_slot_t *k;
	if (s < 0 || s >= NET_SOCK_SLOTS) return;
	k = &slots[s];
	k->tx_queue_len = 0;
	k->established = false;
	if (k->conn) tcp_abort(k->conn);
	k->conn = NULL;
}

// One segment per slot per call: tcp.c keeps one unacknowledged
// segment per connection, so offering more would only be refused.
void sock_poll(void) {

	for (int s = 0; s < NET_SOCK_SLOTS; s++) {

		sock_slot_t *k = &slots[s];
		uint16_t n;

		if (!k->established || k->tx_queue_len == 0) continue;

		n = k->tx_queue_len > tcp_mss(k->conn) ? tcp_mss(k->conn) : k->tx_queue_len;
		if (tcp_send(k->conn, k->tx_queue, n)) {
			memmove(k->tx_queue, k->tx_queue + n, (size_t)(k->tx_queue_len - n));
			k->tx_queue_len = (uint16_t)(k->tx_queue_len - n);
		}

	}

}
