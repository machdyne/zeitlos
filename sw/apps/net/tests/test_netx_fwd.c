/*
 * Host test for netx's interfaces and forwarding (-DNETX: netif.c and
 * the NETX parts of eth.c, arp.c and ip.c), on a two-interface "wire".
 *
 *   make test   (or: cc -std=gnu99 -Wall -DNETX -I. -I../../common \
 *                  -o /tmp/t tests/test_netx_fwd.c && /tmp/t)
 *
 * The router: lan 10.0.1.1/24 on PHY A, wan 10.0.2.1/24 on PHY B,
 * forwarding on. Host A is 10.0.1.2 on wire A; host B 10.0.2.2 on wire
 * B. The test plays both hosts, frame by frame:
 *
 *   - A pings B: the router asks for B's MAC on wire B (and drops the
 *     first packet), B answers, A pings again: on wire B with the TTL one
 *     less and a good checksum, from the router's wan MAC.
 *   - B's reply comes back the same way, ARP for A first.
 *   - TTL 1: time exceeded, back to A from 10.0.1.1.
 *   - no route: network unreachable; then a static route via a gateway
 *     on wire B, and a default gateway.
 *   - A pings the router's wan address: answered from that address.
 *   - TCP to the wan address from wire A: not delivered (a strong host).
 *   - forwarding off: dropped, nothing on wire B.
 *   - a directed broadcast and multicast: not forwarded.
 *   - the board's own packet picks its interface by route.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* -- stubs for what ip.c reaches beyond this test -- */
static int tcp_seen, udp_seen;
void tcp_handle(uint32_t src, const uint8_t *p, uint16_t len) { (void)src; (void)p; (void)len; tcp_seen++; }
void udp_handle(uint32_t src, const uint8_t *p, uint16_t len) { (void)src; (void)p; (void)len; udp_seen++; }
static uint32_t ticks = 1000;
uint32_t z_uptime_ticks(void) { return ticks; }

#include "../netif.h"
#include "../net_phy.h"
const net_phy_t *net_phy;
char *fs_mallocfile(char *filename) { (void)filename; return 0; }

/* -- the two wires -- */
#define QMAX 16
typedef struct { uint8_t f[1600]; uint16_t len; } frame_t;
typedef struct { frame_t q[QMAX]; int n, r; } wire_t;
static wire_t to_router[2], from_router[2];

static void put(wire_t *w, const uint8_t *f, uint16_t len)
{
	if (w->r == w->n) w->r = w->n = 0;
	if (w->n < QMAX) { memcpy(w->q[w->n].f, f, len); w->q[w->n].len = len; w->n++; }
}
static frame_t *take(wire_t *w)
{
	return w->r < w->n ? &w->q[w->r++] : NULL;
}
static void clear(wire_t *w) { w->n = w->r = 0; }

