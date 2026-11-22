/*
 * Host test for netx's firewall (-DNETX, fw.c), on the gateway test's
 * wire (test_netx_gw.c, whose harness this is): the defaults on a WAN
 * interface, rules from firewall.cfg both ways, and a board with no
 * WAN, which filters nothing.
 *
 *   make test   (or: cc -std=gnu99 -Wall -DNETX -I. -I../../common \
 *                  -o /tmp/t tests/test_netx_gw.c && /tmp/t)
 *
 * The router: lan 192.168.7.1/24 on PHY A (DHCP server, DNS forwarder),
 * wan 10.0.2.1/24 on PHY B with NAT and gateway 10.0.2.9; upstream DNS
 * at 10.0.2.53. Hosts on wire A: .20 and .50; the internet is behind
 * the gateway on wire B. Every frame on both wires is checked: the
 * addresses, the ports, and every checksum, recomputed.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static int tcp_seen;
void tcp_handle(uint32_t src, const uint8_t *p, uint16_t len) { (void)src; (void)p; (void)len; tcp_seen++; }
static uint32_t ticks = 1000;
uint32_t z_uptime_ticks(void) { return ticks; }
static uint32_t upstream_ns;
uint32_t dns_nameserver(void) { return upstream_ns; }

#include "../netif.h"
#include "../net_phy.h"
const net_phy_t *net_phy;
char *fs_mallocfile(char *filename) { (void)filename; return 0; }

#define QMAX 32
typedef struct { uint8_t f[1600]; uint16_t len; } frame_t;
typedef struct { frame_t q[QMAX]; int n, r; } wire_t;
static wire_t to_router[2], from_router[2];
static void put(wire_t *w, const uint8_t *f, uint16_t len)
{ if (w->r == w->n) w->r = w->n = 0; if (w->n < QMAX) { memcpy(w->q[w->n].f, f, len); w->q[w->n].len = len; w->n++; } }
static frame_t *take(wire_t *w) { return w->r < w->n ? &w->q[w->r++] : NULL; }
static void clear(wire_t *w) { w->n = w->r = 0; }
static uint16_t recv_a(uint8_t *b, uint16_t m) { frame_t *f = take(&to_router[0]); (void)m; if (!f) return 0; memcpy(b, f->f, f->len); return f->len; }
static uint16_t recv_b(uint8_t *b, uint16_t m) { frame_t *f = take(&to_router[1]); (void)m; if (!f) return 0; memcpy(b, f->f, f->len); return f->len; }
static bool send_a(const uint8_t *b, uint16_t len) { put(&from_router[0], b, len); return true; }
static bool send_b(const uint8_t *b, uint16_t len) { put(&from_router[1], b, len); return true; }
static bool init_x(const uint8_t m[6]) { (void)m; return true; }
static void dump_x(void) {}
static const net_phy_t phy_a = { "a", init_x, recv_a, send_a, dump_x, 0, 4 * 1460 };
static const net_phy_t phy_b = { "b", init_x, recv_b, send_b, dump_x, 0, 4 * 1460 };

#include "../netif.c"
#include "../eth.c"
#include "../arp.c"
#include "../ip.c"
#include "../udp.c"
#include "../nat.c"
#include "../fw.c"
#include "../netcfg.c"
#include "../dhcpd.c"
#include "../dnsfwd.c"

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))
static const uint8_t mac_h20[6] = { 0x02, 0xAA, 0, 0, 0, 0x20 };
static const uint8_t mac_h50[6] = { 0x02, 0xAA, 0, 0, 0, 0x50 };
static const uint8_t mac_g[6] = { 0x02, 0xCC, 0, 0, 0, 0x09 };
static const uint8_t mac_ns[6] = { 0x02, 0xCC, 0, 0, 0, 0x53 };
static const uint8_t r0[6] = { 0x02, 0, 0, 0, 0, 0x01 };
static const uint8_t r1[6] = { 0x02, 0, 0, 0, 1, 0x01 };
static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
#define LAN IP(192, 168, 7, 1)
#define WAN IP(10, 0, 2, 1)
#define H20 IP(192, 168, 7, 20)
#define H50 IP(192, 168, 7, 50)
#define REM IP(203, 0, 113, 5)

static uint16_t tg16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static uint32_t sum(const uint8_t *d, int n, uint32_t s)
{
	int i;
	for (i = 0; i + 1 < n; i += 2) s += (uint32_t)d[i] << 8 | d[i + 1];
	if (n & 1) s += (uint32_t)d[n - 1] << 8;
	return s;
}
static uint16_t fold(uint32_t s) { while (s >> 16) s = (s & 0xFFFF) + (s >> 16); return (uint16_t)~s; }
static uint16_t csum(const uint8_t *d, int n) { return fold(sum(d, n, 0)); }

/* the L4 checksum of the IP packet at ip, with its pseudo-header */
static uint16_t l4sum(const uint8_t *ip)
{
	int tot = tg16(ip + 2), n = tot - 20;
	uint32_t s = sum(ip + 12, 8, 0) + ip[9] + n;
	return fold(sum(ip + 20, n, s));
}

