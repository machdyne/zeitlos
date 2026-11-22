/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx's firewall. See fw.h; docs/netx.md, "The firewall".
 *
 * /data/net/firewall.cfg, a rule a line, first match wins:
 *
 *   allow|deny [in=IF] [out=IF] [to=board|through] [proto=tcp|udp|icmp]
 *              [src=a.b.c.d[/len]] [dst=a.b.c.d[/len]]
 *              [sport=N[-M]] [dport=N[-M]]
 *
 * IF is an interface's name (wan, lan, lan2, or the PHY's when net.cfg
 * names none). A rule with neither `to` says nothing about where the
 * packet goes: it is judged both ways.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zpaths.h"
#include "../../common/zfsapp.h"
#include "fw.h"
#include "netif.h"
#include "netcfg.h"

static fw_rule_t rules[FW_RULES];
static int nrules;
static fw_service_t svc[NETIF_MAX][FW_ALLOW];
static int nsvc[NETIF_MAX];
uint32_t fw_dropped_in, fw_dropped_fwd;

/* the board's own UDP: whose answers may come in */
#define FW_FLOWS	32
#define FW_FLOW_TICKS	(120 * Z_TICK_HZ)
static struct { uint32_t remote; uint16_t lport; uint32_t t; } flows[FW_FLOWS];

static uint16_t fw_g16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t fw_g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int fw_nrules(void) { return nrules; }
const fw_rule_t *fw_rule(int i) { return &rules[i]; }

static bool is_wan(int ifx)
{
	return ifx >= 0 && ifx < netif_n && (netif[ifx].nat || !strcmp(netif[ifx].name, "wan"));
}

static int if_by_name(const char *n)
{
	int i;
	for (i = 0; i < netif_n; i++)
		if (!strcmp(netif[i].name, n))
			return i;
	return -2;		/* named, but not here: the rule never matches */
}

bool fw_allow(int ifx, const char *w)
{
	uint8_t proto = 6;
	int port;
	if (ifx < 0 || ifx >= NETIF_MAX || nsvc[ifx] >= FW_ALLOW)
		return false;
	if (!strcmp(w, "ssh")) port = 22;
	else if (!strcmp(w, "telnet")) port = 23;
	else if (!strcmp(w, "http")) port = 80;
	else if (!strcmp(w, "https")) port = 443;
	else if (!strcmp(w, "ping")) { proto = 1; port = 0; }
	else {
		if (!strncmp(w, "udp/", 4)) { proto = 17; w += 4; }
		else if (!strncmp(w, "tcp/", 4)) w += 4;
		port = atoi(w);
		if (port <= 0 || port > 65535)
			return false;
	}
	svc[ifx][nsvc[ifx]].proto = proto;
	svc[ifx][nsvc[ifx]].port = (uint16_t)port;
	nsvc[ifx]++;
	return true;
}

static bool service(int ifx, uint8_t proto, uint16_t port)
{
	int i;
	for (i = 0; i < nsvc[ifx]; i++)
		if (svc[ifx][i].proto == proto && svc[ifx][i].port == port)
			return true;
	return false;
}

static int net_arg(char *v, uint32_t *ip, uint32_t *mask)
{
	char *sl = strchr(v, '/');
	*mask = 0xFFFFFFFFu;
	if (sl) {
		int n = atoi(sl + 1);
		*sl = 0;
		if (n < 0 || n > 32) return -1;
		*mask = n ? 0xFFFFFFFFu << (32 - n) : 0;
	}
	if (netcfg_parse_ipv4(v, ip)) return -1;
	*ip &= *mask;
	return 0;
}

static int port_arg(char *v, uint16_t *lo, uint16_t *hi)
{
	char *d = strchr(v, '-');
	int a = atoi(v), b = d ? atoi(d + 1) : a;
	if (a < 0 || a > 65535 || b < a || b > 65535) return -1;
	*lo = (uint16_t)a;
	*hi = (uint16_t)b;
	return 0;
}

