/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx's interfaces and routes. See netif.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "netif.h"
#include "eth.h"

netif_t netif[NETIF_MAX];
int netif_n;
netif_t *netif_cur = &netif[0];
bool netif_forward;
netif_route_t netif_routes[NETIF_ROUTES];
int netif_nroutes;

void netif_set(int i)
{
	netif_cur = &netif[i];
	net_phy = netif[i].phy;
	memcpy(eth_our_mac, netif[i].mac, 6);
}

static int attached(uint32_t ip)
{
	int i;
	for (i = 0; i < netif_n; i++)
		if (netif[i].up && netif[i].ip && netif[i].mask &&
				(ip & netif[i].mask) == (netif[i].ip & netif[i].mask))
			return i;
	return -1;
}

int netif_route(uint32_t dst, uint32_t *next_hop)
{
	int i, best = -1, r;
	uint32_t best_mask = 0;

	i = attached(dst);
	if (i >= 0) {
		*next_hop = dst;
		return i;
	}
	// static routes: the longest prefix whose gateway is attached
	for (r = 0; r < netif_nroutes; r++) {
		const netif_route_t *rt = &netif_routes[r];
		if ((dst & rt->mask) != rt->net)
			continue;
		if (best >= 0 && rt->mask <= best_mask)
			continue;
		i = attached(rt->via);
		if (i < 0)
			continue;
		best = r;
		best_mask = rt->mask;
	}
	if (best >= 0) {
		*next_hop = netif_routes[best].via;
		return attached(netif_routes[best].via);
	}
	// the default route: the first interface with a gateway
	for (i = 0; i < netif_n; i++)
		if (netif[i].up && netif[i].gw && attached(netif[i].gw) == i) {
			*next_hop = netif[i].gw;
			return i;
		}
	return -1;
}

bool netif_is_ours(uint32_t ip)
{
	int i;
	for (i = 0; i < netif_n; i++)
		if (netif[i].ip && netif[i].ip == ip)
			return true;
	return false;
}

uint32_t netif_src_for(uint32_t dst)
{
	uint32_t hop;
	int i = netif_route(dst, &hop);
	return i >= 0 ? netif[i].ip : netif_cur->ip;
}

bool netif_add_route(uint32_t net, uint32_t mask, uint32_t via)
{
	if (netif_nroutes >= NETIF_ROUTES)
		return false;
	netif_routes[netif_nroutes].net = net & mask;
	netif_routes[netif_nroutes].mask = mask;
	netif_routes[netif_nroutes].via = via;
	netif_nroutes++;
	return true;
}
