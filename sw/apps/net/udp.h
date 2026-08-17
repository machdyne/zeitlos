#ifndef UDP_H
#define UDP_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * UDP: per-port receive dispatch, send. No checksum is computed or
 * verified -- valid per RFC 768 (a UDP checksum of 0 means "not
 * used"), and IP-level checksums already cover header corruption.
 * Chosen deliberately for simplicity; revisit if this ever needs to
 * run over a link less trustworthy than a local dev network.
 */

#include <stdint.h>
#include <stdbool.h>

#define UDP_HDR_LEN     8
#define UDP_MAX_PAYLOAD 1472	// leaves room for UDP+IP headers within ETH_MTU

// called for each received datagram on a port registered via
// udp_listen(). src_ip/src_port identify who sent it.
typedef void (*udp_handler_t)(uint32_t src_ip, uint16_t src_port,
	const uint8_t *data, uint16_t len);

// registers (or replaces) the handler for a port. only one handler
// per port -- no fan-out; fine for a single TFTP client on its own
// ephemeral port, revisit if more concurrent listeners are needed.
void udp_listen(uint16_t port, udp_handler_t handler);
void udp_close(uint16_t port);

bool udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
	const uint8_t *data, uint16_t len);

// dispatched from ip_handle() for protocol 17 (UDP)
void udp_handle(uint32_t src_ip, const uint8_t *payload, uint16_t len);

#endif
