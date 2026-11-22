/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx's DHCP server (RFC 2131/2132), on every interface with
 * `<if>.dhcpd = 1` (docs/netx.md, dhcpd.h). Built with -DNETX only.
 *
 * Each such interface hands out addresses from its pool (`<if>.pool =
 * a.b.c.d-a.b.c.e`; by default .100 to .199 of its subnet, within it)
 * for `<if>.lease` seconds (3600), with the interface's own address as
 * the client's gateway and nameserver (the DNS forwarder, dnsfwd.c),
 * or the board's upstream nameserver when that is off. A client that
 * comes back gets the address it had, while nobody else has it.
 *
 * Leases live in RAM: after a restart clients ask again (and are given
 * the same address again if they ask for it and it is free). Replies
 * are broadcast, which every client takes, except to a client renewing
 * an address it has (unicast to it).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "dhcpd.h"
#include "netif.h"
#include "udp.h"

#define BOOTP_LEN	236
#define MAGIC		0x63825363u

enum { DISCOVER = 1, OFFER, REQUEST, DECLINE, ACK, NAK, RELEASE, INFORM };

typedef struct {
	uint8_t mac[6];
	uint8_t ifx;
	uint8_t bound;			/* 0 offered, 1 bound, 2 declined (in use by someone) */
	uint32_t ip;
	uint32_t expires;		/* seconds of uptime; 0 free */
} lease_t;

static lease_t leases[DHCPD_LEASES];
static uint32_t dns_for_lan;		/* 0: the interface's own address */
uint32_t dhcpd_offers, dhcpd_acks, dhcpd_naks;

static uint32_t dhcpd_now_s(void) { return z_uptime_ticks() / Z_TICK_HZ + 1; }
static uint32_t dhcpd_g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void dhcpd_p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static void pool(const netif_t *n, uint32_t *lo, uint32_t *hi)
{
	uint32_t net = n->ip & n->mask, bc = net | ~n->mask;
	*lo = n->pool_lo ? n->pool_lo : net + 100;
	*hi = n->pool_hi ? n->pool_hi : net + 199;
	if (*lo <= net) *lo = net + 1;
	if (*hi >= bc) *hi = bc - 1;
}

static lease_t *by_mac(int ifx, const uint8_t *mac)
{
	int i;
	for (i = 0; i < DHCPD_LEASES; i++)
		if (leases[i].expires && leases[i].ifx == ifx && !memcmp(leases[i].mac, mac, 6))
			return &leases[i];
	return 0;
}

/* is ip free for mac on ifx? (a lease of someone else's that is live
 * says no; an expired one is anybody's) */
static bool ip_free(int ifx, uint32_t ip, const uint8_t *mac, uint32_t now)
{
	int i;
	if (ip == netif[ifx].ip)
		return false;
	for (i = 0; i < DHCPD_LEASES; i++)
		if (leases[i].expires > now && leases[i].ip == ip &&
				(leases[i].ifx != ifx || memcmp(leases[i].mac, mac, 6)))
			return false;
	return true;
}

static lease_t *slot(uint32_t now)
{
	int i;
	for (i = 0; i < DHCPD_LEASES; i++)
		if (leases[i].expires <= now)
			return &leases[i];
	return 0;
}

static void reply(int ifx, const uint8_t *req, uint8_t type, uint32_t yiaddr, uint32_t lease)
{
	static uint8_t b[300];
	const netif_t *n = &netif[ifx];
	uint32_t ciaddr = dhcpd_g32(req + 12);
	int i = BOOTP_LEN;

	memset(b, 0, sizeof(b));
	b[0] = 2;			/* BOOTREPLY */
	b[1] = 1; b[2] = 6;
	memcpy(b + 4, req + 4, 4);	/* xid */
	memcpy(b + 10, req + 10, 2);	/* flags */
	if (type == ACK && !yiaddr)
		memcpy(b + 12, req + 12, 4);	/* INFORM: ciaddr back */
	dhcpd_p32(b + 16, yiaddr);
	dhcpd_p32(b + 20, n->ip);		/* siaddr */
	memcpy(b + 28, req + 28, 16);	/* chaddr */
	dhcpd_p32(b + i, MAGIC); i += 4;
	b[i++] = 53; b[i++] = 1; b[i++] = type;
	b[i++] = 54; b[i++] = 4; dhcpd_p32(b + i, n->ip); i += 4;
	if (type != NAK) {
		if (lease) {
			b[i++] = 51; b[i++] = 4; dhcpd_p32(b + i, lease); i += 4;
			b[i++] = 58; b[i++] = 4; dhcpd_p32(b + i, lease / 2); i += 4;
			b[i++] = 59; b[i++] = 4; dhcpd_p32(b + i, lease / 8 * 7); i += 4;
		}
		b[i++] = 1; b[i++] = 4; dhcpd_p32(b + i, n->mask); i += 4;
		b[i++] = 3; b[i++] = 4; dhcpd_p32(b + i, n->ip); i += 4;
		b[i++] = 6; b[i++] = 4; dhcpd_p32(b + i, dns_for_lan ? dns_for_lan : n->ip); i += 4;
	}
	b[i++] = 255;
	while (i < 300) b[i++] = 0;
	netif_set(ifx);
	/* a client that has its address hears it there; one that has none,
	 * by broadcast (RFC 2131 4.1) */
	udp_send(ciaddr && type != NAK ? ciaddr : 0xFFFFFFFFu, 68, 67, b, 300);
}