static uint16_t recv_a(uint8_t *b, uint16_t max) { frame_t *f = take(&to_router[0]); if (!f) return 0; (void)max; memcpy(b, f->f, f->len); return f->len; }
static uint16_t recv_b(uint8_t *b, uint16_t max) { frame_t *f = take(&to_router[1]); if (!f) return 0; (void)max; memcpy(b, f->f, f->len); return f->len; }
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
#include "../nat.c"
#include "../fw.c"
#include "../netcfg.c"

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))
static const uint8_t mac_a[6] = { 0x02, 0xAA, 0, 0, 0, 0x02 };
static const uint8_t mac_b[6] = { 0x02, 0xBB, 0, 0, 0, 0x02 };
static const uint8_t mac_g[6] = { 0x02, 0xCC, 0, 0, 0, 0x09 };	/* a gateway on wire B */
static const uint8_t r_mac0[6] = { 0x02, 0, 0, 0, 0, 0x01 };
static const uint8_t r_mac1[6] = { 0x02, 0, 0, 0, 1, 0x01 };
static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static uint16_t csum(const uint8_t *d, int n)
{
	uint32_t s = 0;
	int i;
	for (i = 0; i + 1 < n; i += 2) s += (uint32_t)d[i] << 8 | d[i + 1];
	if (n & 1) s += (uint32_t)d[n - 1] << 8;
	while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
	return (uint16_t)~s;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* an IPv4 frame from a host: proto, TTL, and a payload of n bytes */
static uint16_t ipv4(uint8_t *f, const uint8_t *dmac, const uint8_t *smac, uint32_t src, uint32_t dst,
		uint8_t proto, uint8_t ttl, const uint8_t *pl, int n)
{
	uint8_t *ip = f + 14;
	memcpy(f, dmac, 6); memcpy(f + 6, smac, 6); f[12] = 0x08; f[13] = 0x00;
	memset(ip, 0, 20);
	ip[0] = 0x45; ip[2] = (20 + n) >> 8; ip[3] = (20 + n) & 0xFF;
	ip[4] = 0x12; ip[5] = 0x34; ip[6] = 0x40; ip[8] = ttl; ip[9] = proto;
	put32(ip + 12, src); put32(ip + 16, dst);
	uint16_t c = csum(ip, 20); ip[10] = c >> 8; ip[11] = c & 0xFF;
	memcpy(ip + 20, pl, n);
	return (uint16_t)(14 + 20 + n);
}

static uint16_t ping(uint8_t *f, const uint8_t *dmac, const uint8_t *smac, uint32_t src, uint32_t dst, uint8_t ttl, uint8_t seq)
{
	uint8_t e[16];
	memset(e, 0, sizeof(e));
	e[0] = 8; e[4] = 0x77; e[7] = seq;
	for (int i = 8; i < 16; i++) e[i] = (uint8_t)(i * 3);
	uint16_t c = csum(e, 16); e[2] = c >> 8; e[3] = c & 0xFF;
	return ipv4(f, dmac, smac, src, dst, 1, ttl, e, 16);
}

static uint16_t arp_reply(uint8_t *f, const uint8_t *dmac, const uint8_t *smac, uint32_t sip, uint32_t tip)
{
	uint8_t *p = f + 14;
	memcpy(f, dmac, 6); memcpy(f + 6, smac, 6); f[12] = 0x08; f[13] = 0x06;
	p[0] = 0; p[1] = 1; p[2] = 8; p[3] = 0; p[4] = 6; p[5] = 4; p[6] = 0; p[7] = 2;
	memcpy(p + 8, smac, 6); put32(p + 14, sip); memcpy(p + 18, dmac, 6); put32(p + 24, tip);
	return 14 + 28;
}

static uint8_t fb[1600];

/* the router handles what is waiting */
static void run(void) { eth_poll(); ip_poll(); }

static bool is_arp_request_for(const frame_t *f, uint32_t ip, const uint8_t *smac, uint32_t sip)
{
	return f && f->f[12] == 0x08 && f->f[13] == 0x06 && f->f[21] == 1 &&
		!memcmp(f->f, bcast, 6) && !memcmp(f->f + 6, smac, 6) &&
		get32(f->f + 14 + 24) == ip && get32(f->f + 14 + 14) == sip;
}

int main(void)
{
	frame_t *f;

	/* the router */
	netif_n = 2;
	netif[0].phy = &phy_a; memcpy(netif[0].mac, r_mac0, 6); strcpy(netif[0].name, "lan");
	netif[0].ip = IP(10, 0, 1, 1); netif[0].mask = 0xFFFFFF00u; netif[0].up = true;
	netif[1].phy = &phy_b; memcpy(netif[1].mac, r_mac1, 6); strcpy(netif[1].name, "lan2");	/* a router between two LANs: no WAN defaults */
	netif[1].ip = IP(10, 0, 2, 1); netif[1].mask = 0xFFFFFF00u; netif[1].up = true;
	netif_forward = true;
	netif_set(0);
	{ uint32_t k = netif[0].ip; arp_init(0); netif[0].ip = k; }

	printf("== netx: two interfaces, forwarding ==\n");

	/* A pings B: the router asks for B on wire B, the packet goes */
	put(&to_router[0], fb, ping(fb, r_mac0, mac_a, IP(10, 0, 1, 2), IP(10, 0, 2, 2), 64, 1));
	run();
	f = take(&from_router[1]);
	CK(is_arp_request_for(f, IP(10, 0, 2, 2), r_mac1, IP(10, 0, 2, 1)),
		"ARP request for B on wire B, from the wan MAC and address");
	CK(!take(&from_router[1]) && !take(&from_router[0]), "and nothing else");
	CK(netif[0].drop == 1, "the first packet dropped (%lu)", (unsigned long)netif[0].drop);

	put(&to_router[1], fb, arp_reply(fb, r_mac1, mac_b, IP(10, 0, 2, 2), IP(10, 0, 2, 1)));
	run();
	put(&to_router[0], fb, ping(fb, r_mac0, mac_a, IP(10, 0, 1, 2), IP(10, 0, 2, 2), 64, 2));
	run();
	f = take(&from_router[1]);
	CK(f && f->f[13] == 0x00, "a frame on wire B");
	if (f) {
		uint8_t *ip = f->f + 14;
		CK(!memcmp(f->f, mac_b, 6) && !memcmp(f->f + 6, r_mac1, 6), "to B's MAC from the wan MAC");
		CK(ip[8] == 63, "TTL 63 (%d)", ip[8]);
		CK(csum(ip, 20) == 0, "header checksum good after the TTL change");
		CK(get32(ip + 12) == IP(10, 0, 1, 2) && get32(ip + 16) == IP(10, 0, 2, 2), "addresses untouched");
		CK(ip[20] == 8 && ip[27] == 2, "the echo request itself");
	}
	CK(netif[1].fwd_out == 1 && ip_forwarded == 1, "counted");

	/* B answers: ARP for A on wire A first */
	put(&to_router[1], fb, ping(fb, r_mac1, mac_b, IP(10, 0, 2, 2), IP(10, 0, 1, 2), 64, 3));
	run();
	f = take(&from_router[0]);
	CK(is_arp_request_for(f, IP(10, 0, 1, 2), r_mac0, IP(10, 0, 1, 1)), "ARP for A on wire A, from the lan side");
	put(&to_router[0], fb, arp_reply(fb, r_mac0, mac_a, IP(10, 0, 1, 2), IP(10, 0, 1, 1)));
	put(&to_router[1], fb, ping(fb, r_mac1, mac_b, IP(10, 0, 2, 2), IP(10, 0, 1, 2), 64, 4));
	run();
	f = take(&from_router[0]);
	CK(f && !memcmp(f->f, mac_a, 6) && !memcmp(f->f + 6, r_mac0, 6) && f->f[14 + 8] == 63,
		"B's packet on wire A, to A, TTL 63");

	/* TTL 1: time exceeded, to A from the lan address */
	clear(&from_router[0]); clear(&from_router[1]);
	put(&to_router[0], fb, ping(fb, r_mac0, mac_a, IP(10, 0, 1, 2), IP(10, 0, 2, 2), 1, 5));
	run();
	f = take(&from_router[0]);
	CK(f && f->f[14 + 9] == 1 && f->f[14 + 20] == 11 && f->f[14 + 21] == 0, "ICMP time exceeded");
	if (f) {
		CK(get32(f->f + 14 + 12) == IP(10, 0, 1, 1) && get32(f->f + 14 + 16) == IP(10, 0, 1, 2),
			"from 10.0.1.1 to A");
		CK(csum(f->f + 14 + 20, f->len - 34) == 0, "ICMP checksum good");
		CK(get32(f->f + 14 + 28 + 16) == IP(10, 0, 2, 2), "it quotes the original header");
	}
	CK(!take(&from_router[1]), "nothing forwarded");

	/* no route */
	put(&to_router[0], fb, ping(fb, r_mac0, mac_a, IP(10, 0, 1, 2), IP(10, 9, 9, 9), 64, 6));
	run();
	f = take(&from_router[0]);
	CK(f && f->f[14 + 20] == 3 && f->f[14 + 21] == 0, "no route: network unreachable");

	/* a static route via a gateway on wire B */
	netif_add_route(IP(10, 9, 0, 0), 0xFFFF0000u, IP(10, 0, 2, 9));
	put(&to_router[1], fb, arp_reply(fb, r_mac1, mac_g, IP(10, 0, 2, 9), IP(10, 0, 2, 1)));
	run();
	put(&to_router[0], fb, ping(fb, r_mac0, mac_a, IP(10, 0, 1, 2), IP(10, 9, 9, 9), 64, 7));
	run();
	f = take(&from_router[1]);
	CK(f && !memcmp(f->f, mac_g, 6) && get32(f->f + 14 + 16) == IP(10, 9, 9, 9), "a static route: to the gateway's MAC");
	/* and the default route */
	netif[1].gw = IP(10, 0, 2, 9);
	put(&to_router[0], fb, ping(fb, r_mac0, mac_a, IP(10, 0, 1, 2), IP(192, 0, 2, 7), 64, 8));
	run();
	f = take(&from_router[1]);
	CK(f && !memcmp(f->f, mac_g, 6) && get32(f->f + 14 + 16) == IP(192, 0, 2, 7), "the default route");

	/* the router's own wan address, from wire A */
	clear(&from_router[0]); clear(&from_router[1]);
	put(&to_router[0], fb, ping(fb, r_mac0, mac_a, IP(10, 0, 1, 2), IP(10, 0, 2, 1), 64, 9));
	run();
	f = take(&from_router[0]);
	CK(f && f->f[14 + 20] == 0 && get32(f->f + 14 + 12) == IP(10, 0, 2, 1) && get32(f->f + 14 + 16) == IP(10, 0, 1, 2),
		"a ping to the wan address is answered on wire A, from that address");
	CK(!take(&from_router[1]), "and not forwarded");
	tcp_seen = 0;
	{
		uint8_t t[20];
		memset(t, 0, sizeof(t));
		put(&to_router[0], fb, ipv4(fb, r_mac0, mac_a, IP(10, 0, 1, 2), IP(10, 0, 2, 1), 6, 64, t, 20));
		run();
		CK(tcp_seen == 0, "TCP to the wan address from wire A: not delivered");
		put(&to_router[0], fb, ipv4(fb, r_mac0, mac_a, IP(10, 0, 1, 2), IP(10, 0, 1, 1), 6, 64, t, 20));
		run();
		CK(tcp_seen == 1, "TCP to the lan address: delivered");
	}

	/* not forwarded: forwarding off, a directed broadcast, multicast */
	clear(&from_router[1]);
	netif_forward = false;
	put(&to_router[0], fb, ping(fb, r_mac0, mac_a, IP(10, 0, 1, 2), IP(10, 0, 2, 2), 64, 10));
	run();
	CK(!take(&from_router[1]), "forwarding off: nothing on wire B");
	netif_forward = true;
	put(&to_router[0], fb, ping(fb, r_mac0, mac_a, IP(10, 0, 1, 2), IP(10, 0, 2, 255), 64, 11));
	run();
	CK(!take(&from_router[1]), "a directed broadcast: not forwarded");
	put(&to_router[0], fb, ping(fb, r_mac0, mac_a, IP(10, 0, 1, 2), IP(224, 0, 0, 251), 64, 12));
	run();
	CK(!take(&from_router[1]), "multicast: not forwarded");

	/* the board's own packet: by route, from that interface */
	clear(&from_router[0]); clear(&from_router[1]);
	netif_set(0);
	{
		uint8_t pl[4] = { 1, 2, 3, 4 };
		ip_send(IP(10, 0, 2, 2), 17, pl, 4);
		f = take(&from_router[1]);
		CK(f && !memcmp(f->f + 6, r_mac1, 6) && get32(f->f + 14 + 12) == IP(10, 0, 2, 1),
			"to B: out of the wan interface, from 10.0.2.1");
		CK(!take(&from_router[0]), "not out of the lan one");
		CK(netif_src_for(IP(10, 0, 1, 2)) == IP(10, 0, 1, 1) &&
			netif_src_for(IP(198, 51, 100, 1)) == IP(10, 0, 2, 1), "netif_src_for");
	}

	/* ARP: one cache, entries by interface */
	{
		uint8_t m[6];
		netif_set(1);
		CK(!arp_lookup(IP(10, 0, 1, 2), m), "A is not known on the wan side");
		netif_set(0);
		CK(arp_lookup(IP(10, 0, 1, 2), m) && !memcmp(m, mac_a, 6), "A is known on the lan side");
	}

	printf("%d checks, %d failed\n", checks, fails);
	if (!fails) printf("PASS\n");
	return fails != 0;
}
