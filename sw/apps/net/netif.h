#ifndef NETIF_H
#define NETIF_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx's network interfaces (built with -DNETX only; flash net has one
 * and none of this). docs/netx.md.
 *
 * -- How several interfaces fit a stack written for one --
 *
 * eth.c, arp.c and ip.c were written around one NIC, one MAC and one
 * address. netx keeps them as they are and gives them a CURRENT
 * interface instead: in a -DNETX build their `net_phy`, `eth_our_mac`
 * and our address, netmask and gateway are the current interface's
 * (netif_cur). Two rules keep "current" right:
 *
 *   - a received frame is handled with the interface it came in on
 *     current (eth_poll() sets it before each frame), so ARP replies,
 *     ICMP and anything answered at once go back out the same way;
 *   - a packet the board sends to a unicast address picks its
 *     interface by route first (ip_send_built(): netif_route()), and
 *     the source address it carries is that interface's.
 *
 * Broadcasts and multicast go out the current interface; DHCP sets the
 * one it works on before it starts.
 *
 * Forwarding (ip_forward(), ip.c) is off unless net.cfg asks for it:
 * `forward = 1`. Static routes: `route = 10.0.0.0/8 via 192.168.1.1`.
 */

#include <stdint.h>
#include <stdbool.h>
#include "net_phy.h"

#ifndef NETIF_MAX
#define NETIF_MAX	3
#endif
#define NETIF_ROUTES	8

typedef struct {
	const net_phy_t *phy;
	uint8_t mac[6];
	uint32_t ip, mask, gw;		// 0: not configured
	char name[6];			// "wan", "lan", "lan2", or the PHY's ("usbd")
	bool up;			// the PHY started
	bool nat;			// masquerade what leaves by this (nat.h)
	bool dhcpd;			// a DHCP server here (dhcpd.h)
	uint32_t pool_lo, pool_hi;	// its addresses (0: .100-.199 of the subnet)
	uint32_t lease;			// seconds (0: 3600)
	// counters
	uint32_t rx, tx, fwd_in, fwd_out, drop;
} netif_t;

typedef struct {
	uint32_t net, mask, via;
} netif_route_t;

extern netif_t netif[NETIF_MAX];
extern int netif_n;			// how many are in use
extern netif_t *netif_cur;		// the current interface (above)
extern bool netif_forward;		// net.cfg forward = 1
extern netif_route_t netif_routes[NETIF_ROUTES];
extern int netif_nroutes;

static inline int netif_index(const netif_t *n) { return (int)(n - netif); }

// Make interface i current: netif_cur, and with it net_phy (the PHY
// eth.c sends on) and eth_our_mac (the MAC it sends from).
void netif_set(int i);

// The interface a packet for `dst` leaves by, and its next hop:
// a directly attached subnet first, then the static routes, then the
// first interface with a gateway. -1: no route.
int netif_route(uint32_t dst, uint32_t *next_hop);

// Is `ip` one of the board's own addresses (any interface)?
bool netif_is_ours(uint32_t ip);

// The source address for a packet to `dst`: its interface's.
uint32_t netif_src_for(uint32_t dst);

// Add a route; false when the table is full.
bool netif_add_route(uint32_t net, uint32_t mask, uint32_t via);

#endif
