#ifndef NAT_H
#define NAT_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx's NAT: masquerade on an interface with `<if>.nat = 1`
 * (docs/netx.md). Built with -DNETX only.
 *
 * What leaves by a NAT interface from another one leaves from that
 * interface's address, with a port (an ICMP echo id) of its own from
 * NAT_PORT_LO-NAT_PORT_HI; what comes back to that address and port is
 * sent on to where it came from. Everything else that arrives at the
 * address is the board's own, as before. TCP, UDP and ICMP echo;
 * other protocols, and fragments after the first, are not translated.
 *
 * The table of connections (nat.size in net.cfg, 256 by default) is
 * also what the firewall will follow (X3). Entries end by timeout:
 * TCP 60 s until both sides have spoken, then 7440 s (RFC 5382), 120 s
 * after a FIN, 10 s after a RST; UDP 30 s, 180 s once answered (RFC
 * 4787); ICMP echo 60 s.
 *
 * Port forwards: `portfwd = tcp 2222 192.168.7.2:22` -- a connection
 * from outside to that port of the NAT interface goes to that host.
 */

#include <stdint.h>
#include <stdbool.h>

#define NAT_PORT_LO	20000
#define NAT_PORT_HI	29999
#define NAT_FWD_MAX	16

typedef struct {
	uint8_t proto;			// 6 TCP, 17 UDP, 1 ICMP echo
	uint8_t state;
	uint16_t lan_port, wan_port, rem_port;
	uint32_t lan_ip, rem_ip;
	uint32_t expires;		// seconds of uptime; 0: free
} nat_ent_t;

typedef struct {
	uint8_t proto;
	uint16_t wan_port, lan_port;
	uint32_t lan_ip;
} nat_fwd_t;

// The table, `size` entries (0: the default). false: no memory.
bool nat_init(int size);

// A port forward; false when there are NAT_FWD_MAX.
bool nat_add_forward(uint8_t proto, uint16_t wan_port, uint32_t lan_ip, uint16_t lan_port);

// Out by a NAT interface (wan_ip): the packet at q (len bytes, its IP
// header first, in the transmit frame) rewritten to leave from wan_ip
// and its port. false: not translatable, or the table is full -- drop.
bool nat_out(uint8_t *q, uint16_t len, uint32_t wan_ip);

// In by a NAT interface, to its own address: the connection it belongs
// to, rewritten in the transmit frame (q, a copy of p) to go to the
// host behind. true: done, *lan_ip is where it goes now; false: not a
// NAT connection's -- the board's own.
bool nat_in(const uint8_t *p, uint16_t len, uint8_t *q, uint32_t *lan_ip);

// For the status page and the tests.
int nat_used(void);
int nat_size(void);
extern uint32_t nat_full_drops, nat_out_count, nat_in_count;

#endif