/* one line into rules[nrules]: 1 a rule, 0 nothing (blank, comment), -1 wrong */
static int parse_line(char *l)
{
	fw_rule_t r;
	char *tok;
	memset(&r, 0, sizeof(r));
	r.in = r.out = -1;
	r.to_board = -1;
	r.sport_hi = r.dport_hi = 0xFFFF;
	while (*l == ' ' || *l == '\t') l++;
	if (!*l || *l == '#') return 0;
	tok = strtok(l, " \t");
	if (!strcmp(tok, "allow")) r.allow = 1;
	else if (!strcmp(tok, "deny")) r.allow = 0;
	else return -1;
	while ((tok = strtok(0, " \t")) != 0) {
		char *v = strchr(tok, '=');
		if (*tok == '#') break;
		if (!v) return -1;
		*v++ = 0;
		if (!strcmp(tok, "in")) r.in = (int8_t)if_by_name(v);
		else if (!strcmp(tok, "out")) r.out = (int8_t)if_by_name(v);
		else if (!strcmp(tok, "to")) {
			if (!strcmp(v, "board")) r.to_board = 1;
			else if (!strcmp(v, "through")) r.to_board = 0;
			else return -1;
		} else if (!strcmp(tok, "proto")) {
			if (!strcmp(v, "tcp")) r.proto = 6;
			else if (!strcmp(v, "udp")) r.proto = 17;
			else if (!strcmp(v, "icmp")) r.proto = 1;
			else return -1;
		} else if (!strcmp(tok, "src")) { if (net_arg(v, &r.src, &r.src_mask)) return -1; }
		else if (!strcmp(tok, "dst")) { if (net_arg(v, &r.dst, &r.dst_mask)) return -1; }
		else if (!strcmp(tok, "sport")) { if (port_arg(v, &r.sport_lo, &r.sport_hi)) return -1; }
		else if (!strcmp(tok, "dport")) { if (port_arg(v, &r.dport_lo, &r.dport_hi)) return -1; }
		else return -1;
	}
	if (r.out >= 0 || r.out == -2) {
		if (r.to_board == 1) return -1;		/* to the board has no out */
		r.to_board = 0;
	}
	if (nrules >= FW_RULES) return -1;
	rules[nrules++] = r;
	return 1;
}

int fw_parse(char *text)
{
	char *line = text;
	int n = 0;
	nrules = 0;
	while (line && *line) {
		char *nl = strchr(line, '\n');
		char *cr;
		if (nl) *nl = 0;
		if ((cr = strchr(line, '\r'))) *cr = 0;
		n++;
		if (parse_line(line) < 0) {
			printf("netx: firewall.cfg line %d is wrong%s\n", n,
				nrules >= FW_RULES ? " (too many rules)" : "");
			nrules = 0;
			return -1;
		}
		line = nl ? nl + 1 : 0;
	}
	return 0;
}

int fw_load(void)
{
	char *buf = fs_mallocfile(Z_PATH_NET_FIREWALL);
	int r;
	nrules = 0;
	if (!buf)
		return 0;
	r = fw_parse(buf);
	free(buf);
	if (r == 0)
		printf("netx: firewall: %d rule(s)\n", nrules);
	return r;
}

/* the parts a rule looks at */
typedef struct {
	uint8_t proto, tcp_flags, icmp_type;
	uint32_t src, dst;
	uint16_t sport, dport;
	bool ports;
} pkt_t;

static bool parse(const uint8_t *p, uint16_t len, pkt_t *k)
{
	uint8_t ihl = (p[0] & 0x0F) * 4;
	const uint8_t *l4 = p + ihl;
	uint16_t n = len > ihl ? (uint16_t)(len - ihl) : 0;
	bool first = ((p[6] & 0x1F) | p[7]) == 0;
	memset(k, 0, sizeof(*k));
	k->proto = p[9];
	k->src = fw_g32(p + 12);
	k->dst = fw_g32(p + 16);
	if (!first)
		return true;		/* a later fragment: no ports to see */
	if ((k->proto == 6 && n >= 14) || (k->proto == 17 && n >= 4)) {
		k->sport = fw_g16(l4);
		k->dport = fw_g16(l4 + 2);
		k->ports = true;
		if (k->proto == 6) k->tcp_flags = l4[13];
	} else if (k->proto == 1 && n >= 1) {
		k->icmp_type = l4[0];
	}
	return true;
}

/* the first rule that matches: its index, or -1 */
static int match(const pkt_t *k, int in, int out, bool to_board)
{
	int i;
	for (i = 0; i < nrules; i++) {
		fw_rule_t *r = &rules[i];
		if (r->to_board == 1 && !to_board) continue;
		if (r->to_board == 0 && to_board) continue;
		if (r->in != -1 && r->in != in) continue;
		if (r->out != -1 && r->out != out) continue;
		if (r->proto && r->proto != k->proto) continue;
		if ((k->src & r->src_mask) != r->src) continue;
		if ((k->dst & r->dst_mask) != r->dst) continue;
		if (r->sport_lo || r->sport_hi != 0xFFFF || r->dport_lo || r->dport_hi != 0xFFFF) {
			if (!k->ports) continue;
			if (k->sport < r->sport_lo || k->sport > r->sport_hi) continue;
			if (k->dport < r->dport_lo || k->dport > r->dport_hi) continue;
		}
		r->hits++;
		return i;
	}
	return -1;
}