static uint16_t ipv4(uint8_t *f, const uint8_t *dmac, const uint8_t *smac, uint32_t src, uint32_t dst,
		uint8_t proto, const uint8_t *pl, int n)
{
	uint8_t *ip = f + 14;
	memcpy(f, dmac, 6); memcpy(f + 6, smac, 6); f[12] = 8; f[13] = 0;
	memset(ip, 0, 20);
	ip[0] = 0x45; put16(ip + 2, 20 + n); ip[6] = 0x40; ip[8] = 64; ip[9] = proto;
	put32(ip + 12, src); put32(ip + 16, dst);
	put16(ip + 10, csum(ip, 20));
	memcpy(ip + 20, pl, n);
	return (uint16_t)(34 + n);
}

/* a TCP segment with a correct checksum */
static uint16_t tcp(uint8_t *f, const uint8_t *dmac, const uint8_t *smac, uint32_t src, uint16_t sp,
		uint32_t dst, uint16_t dp, uint8_t flags)
{
	uint8_t t[24];
	memset(t, 0, sizeof(t));
	put16(t, sp); put16(t + 2, dp); put32(t + 4, 1000); t[12] = 0x50; t[13] = flags; put16(t + 14, 8192);
	t[20] = 'h'; t[21] = 'i'; t[22] = '!'; t[23] = '\n';
	uint16_t n = ipv4(f, dmac, smac, src, dst, 6, t, 24);
	put16(f + 14 + 20 + 16, l4sum(f + 14));
	return n;
}

static uint16_t udp(uint8_t *f, const uint8_t *dmac, const uint8_t *smac, uint32_t src, uint16_t sp,
		uint32_t dst, uint16_t dp, const uint8_t *pl, int n, bool with_csum)
{
	uint8_t u[600];
	memset(u, 0, 8);
	put16(u, sp); put16(u + 2, dp); put16(u + 4, 8 + n);
	memcpy(u + 8, pl, n);
	uint16_t len = ipv4(f, dmac, smac, src, dst, 17, u, 8 + n);
	if (with_csum) { uint16_t c = l4sum(f + 14); put16(f + 14 + 20 + 6, c ? c : 0xFFFF); }
	return len;
}

static uint16_t mk_echo(uint8_t *f, const uint8_t *dmac, const uint8_t *smac, uint32_t src, uint32_t dst,
		uint8_t type, uint16_t id)
{
	uint8_t e[12];
	memset(e, 0, sizeof(e));
	e[0] = type; put16(e + 4, id); e[7] = 1; e[8] = 'z';
	put16(e + 2, csum(e, 12));
	return ipv4(f, dmac, smac, src, dst, 1, e, 12);
}

static uint16_t arp_reply(uint8_t *f, const uint8_t *dmac, const uint8_t *smac, uint32_t sip, uint32_t tip)
{
	uint8_t *p = f + 14;
	memcpy(f, dmac, 6); memcpy(f + 6, smac, 6); f[12] = 8; f[13] = 6;
	p[0] = 0; p[1] = 1; p[2] = 8; p[3] = 0; p[4] = 6; p[5] = 4; p[6] = 0; p[7] = 2;
	memcpy(p + 8, smac, 6); put32(p + 14, sip); memcpy(p + 18, dmac, 6); put32(p + 24, tip);
	return 42;
}

static uint8_t fb[1600];
static void run(void) { eth_poll(); ip_poll(); }

/* a frame's IP packet is sound: header checksum, and its L4 one */
static bool sound(const frame_t *f)
{
	const uint8_t *ip = f->f + 14;
	if (csum(ip, 20)) return false;
	if (ip[9] == 6) return l4sum(ip) == 0;
	if (ip[9] == 17) return tg16(ip + 26) == 0 || l4sum(ip) == 0;
	if (ip[9] == 1) return csum(ip + 20, tg16(ip + 2) - 20) == 0;
	return true;
}

/* -- DHCP client messages -- */
static uint16_t dhcp_msg(uint8_t *f, const uint8_t *mac, uint8_t type, uint32_t want, uint32_t server, uint32_t ciaddr)
{
	uint8_t b[300];
	int i = 236;
	memset(b, 0, sizeof(b));
	b[0] = 1; b[1] = 1; b[2] = 6; put32(b + 4, 0xC0FFEE00u | mac[5]);
	put32(b + 12, ciaddr);
	memcpy(b + 28, mac, 6);
	put32(b + i, 0x63825363u); i += 4;
	b[i++] = 53; b[i++] = 1; b[i++] = type;
	if (want) { b[i++] = 50; b[i++] = 4; put32(b + i, want); i += 4; }
	if (server) { b[i++] = 54; b[i++] = 4; put32(b + i, server); i += 4; }
	b[i++] = 255;
	return udp(f, bcast, mac, ciaddr, 68, 0xFFFFFFFFu, 67, b, 300, false);
}

