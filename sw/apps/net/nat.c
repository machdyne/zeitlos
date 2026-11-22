/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx's NAT. See nat.h.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "nat.h"

#define NAT_DEFAULT	256

enum { S_NEW, S_EST, S_FIN, S_RST };

static nat_ent_t *tab;
static int tab_n;
static nat_fwd_t fwds[NAT_FWD_MAX];
static int nfwd;
static uint16_t next_port = NAT_PORT_LO;
uint32_t nat_full_drops, nat_out_count, nat_in_count;

static uint32_t nat_now_s(void)
{
	return z_uptime_ticks() / Z_TICK_HZ + 1;	/* never 0: 0 is a free entry */
}

static uint16_t nat_g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t nat_g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void nat_p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void nat_p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* RFC 1624 eqn. 3, on the checksum stored at c: one 16-bit word old -> new */
static void adj16(uint8_t *c, uint16_t old, uint16_t new_)
{
	uint32_t s = (uint16_t)~nat_g16(c) + (uint32_t)(uint16_t)~old + new_;
	s = (s & 0xFFFF) + (s >> 16);
	s = (s & 0xFFFF) + (s >> 16);
	nat_p16(c, (uint16_t)~s);
}

static void adj32(uint8_t *c, uint32_t old, uint32_t new_)
{
	adj16(c, (uint16_t)(old >> 16), (uint16_t)(new_ >> 16));
	adj16(c, (uint16_t)old, (uint16_t)new_);
}

bool nat_init(int size)
{
	if (size <= 0)
		size = NAT_DEFAULT;
	free(tab);
	tab = calloc((size_t)size, sizeof(*tab));
	tab_n = tab ? size : 0;
	nfwd = 0;
	return tab != 0;
}

bool nat_add_forward(uint8_t proto, uint16_t wan_port, uint32_t lan_ip, uint16_t lan_port)
{
	if (nfwd >= NAT_FWD_MAX)
		return false;
	fwds[nfwd].proto = proto;
	fwds[nfwd].wan_port = wan_port;
	fwds[nfwd].lan_ip = lan_ip;
	fwds[nfwd].lan_port = lan_port;
	nfwd++;
	return true;
}

int nat_size(void) { return tab_n; }

int nat_used(void)
{
	uint32_t now = nat_now_s();
	int i, n = 0;
	for (i = 0; i < tab_n; i++)
		if (tab[i].expires > now)
			n++;
	return n;
}

static bool port_taken(uint8_t proto, uint16_t port, uint32_t now)
{
	int i;
	for (i = 0; i < tab_n; i++)
		if (tab[i].expires > now && tab[i].proto == proto && tab[i].wan_port == port)
			return true;
	for (i = 0; i < nfwd; i++)
		if (fwds[i].proto == proto && fwds[i].wan_port == port)
			return true;
	return false;
}

static uint16_t new_port(uint8_t proto, uint32_t now)
{
	int tries;
	for (tries = 0; tries <= NAT_PORT_HI - NAT_PORT_LO; tries++) {
		uint16_t p = next_port++;
		if (next_port > NAT_PORT_HI)
			next_port = NAT_PORT_LO;
		if (!port_taken(proto, p, now))
			return p;
	}
	return 0;
}

static nat_ent_t *alloc_ent(uint32_t now)
{
	int i;
	for (i = 0; i < tab_n; i++)
		if (tab[i].expires <= now)
			return &tab[i];
	return 0;
}

/* the timeout after a packet: `in` is from the outside */
static void touch(nat_ent_t *e, const uint8_t *l4, bool in, uint32_t now)
{
	uint32_t t;
	if (e->proto == 6) {
		uint8_t fl = l4[13];
		if (fl & 0x04)
			e->state = S_RST;
		else if (fl & 0x01)
			e->state = S_FIN;
		else if (in && e->state == S_NEW)
			e->state = S_EST;
		t = e->state == S_RST ? 10 : e->state == S_FIN ? 120 :
			e->state == S_EST ? 7440 : 60;
	} else if (e->proto == 17) {
		if (in)
			e->state = S_EST;
		t = e->state == S_EST ? 180 : 30;
	} else {
		t = 60;
	}
	e->expires = now + t;
}

/* the protocol's ports at l4 (TCP, UDP), or the echo id (ICMP): false
 * when the packet is not one this translates */
static bool ports(const uint8_t *q, uint16_t len, bool in, uint8_t *proto,
		uint16_t *src, uint16_t *dst, const uint8_t **l4)
{
	uint8_t ihl = (q[0] & 0x0F) * 4;
	uint16_t n;
	if (len < ihl || ((q[6] & 0x1F) | q[7]))
		return false;		/* a fragment after the first */
	n = (uint16_t)(len - ihl);
	*proto = q[9];
	*l4 = q + ihl;
	if (*proto == 6 && n >= 20) {
		*src = nat_g16(*l4);
		*dst = nat_g16(*l4 + 2);
		return true;
	}
	if (*proto == 17 && n >= 8) {
		*src = nat_g16(*l4);
		*dst = nat_g16(*l4 + 2);
		return true;
	}
	if (*proto == 1 && n >= 8 && (*l4)[0] == (in ? 0 : 8)) {
		/* an echo request out, its reply in: the id is the "port" */
		*src = *dst = nat_g16(*l4 + 4);
		return true;
	}
	return false;
}

