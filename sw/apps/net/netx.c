/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx's start: every interface it has, each with its address
 * (docs/netx.md, netif.h). Built with -DNETX only.
 *
 * Which interfaces:
 *
 *   - net.cfg names them (wan., lan., lan2.): those, in that order,
 *     each on the PHY its .phy says (builtin, builtin2, usb, usbdev);
 *     without .phy, wan is the built-in MAC (or a USB adapter on a
 *     board with none) and lan and lan2 the second built-in MAC
 *     (Klinge's Ethernet B), then the USB-C device, then an adapter.
 *   - it names none: every PHY the board has -- the built-in MAC(s),
 *     the USB-C device, a USB adapter that is plugged in -- with net.cfg's
 *     plain keys (phy, dhcp, ip, mask, gw) for the first, as flash net
 *     takes them, and DHCP for the others.
 *
 * Addresses: .ip (a.b.c.d/len) and .gw; .dhcp = 1 to ask a DHCP server
 * (the default for an interface with no .ip).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "netx.h"
#include "netif.h"
#include "net_phy.h"
#include "dhcp.h"
#include "eth.h"
#include "arp.h"
#include "ip.h"
#include "tcp.h"
#include "usb_ecm.h"
#include "nat.h"
#include "dhcpd.h"
#include "dnsfwd.h"
#include "dns.h"
#include "fw.h"

static void print_ip(uint32_t a)
{
	printf("%lu.%lu.%lu.%lu", (unsigned long)(a >> 24 & 0xFF),
		(unsigned long)(a >> 16 & 0xFF), (unsigned long)(a >> 8 & 0xFF),
		(unsigned long)(a & 0xFF));
}

static bool have_phy(const net_phy_t *p)
{
	int i;
	for (i = 0; i < netif_n; i++)
		if (netif[i].phy == p)
			return true;
	return false;
}

/* the PHY for a role with no .phy: the first of `kinds` there is */
static const net_phy_t *first_of(const int *kinds, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		const net_phy_t *p = net_phy_of(kinds[i]);
		if (p && !have_phy(p))
			return p;
	}
	return 0;
}

static void add(const net_phy_t *p, const char *name, const netcfg_if_t *f)
{
	netif_t *n;
	if (!p || have_phy(p) || netif_n >= NETIF_MAX)
		return;
	n = &netif[netif_n];
	memset(n, 0, sizeof(*n));
	n->phy = p;
	strncpy(n->name, name, sizeof(n->name) - 1);
	if (f) {
		n->ip = f->ip;
		n->mask = f->mask;
		n->gw = f->gw;
		n->nat = f->nat != 0;
		n->dhcpd = f->dhcpd != 0;
		n->pool_lo = f->pool_lo;
		n->pool_hi = f->pool_hi;
		n->lease = f->lease;
	}
	netif_n++;
}