/* option code in a DHCP reply frame */
static const uint8_t *ropt(const frame_t *f, uint8_t code)
{
	const uint8_t *o = f->f + 14 + 28 + 240;
	int n = f->len - (14 + 28 + 240), i = 0;
	while (i < n && o[i] != 255) {
		if (o[i] == 0) { i++; continue; }
		if (o[i] == code) return o + i + 2;
		i += 2 + o[i + 1];
	}
	return 0;
}


static int udp_got;
static void on_udp(uint32_t ip, uint16_t port, const uint8_t *d, uint16_t len) { (void)ip; (void)port; (void)d; (void)len; udp_got++; }

int main(void)
{
	frame_t *f;
	char rules[512];

	netif_n = 2;
	netif[0].phy = &phy_a; memcpy(netif[0].mac, r0, 6); strcpy(netif[0].name, "lan");
	netif[0].ip = LAN; netif[0].mask = 0xFFFFFF00u; netif[0].up = true;
	netif[1].phy = &phy_b; memcpy(netif[1].mac, r1, 6); strcpy(netif[1].name, "wan");
	netif[1].ip = WAN; netif[1].mask = 0xFFFFFF00u; netif[1].gw = IP(10, 0, 2, 9); netif[1].up = true;
	netif[1].nat = true;
	netif_forward = true;
	netif_set(0);
	{ uint32_t k = netif[0].ip; arp_init(0); netif[0].ip = k; }
	nat_init(0);
	fw_allow(1, "ssh");
	fw_allow(1, "udp/5000");
	udp_listen(50000, on_udp);
	udp_listen(5000, on_udp);

	put(&to_router[0], fb, arp_reply(fb, r0, mac_h20, H20, LAN)); run();
	put(&to_router[1], fb, arp_reply(fb, r1, mac_g, IP(10, 0, 2, 9), WAN)); run();
	clear(&from_router[0]); clear(&from_router[1]);

	printf("== netx: the firewall ==\n");

	/* -- the defaults, from the WAN, to the board -- */
	tcp_seen = 0;
	put(&to_router[1], fb, tcp(fb, r1, mac_g, REM, 40000, WAN, 22, 0x02)); run();
	CK(tcp_seen == 1, "a SYN to ssh (wan.allow): in");
	put(&to_router[1], fb, tcp(fb, r1, mac_g, REM, 40000, WAN, 23, 0x02)); run();
	CK(tcp_seen == 1 && fw_dropped_in == 1, "a SYN to telnet: dropped");
	put(&to_router[1], fb, tcp(fb, r1, mac_g, REM, 40000, WAN, 23, 0x10)); run();
	CK(tcp_seen == 2, "not a SYN: the stack's to answer");
	put(&to_router[1], fb, tcp(fb, r1, mac_g, REM, 40000, WAN, 23, 0x12)); run();
	CK(tcp_seen == 3, "a SYN-ACK (an answer to the board's own): in");
	put(&to_router[1], fb, mk_echo(fb, r1, mac_g, REM, WAN, 8, 7)); run();
	f = take(&from_router[1]);
	CK(f && f->f[34] == 0 && get32(f->f + 30) == REM, "ping: answered");
	{
		uint8_t pl[3] = { 1, 2, 3 };
		udp_got = 0;
		put(&to_router[1], fb, udp(fb, r1, mac_g, REM, 7, WAN, 50000, pl, 3, false)); run();
		CK(udp_got == 0, "UDP that answers nothing: dropped");
		netif_set(1);
		udp_send(REM, 7, 50000, pl, 3);
		take(&from_router[1]);
		put(&to_router[1], fb, udp(fb, r1, mac_g, REM, 7, WAN, 50000, pl, 3, false)); run();
		CK(udp_got == 1, "once the board has sent to it: its answer comes in");
		put(&to_router[1], fb, udp(fb, r1, mac_g, IP(192, 0, 2, 66), 7, WAN, 50000, pl, 3, false)); run();
		CK(udp_got == 1, "but not from someone else");
		put(&to_router[1], fb, udp(fb, r1, mac_g, IP(192, 0, 2, 66), 7, WAN, 5000, pl, 3, false)); run();
		CK(udp_got == 2, "a UDP service (wan.allow udp/5000): in");
		ticks += 121 * Z_TICK_HZ;
		put(&to_router[1], fb, udp(fb, r1, mac_g, REM, 7, WAN, 50000, pl, 3, false)); run();
		CK(udp_got == 2, "two minutes later the flow is forgotten");
	}

	/* -- the defaults, through -- */
	clear(&from_router[0]); clear(&from_router[1]);
	put(&to_router[1], fb, mk_echo(fb, r1, mac_g, REM, H20, 8, 8)); run();
	CK(!take(&from_router[0]) && fw_dropped_fwd == 1, "straight to a LAN address from the WAN: dropped");
	put(&to_router[0], fb, tcp(fb, r0, mac_h20, H20, 5555, REM, 80, 0x02)); run();
	f = take(&from_router[1]);
	CK(f && get32(f->f + 26) == WAN, "the LAN out to the WAN: through, by NAT");
	if (f) {
		uint16_t wp = tg16(f->f + 34);
		put(&to_router[1], fb, tcp(fb, r1, mac_g, REM, 80, WAN, wp, 0x12)); run();
		CK(take(&from_router[0]) != 0, "and its answer back");
	}

	/* -- the LAN, to the board: no defaults there -- */
	tcp_seen = 0;
	put(&to_router[0], fb, tcp(fb, r0, mac_h20, H20, 6000, LAN, 23, 0x02)); run();
	CK(tcp_seen == 1, "a SYN to telnet from the LAN: in");

	/* -- rules -- */
	strcpy(rules, "# test rules\n"
		"deny in=lan dst=203.0.113.0/24\n"
		"allow in=wan proto=tcp dport=23 src=198.51.100.0/24\n"
		"deny in=lan to=board proto=tcp dport=22-23\n"
		"deny in=lan2 proto=udp   # no such interface here\n"
		"\n");
	CK(fw_parse(rules) == 0 && fw_nrules() == 4, "four rules (%d)", fw_nrules());
	clear(&from_router[1]);
	put(&to_router[0], fb, tcp(fb, r0, mac_h20, H20, 5600, REM, 80, 0x02)); run();
	CK(!take(&from_router[1]) && fw_rule(0)->hits == 1, "rule 1: the LAN to 203.0.113.0/24, denied");
	put(&to_router[0], fb, tcp(fb, r0, mac_h20, H20, 5601, IP(198, 51, 100, 9), 80, 0x02)); run();
	CK(take(&from_router[1]) != 0, "elsewhere: through");
	tcp_seen = 0;
	put(&to_router[1], fb, tcp(fb, r1, mac_g, IP(198, 51, 100, 7), 40000, WAN, 23, 0x02)); run();
	CK(tcp_seen == 1 && fw_rule(1)->hits == 1, "rule 2: telnet from 198.51.100.0/24, allowed");
	put(&to_router[1], fb, tcp(fb, r1, mac_g, IP(192, 0, 2, 1), 40000, WAN, 23, 0x02)); run();
	CK(tcp_seen == 1, "from elsewhere: the default (dropped)");
	put(&to_router[0], fb, tcp(fb, r0, mac_h20, H20, 6001, LAN, 22, 0x02)); run();
	CK(tcp_seen == 1 && fw_rule(2)->hits == 1, "rule 3: ssh from the LAN to the board, denied");
	put(&to_router[0], fb, tcp(fb, r0, mac_h20, H20, 6002, LAN, 80, 0x02)); run();
	CK(tcp_seen == 2, "port 80 from the LAN: in");
	CK(fw_rule(3)->hits == 0, "a rule for an interface that is not here never matches");

	/* -- wrong rules -- */
	strcpy(rules, "permit in=wan\n");
	CK(fw_parse(rules) == -1 && fw_nrules() == 0, "permit: wrong");
	strcpy(rules, "allow to=board out=wan\n");
	CK(fw_parse(rules) == -1, "to=board with out=: wrong");
	strcpy(rules, "deny dport=70000\n");
	CK(fw_parse(rules) == -1, "a port past 65535: wrong");
	strcpy(rules, "deny src=10.0.0.0/40\n");
	CK(fw_parse(rules) == -1, "a /40: wrong");
	strcpy(rules, "deny colour=blue\n");
	CK(fw_parse(rules) == -1, "an unknown key: wrong");
	CK(!fw_allow(1, "gopher") && !fw_allow(1, "udp/0"), "services that are not");

	/* -- a board with no WAN filters nothing -- */
	fw_parse(strcpy(rules, ""));
	strcpy(netif[1].name, "lan2");
	netif[1].nat = false;
	tcp_seen = 0;
	put(&to_router[1], fb, tcp(fb, r1, mac_g, REM, 40000, IP(10, 0, 2, 1), 23, 0x02)); run();
	CK(tcp_seen == 1, "no WAN: a SYN to telnet is in");

	printf("netx fw: %d checks, %d failed\n", checks, fails);
	if (!fails) printf("PASS\n");
	return fails != 0;
}
