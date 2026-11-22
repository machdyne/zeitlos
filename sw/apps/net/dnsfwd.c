/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx's DNS forwarder (docs/netx.md, dnsfwd.h): what a host on an
 * interface with a DHCP server asks the board on port 53 goes to the
 * board's own nameserver (the one DHCP gave the wan side, or net.cfg's
 * dns =), and the answer comes back to it. Built with -DNETX only.
 *
 * Each query goes out with an ID of its own, from DNSFWD_PORT, and up to
 * DNSFWD_PENDING wait at once; one not answered in 5 seconds is given
 * up (the host asks again). With no nameserver the host is told
 * SERVFAIL at once. No cache: the upstream has one.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "dnsfwd.h"
#include "netif.h"
#include "udp.h"

typedef struct {
	uint16_t id, client_id, client_port;
	uint32_t client_ip;
	uint32_t t;			/* ticks when sent; 0: free */
} pend_t;

static pend_t pend[DNSFWD_PENDING];
static uint32_t (*upstream)(void);
static uint16_t next_id = 0x5a17;
uint32_t dnsfwd_queries, dnsfwd_answers, dnsfwd_failed;

static uint8_t buf[UDP_MAX_PAYLOAD];

static void servfail(uint32_t ip, uint16_t port, const uint8_t *q, uint16_t len)
{
	if (len < 12) return;
	memcpy(buf, q, len);
	buf[2] = (uint8_t)(0x80 | (q[2] & 0x79));	/* a response, opcode and RD kept */
	buf[3] = 0x82;					/* RA, SERVFAIL */
	dnsfwd_failed++;
	udp_send(ip, port, 53, buf, len);
}

static void on_query(uint32_t src_ip, uint16_t src_port, const uint8_t *d, uint16_t len)
{
	uint32_t now = z_uptime_ticks(), ns;
	pend_t *p = 0;
	int i;

	if (!netif_cur->dhcpd || len < 12 || len > sizeof(buf) || (d[2] & 0x80))
		return;			/* only where it serves; and only questions */
	dnsfwd_queries++;
	ns = upstream ? upstream() : 0;
	if (!ns || netif_is_ours(ns)) {
		servfail(src_ip, src_port, d, len);
		return;
	}
	for (i = 0; i < DNSFWD_PENDING; i++)
		if (!pend[i].t || now - pend[i].t > DNSFWD_TIMEOUT) {
			p = &pend[i];
			break;
		}
	if (!p) {
		servfail(src_ip, src_port, d, len);
		return;
	}
	p->id = next_id++;
	p->client_id = (uint16_t)(d[0] << 8 | d[1]);
	p->client_ip = src_ip;
	p->client_port = src_port;
	p->t = now ? now : 1;
	memcpy(buf, d, len);
	buf[0] = (uint8_t)(p->id >> 8);
	buf[1] = (uint8_t)p->id;
	udp_send(ns, 53, DNSFWD_PORT, buf, len);
}

static void on_answer(uint32_t src_ip, uint16_t src_port, const uint8_t *d, uint16_t len)
{
	uint16_t id;
	int i;

	if (len < 12 || len > sizeof(buf) || src_port != 53 || !upstream || src_ip != upstream())
		return;
	id = (uint16_t)(d[0] << 8 | d[1]);
	for (i = 0; i < DNSFWD_PENDING; i++) {
		pend_t *p = &pend[i];
		if (!p->t || p->id != id)
			continue;
		memcpy(buf, d, len);
		buf[0] = (uint8_t)(p->client_id >> 8);
		buf[1] = (uint8_t)p->client_id;
		p->t = 0;
		dnsfwd_answers++;
		udp_send(p->client_ip, p->client_port, 53, buf, len);
		return;
	}
}

void dnsfwd_init(uint32_t (*nameserver)(void))
{
	memset(pend, 0, sizeof(pend));
	upstream = nameserver;
	udp_listen(53, on_query);
	udp_listen(DNSFWD_PORT, on_answer);
}