/* rewrite one end of the packet: the address at a (12 source, 16
 * destination) and the port at l4 + po, with every checksum */
static void rewrite(uint8_t *q, uint8_t *l4, uint8_t proto, int a, int po,
		uint32_t ip, uint16_t port)
{
	uint32_t old_ip = nat_g32(q + a);
	uint16_t old_port;
	if (proto == 1)
		po = 4;			/* the echo id */
	old_port = nat_g16(l4 + po);
	adj32(q + 10, old_ip, ip);
	if (proto == 6) {
		adj32(l4 + 16, old_ip, ip);
		adj16(l4 + 16, old_port, port);
	} else if (proto == 17) {
		if (nat_g16(l4 + 6)) {		/* 0: the sender computed none */
			adj32(l4 + 6, old_ip, ip);
			adj16(l4 + 6, old_port, port);
			if (!nat_g16(l4 + 6))
				nat_p16(l4 + 6, 0xFFFF);
		}
	} else {
		adj16(l4 + 2, old_port, port);	/* ICMP: no pseudo-header */
	}
	nat_p32(q + a, ip);
	nat_p16(l4 + po, port);
}

bool nat_out(uint8_t *q, uint16_t len, uint32_t wan_ip)
{
	uint8_t proto;
	uint16_t sp, dp;
	const uint8_t *l4c;
	uint32_t now = nat_now_s(), lan_ip, rem_ip;
	nat_ent_t *e = 0;
	int i;

	if (!tab || !ports(q, len, false, &proto, &sp, &dp, &l4c))
		return false;
	lan_ip = nat_g32(q + 12);
	rem_ip = nat_g32(q + 16);
	if (proto == 1)
		dp = 0;
	for (i = 0; i < tab_n; i++) {
		nat_ent_t *t = &tab[i];
		if (t->expires > now && t->proto == proto && t->lan_ip == lan_ip &&
				t->lan_port == sp && t->rem_ip == rem_ip && t->rem_port == dp) {
			e = t;
			break;
		}
	}
	if (!e) {
		uint16_t wp;
		e = alloc_ent(now);
		wp = e ? new_port(proto, now) : 0;
		if (!e || !wp) {
			nat_full_drops++;
			return false;
		}
		e->proto = proto;
		e->state = S_NEW;
		e->lan_ip = lan_ip;
		e->lan_port = sp;
		e->rem_ip = rem_ip;
		e->rem_port = dp;
		e->wan_port = wp;
	}
	touch(e, l4c, false, now);
	rewrite(q, q + (l4c - q), proto, 12, 0, wan_ip, e->wan_port);
	nat_out_count++;
	return true;
}

bool nat_in(const uint8_t *p, uint16_t len, uint8_t *q, uint32_t *lan_ip)
{
	uint8_t proto;
	uint16_t sp, dp;
	const uint8_t *l4c;
	uint32_t now = nat_now_s(), rem_ip;
	nat_ent_t *e = 0;
	int i;

	if (!tab || !ports(p, len, true, &proto, &sp, &dp, &l4c))
		return false;
	rem_ip = nat_g32(p + 12);
	if (proto == 1)
		sp = 0;
	for (i = 0; i < tab_n; i++) {
		nat_ent_t *t = &tab[i];
		if (t->expires > now && t->proto == proto && t->wan_port == dp &&
				t->rem_ip == rem_ip && t->rem_port == sp) {
			e = t;
			break;
		}
	}
	if (!e) {
		/* a port forward: the connection starts here */
		for (i = 0; i < nfwd; i++)
			if (fwds[i].proto == proto && fwds[i].wan_port == dp && proto != 1)
				break;
		if (i == nfwd)
			return false;
		e = alloc_ent(now);
		if (!e) {
			nat_full_drops++;
			return false;
		}
		e->proto = proto;
		e->state = S_NEW;
		e->lan_ip = fwds[i].lan_ip;
		e->lan_port = fwds[i].lan_port;
		e->rem_ip = rem_ip;
		e->rem_port = sp;
		e->wan_port = dp;
		touch(e, l4c, false, now);	/* opened from outside: the first side */
	} else {
		touch(e, l4c, true, now);
	}
	memmove(q, p, len);
	rewrite(q, q + (l4c - p), proto, 16, 2, e->lan_ip, e->lan_port);
	*lan_ip = e->lan_ip;
	nat_in_count++;
	return true;
}