int netx_bringup(const netcfg_t *cfg, const uint8_t base_mac[6], uint32_t *dns)
{
	static const int wan_kinds[] = { NET_PHY_BUILTIN, NET_PHY_USB, NET_PHY_USBDEV, NET_PHY_BUILTIN2, NET_PHY_XLINK };
	static const int lan_kinds[] = { NET_PHY_BUILTIN2, NET_PHY_USBDEV, NET_PHY_XLINK, NET_PHY_USB, NET_PHY_BUILTIN };
	int want_dhcp[NETIF_MAX];
	int i, up = 0;

	netif_n = 0;
	if (cfg->n_if) {
		for (i = 0; i < cfg->n_if; i++) {
			const netcfg_if_t *f = &cfg->ifs[i];
			const net_phy_t *p = f->phy >= 0 ? net_phy_of(f->phy) :
				!strcmp(f->name, "wan") ? first_of(wan_kinds, 5) :
				first_of(lan_kinds, 5);
			if (!p) {
				printf("netx: %s: no such interface on this board\n", f->name);
				continue;
			}
			want_dhcp[netif_n] = f->dhcp >= 0 ? f->dhcp : (f->ip == 0);
			add(p, f->name, f);
		}
	} else {
		/* every PHY the board has; the first takes net.cfg's plain keys */
		const net_phy_t *first = net_phy_of(cfg->phy == NET_PHY_AUTO ?
			NET_PHY_BUILTIN : cfg->phy);
		netcfg_if_t plain;
		memset(&plain, 0, sizeof(plain));
		plain.ip = cfg->ip;
		plain.mask = cfg->mask;
		plain.gw = cfg->gw;
		if (!first)
			first = first_of(wan_kinds, 5);
		if (first) {
			want_dhcp[0] = cfg->dhcp >= 0 ? cfg->dhcp : 1;
			add(first, first->name, &plain);
		}
		if (cfg->phy == NET_PHY_AUTO) {
			const net_phy_t *p;
			if ((p = net_phy_of(NET_PHY_BUILTIN2)) && !have_phy(p)) {
				want_dhcp[netif_n] = 1;
				add(p, p->name, 0);
			}
			if ((p = net_phy_of(NET_PHY_XLINK)) && !have_phy(p)) {
				want_dhcp[netif_n] = 1;
				add(p, p->name, 0);
			}
			if ((p = net_phy_of(NET_PHY_USBDEV)) && !have_phy(p)) {
				want_dhcp[netif_n] = 1;
				add(p, p->name, 0);
			}
			if ((p = net_phy_of(NET_PHY_USB)) && !have_phy(p) && usb_ecm_present()) {
				want_dhcp[netif_n] = 1;
				add(p, p->name, 0);
			}
		}
	}

	/* the PHYs: each its own MAC (the base, interface number in byte 4),
	 * unless it has one of its own (a USB adapter) */
	for (i = 0; i < netif_n; i++) {
		netif_t *n = &netif[i];
		memcpy(n->mac, base_mac, 6);
		n->mac[4] = (uint8_t)(base_mac[4] + i);
		netif_set(i);
		printf("netx: %s: starting %s\n", n->name, n->phy->name);
		if (!n->phy->init(n->mac)) {
			printf("netx: %s: %s did not start\n", n->name, n->phy->name);
			continue;
		}
		if (n->phy->get_mac)
			n->phy->get_mac(n->mac);
		n->up = true;
		up++;
	}
	if (!up)
		return -1;

	/* the smallest receive buffer and MSS of them bound TCP (net_phy.h) */
	{
		uint16_t cap = 0xFFFF, mss = 0xFFFF;
		for (i = 0; i < netif_n; i++) {
			const net_phy_t *p = netif[i].phy;
			if (!netif[i].up) continue;
			if (p->rx_capacity < cap) cap = p->rx_capacity;
			if ((p->rx_mss ? p->rx_mss : TCP_ADVERTISE_MSS) < mss)
				mss = p->rx_mss ? p->rx_mss : TCP_ADVERTISE_MSS;
		}
		tcp_set_rx_mss(mss);
		tcp_set_rx_window_max(cap);
	}

	{
		/* arp_init() clears the cache and the current interface's
		 * address with it: keep the one net.cfg gave */
		uint32_t keep = netif[0].ip;
		netif_set(0);
		arp_init(0);
		netif[0].ip = keep;
	}

	/* the addresses */
	for (i = 0; i < netif_n; i++) {
		netif_t *n = &netif[i];
		uint32_t ip, mask, gw, d = 0;
		if (!n->up)
			continue;
		netif_set(i);
		if (want_dhcp[i]) {
			uint32_t keep_ip = n->ip, keep_mask = n->mask, keep_gw = n->gw;
			n->ip = 0;		/* takes DHCP's replies (ip.c) */
			if (dhcp_acquire(n->mac, &ip, &mask, &gw, &d)) {
				n->ip = ip;
				n->mask = mask;
				n->gw = gw;
				if (d && !*dns)
					*dns = d;
			} else {
				n->ip = keep_ip;
				n->mask = keep_mask;
				n->gw = keep_gw;
				printf("netx: %s: no DHCP server answered\n", n->name);
			}
		}
		if (n->ip && !n->mask)
			n->mask = 0xFFFFFF00u;
	}

	/* the firewall: each interface's services, then the rules */
	if (cfg->n_if) {
		int k;
		for (k = 0; k < cfg->n_if; k++) {
			char list[64], *w;
			int ifx;
			for (ifx = 0; ifx < netif_n; ifx++)
				if (!strcmp(netif[ifx].name, cfg->ifs[k].name))
					break;
			if (ifx == netif_n || !cfg->ifs[k].allow[0])
				continue;
			strcpy(list, cfg->ifs[k].allow);
			for (w = strtok(list, ", "); w; w = strtok(0, ", "))
				if (!fw_allow(ifx, w))
					printf("netx: %s.allow: '%s' is not a service\n", netif[ifx].name, w);
		}
	}
	if (fw_load() != 0)
		printf("netx: firewall.cfg not used\n");

	/* NAT is forwarding: one implies the other */
	netif_forward = cfg->forward != 0;
	for (i = 0; i < netif_n; i++)
		if (netif[i].nat)
			netif_forward = true;
	if (netif_forward) {
		bool any_nat = false;
		for (i = 0; i < netif_n; i++)
			any_nat |= netif[i].nat;
		if (any_nat) {
			if (!nat_init(cfg->nat_size))
				printf("netx: no memory for the NAT table\n");
			for (i = 0; i < cfg->n_portfwd; i++)
				nat_add_forward(cfg->portfwd[i].proto, cfg->portfwd[i].wan_port,
					cfg->portfwd[i].lan_ip, cfg->portfwd[i].lan_port);
		}
	}
	netif_nroutes = 0;
	for (i = 0; i < cfg->n_routes; i++)
		netif_add_route(cfg->routes[i].net, cfg->routes[i].mask, cfg->routes[i].via);

	for (i = 0; i < netif_n; i++) {
		netif_t *n = &netif[i];
		printf("netx: %s (%s) %s mac %02x:%02x:%02x:%02x:%02x:%02x ",
			n->name, n->phy->name, n->up ? "up" : "DOWN",
			n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
		print_ip(n->ip);
		printf("/");
		print_ip(n->mask);
		if (n->gw) {
			printf(" gw ");
			print_ip(n->gw);
		}
		printf("\n");
	}
	printf("netx: forwarding %s, %d static route(s)", netif_forward ? "on" : "off",
		netif_nroutes);
	if (nat_size())
		printf(", NAT %d connections, %d port forward(s)", nat_size(), cfg->n_portfwd);
	printf("\n");
	netif_set(0);
	return 0;
}

/* The main loop's wait: the least any polled interface asks for, plus
 * one (net_phy.h idle_ticks); 0 when none of them polls. */
uint32_t netx_idle_ticks(void)
{
	uint32_t best = 0;
	int i;
	for (i = 0; i < netif_n; i++) {
		const net_phy_t *p = netif[i].phy;
		uint32_t w;
		if (!netif[i].up || !p->idle_ticks)
			continue;
		w = p->idle_ticks() + 1;
		if (!best || w < best)
			best = w;
	}
	return best;
}

/* The services for the LAN side, once the nameserver is known: the DHCP
 * server on every interface with .dhcpd, and the DNS forwarder there
 * unless dnsfwd = 0 (DHCP then hands out the board's own nameserver). */
void netx_services(const netcfg_t *cfg)
{
	bool any = false;
	int i;
	for (i = 0; i < netif_n; i++)
		any |= netif[i].dhcpd && netif[i].up;
	if (!any)
		return;
	if (cfg->dnsfwd != 0) {
		dnsfwd_init(dns_nameserver);
		dhcpd_init(0);
	} else {
		dhcpd_init(dns_nameserver());
	}
}