static void log_drop(const pkt_t *k, int in, const char *why)
{
	static uint32_t t0, n;
	uint32_t now = z_uptime_ticks();
	/* The default rules drop a network's broadcast and multicast chatter
	   all day (DHCP for others, smart plugs announcing themselves):
	   dropped and counted, not printed. A rule's drops are always
	   printed -- somebody asked for them. */
	if (why[0] == 'd' && (k->dst == 0xFFFFFFFFu || (k->dst >> 28) == 0xE ||
			(in >= 0 && netif[in].mask && k->dst == (netif[in].ip | ~netif[in].mask))))
		return;
	if (now - t0 >= Z_TICK_HZ) { t0 = now; n = 0; }
	if (n++ >= 5) return;		/* at most five a second */
	printf("netx: fw: drop %s %lu.%lu.%lu.%lu:%u -> %lu.%lu.%lu.%lu:%u in %s (%s)\n",
		k->proto == 6 ? "tcp" : k->proto == 17 ? "udp" : k->proto == 1 ? "icmp" : "ip",
		(unsigned long)(k->src >> 24), (unsigned long)(k->src >> 16 & 0xFF),
		(unsigned long)(k->src >> 8 & 0xFF), (unsigned long)(k->src & 0xFF), k->sport,
		(unsigned long)(k->dst >> 24), (unsigned long)(k->dst >> 16 & 0xFF),
		(unsigned long)(k->dst >> 8 & 0xFF), (unsigned long)(k->dst & 0xFF), k->dport,
		in >= 0 ? netif[in].name : "?", why);
}

static bool own_flow(uint32_t remote, uint16_t lport)
{
	uint32_t now = z_uptime_ticks();
	int i;
	for (i = 0; i < FW_FLOWS; i++)
		if (flows[i].t && now - flows[i].t <= FW_FLOW_TICKS && flows[i].lport == lport &&
				(!flows[i].remote || flows[i].remote == remote))
			return true;
	return false;
}

void fw_note(const uint8_t *p)
{
	uint32_t now = z_uptime_ticks(), dst;
	uint16_t lport;
	int i, free_i = -1, old = 0;
	if (p[9] != 17)
		return;
	dst = fw_g32(p + 16);
	lport = fw_g16(p + 20);
	if (dst == 0xFFFFFFFFu || (dst >> 28) == 0xE)
		dst = 0;		/* answered by anyone: DHCP, mDNS */
	for (i = 0; i < FW_FLOWS; i++) {
		if (flows[i].t && flows[i].remote == dst && flows[i].lport == lport) {
			flows[i].t = now ? now : 1;
			return;
		}
		if (!flows[i].t || now - flows[i].t > FW_FLOW_TICKS) { if (free_i < 0) free_i = i; }
		else if (flows[i].t - flows[old].t > 0x80000000u) old = i;
	}
	i = free_i >= 0 ? free_i : old;
	flows[i].remote = dst;
	flows[i].lport = lport;
	flows[i].t = now ? now : 1;
}

bool fw_input(const uint8_t *p, uint16_t len)
{
	int in = netif_index(netif_cur), r;
	pkt_t k;
	parse(p, len, &k);
	r = match(&k, in, -1, true);
	if (r >= 0) {
		if (!rules[r].allow) { fw_dropped_in++; log_drop(&k, in, "rule"); }
		return rules[r].allow;
	}
	if (!is_wan(in))
		return true;
	switch (k.proto) {
	case 1:
		/* ping, the answers to the board's own, and the errors */
		if (k.icmp_type == 8 || k.icmp_type == 0 || k.icmp_type == 3 ||
				k.icmp_type == 11 || k.icmp_type == 12)
			return true;
		break;
	case 6:
		/* only a SYN opens a connection: to a service, or not at all */
		if (!k.ports || !(k.tcp_flags & 0x02) || (k.tcp_flags & 0x10) ||
				service(in, 6, k.dport))
			return true;
		break;
	case 17:
		if (k.ports && (own_flow(k.src, k.dport) || service(in, 17, k.dport)))
			return true;
		break;
	default:
		break;
	}
	fw_dropped_in++;
	log_drop(&k, in, "default");
	return false;
}

bool fw_forward(const uint8_t *p, uint16_t len, int in, int out)
{
	int r;
	pkt_t k;
	parse(p, len, &k);
	r = match(&k, in, out, false);
	if (r >= 0) {
		if (!rules[r].allow) { fw_dropped_fwd++; log_drop(&k, in, "rule"); }
		return rules[r].allow;
	}
	if (!is_wan(in))
		return true;
	fw_dropped_fwd++;
	log_drop(&k, in, "default");
	return false;
}
