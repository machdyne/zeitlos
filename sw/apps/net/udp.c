/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * UDP. See udp.h.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "udp.h"
#include "ip.h"
#include "../../common/zeitlos.h"

#define UDP_MAX_LISTENERS 4

typedef struct {
	bool used;
	uint16_t port;
	udp_handler_t handler;
} udp_listener_t;

static udp_listener_t listeners[UDP_MAX_LISTENERS];

void udp_listen(uint16_t port, udp_handler_t handler) {

	for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
		if (listeners[i].used && listeners[i].port == port) {
			listeners[i].handler = handler;
			return;
		}
	}

	for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
		if (!listeners[i].used) {
			listeners[i].used = true;
			listeners[i].port = port;
			listeners[i].handler = handler;
			return;
		}
	}

	// no free slot -- silently ignored. UDP_MAX_LISTENERS (4) comfortably
	// covers current usage (one TFTP transfer at a time); raise it if
	// more concurrent listeners are ever needed.

}

void udp_close(uint16_t port) {
	for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
		if (listeners[i].used && listeners[i].port == port)
			listeners[i].used = false;
	}
}

bool udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
	const uint8_t *data, uint16_t len) {

	static uint8_t pkt[UDP_HDR_LEN + UDP_MAX_PAYLOAD];
	if (len > UDP_MAX_PAYLOAD) return false;

	uint16_t total_len = UDP_HDR_LEN + len;

	pkt[0] = (src_port >> 8) & 0xFF;
	pkt[1] = src_port & 0xFF;
	pkt[2] = (dst_port >> 8) & 0xFF;
	pkt[3] = dst_port & 0xFF;
	pkt[4] = (total_len >> 8) & 0xFF;
	pkt[5] = total_len & 0xFF;
	pkt[6] = 0; pkt[7] = 0;	// checksum: not computed, see udp.h

	for (uint16_t i = 0; i < len; i++) pkt[UDP_HDR_LEN + i] = data[i];

	return ip_send(dst_ip, 17, pkt, total_len);	// protocol 17 = UDP

}

void udp_handle(uint32_t src_ip, const uint8_t *p, uint16_t len) {

	if (len < UDP_HDR_LEN) return;

	uint16_t src_port = (p[0] << 8) | p[1];
	uint16_t dst_port = (p[2] << 8) | p[3];
	uint16_t total_len = (p[4] << 8) | p[5];
	// p[6]/p[7] = checksum -- not verified, see udp.h

	if (total_len > len) return;	// truncated

	const uint8_t *payload = p + UDP_HDR_LEN;
	uint16_t paylen = total_len - UDP_HDR_LEN;

	for (int i = 0; i < UDP_MAX_LISTENERS; i++) {
		if (listeners[i].used && listeners[i].port == dst_port) {
			listeners[i].handler(src_ip, src_port, payload, paylen);
			return;
		}
	}

	// no listener on this port -- silently dropped (a real UDP stack
	// would send an ICMP port-unreachable; not implemented, unlikely
	// to matter for this dev-tool use case)

}