/* option `code` of the request, or 0 */
static const uint8_t *opt(const uint8_t *o, int n, uint8_t code, int *len)
{
	int i = 0;
	while (i < n) {
		uint8_t c = o[i];
		if (c == 255) break;
		if (c == 0) { i++; continue; }
		if (i + 1 >= n || i + 2 + o[i + 1] > n) break;
		if (c == code) { *len = o[i + 1]; return o + i + 2; }
		i += 2 + o[i + 1];
	}
	return 0;
}

static void on_request(uint32_t src_ip, uint16_t src_port, const uint8_t *d, uint16_t len)
{
	int ifx = netif_index(netif_cur), ol = 0, l;
	const netif_t *n = netif_cur;
	const uint8_t *o, *t, *mac = d + 28;
	uint32_t now = dhcpd_now_s(), want = 0, server = 0, lo, hi, ip, lt;
	lease_t *e;
	uint8_t type;

	(void)src_ip; (void)src_port;
	if (!n->dhcpd || !n->ip || len < BOOTP_LEN + 4 || d[0] != 1 || d[1] != 1 || d[2] != 6)
		return;
	if (dhcpd_g32(d + BOOTP_LEN) != MAGIC)
		return;
	o = d + BOOTP_LEN + 4;
	ol = len - BOOTP_LEN - 4;
	t = opt(o, ol, 53, &l);
	if (!t || l != 1)
		return;
	type = t[0];
	if ((t = opt(o, ol, 50, &l)) && l == 4) want = dhcpd_g32(t);
	if ((t = opt(o, ol, 54, &l)) && l == 4) server = dhcpd_g32(t);
	pool(n, &lo, &hi);
	lt = n->lease ? n->lease : 3600;
	e = by_mac(ifx, mac);

	switch (type) {
	case DISCOVER:
		ip = 0;
		if (e && e->expires > now && e->bound != 2)
			ip = e->ip;		/* what it had */
		else if (want >= lo && want <= hi && ip_free(ifx, want, mac, now))
			ip = want;
		else
			for (ip = lo; ip <= hi; ip++)
				if (ip_free(ifx, ip, mac, now)) break;
		if (ip > hi || !ip) {
			printf("netx: dhcpd: %s: no address left\n", n->name);
			return;
		}
		if (!e && !(e = slot(now)))
			return;
		memcpy(e->mac, mac, 6);
		e->ifx = (uint8_t)ifx;
		e->ip = ip;
		e->bound = 0;
		e->expires = now + 60;		/* held while it decides */
		dhcpd_offers++;
		reply(ifx, d, OFFER, ip, lt);
		return;
	case REQUEST:
		if (server && server != n->ip) {
			/* it took another server's offer */
			if (e && !e->bound) e->expires = 0;
			return;
		}
		ip = want ? want : dhcpd_g32(d + 12);	/* SELECTING/INIT-REBOOT, or renewing */
		if (!ip || ip < lo || ip > hi || !ip_free(ifx, ip, mac, now)) {
			dhcpd_naks++;
			if (server || want)	/* RFC 2131 4.3.2: silent when not ours to judge */
				reply(ifx, d, NAK, 0, 0);
			return;
		}
		if (!e && !(e = slot(now)))
			return;
		memcpy(e->mac, mac, 6);
		e->ifx = (uint8_t)ifx;
		e->ip = ip;
		e->bound = 1;
		e->expires = now + lt;
		dhcpd_acks++;
		reply(ifx, d, ACK, ip, lt);
		return;
	case DECLINE:
		/* someone else has it: keep it out of the pool for a while */
		if (e) {
			memset(e->mac, 0, 6);
			e->bound = 2;
			e->expires = now + lt;
		}
		return;
	case RELEASE:
		if (e && e->ip == dhcpd_g32(d + 12))
			e->expires = 0;
		return;
	case INFORM:
		reply(ifx, d, ACK, 0, 0);
		return;
	default:
		return;
	}
}

void dhcpd_init(uint32_t lan_dns)
{
	int i;
	bool any = false;
	memset(leases, 0, sizeof(leases));
	dns_for_lan = lan_dns;
	for (i = 0; i < netif_n; i++)
		if (netif[i].dhcpd && netif[i].ip) {
			uint32_t lo, hi;
			pool(&netif[i], &lo, &hi);
			printf("netx: dhcpd on %s: %lu.%lu.%lu.%lu-.%lu\n", netif[i].name,
				(unsigned long)(lo >> 24), (unsigned long)(lo >> 16 & 0xFF),
				(unsigned long)(lo >> 8 & 0xFF), (unsigned long)(lo & 0xFF),
				(unsigned long)(hi & 0xFF));
			any = true;
		}
	if (any)
		udp_listen(67, on_request);
}

int dhcpd_leases(void)
{
	uint32_t now = dhcpd_now_s();
	int i, n = 0;
	for (i = 0; i < DHCPD_LEASES; i++)
		if (leases[i].expires > now && leases[i].bound == 1)
			n++;
	return n;
}
