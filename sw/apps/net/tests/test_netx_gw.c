/*
 * Host test for netx as a gateway (-DNETX): NAT (nat.c), the DHCP
 * server (dhcpd.c) and the DNS forwarder (dnsfwd.c), with the real
 * netif.c, eth.c, arp.c, ip.c and udp.c, on a two-interface "wire".
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

int main(void)
{
	frame_t *f;
	uint16_t wp;

	netif_n = 2;
	netif[0].phy = &phy_a; memcpy(netif[0].mac, r0, 6); strcpy(netif[0].name, "lan");
	netif[0].ip = LAN; netif[0].mask = 0xFFFFFF00u; netif[0].up = true; netif[0].dhcpd = true;
	netif[1].phy = &phy_b; memcpy(netif[1].mac, r1, 6); strcpy(netif[1].name, "wan");
	netif[1].ip = WAN; netif[1].mask = 0xFFFFFF00u; netif[1].gw = IP(10, 0, 2, 9); netif[1].up = true;
	netif[1].nat = true;
	netif_forward = true;
	netif_set(0);
	{ uint32_t k = netif[0].ip; arp_init(0); netif[0].ip = k; }
	nat_init(0);
	nat_add_forward(6, 2222, H50, 22);
	upstream_ns = IP(10, 0, 2, 53);
	dnsfwd_init(dns_nameserver);
	dhcpd_init(0);

	/* everyone's MAC known to the router, so that no first packet is lost */
	put(&to_router[0], fb, arp_reply(fb, r0, mac_h20, H20, LAN)); run();
	put(&to_router[0], fb, arp_reply(fb, r0, mac_h50, H50, LAN)); run();
	put(&to_router[1], fb, arp_reply(fb, r1, mac_g, IP(10, 0, 2, 9), WAN)); run();
	put(&to_router[1], fb, arp_reply(fb, r1, mac_ns, IP(10, 0, 2, 53), WAN)); run();
	clear(&from_router[0]); clear(&from_router[1]);

	printf("== netx as a gateway: NAT, DHCP server, DNS forwarder ==\n");

	/* -- NAT, TCP: out from the wan address and a port of its own -- */
	put(&to_router[0], fb, tcp(fb, r0, mac_h20, H20, 5555, REM, 80, 0x02));
	run();
	f = take(&from_router[1]);
	CK(f && !memcmp(f->f, mac_g, 6), "the SYN on wire B, to the gateway");
	wp = 0;
	if (f) {
		const uint8_t *ip = f->f + 14;
		wp = tg16(ip + 20);
		CK(get32(ip + 12) == WAN && get32(ip + 16) == REM, "from the wan address, to the server");
		CK(wp >= NAT_PORT_LO && wp <= NAT_PORT_HI, "from a NAT port (%u)", wp);
		CK(tg16(ip + 22) == 80 && ip[8] == 63, "to port 80, TTL 63");
		CK(sound(f), "IP and TCP checksums good");
	}
	/* the answer comes back to the host */
	put(&to_router[1], fb, tcp(fb, r1, mac_g, REM, 80, WAN, wp, 0x12));
	run();
	f = take(&from_router[0]);
	CK(f && !memcmp(f->f, mac_h20, 6), "the SYN-ACK on wire A, to host .20");
	if (f) {
		const uint8_t *ip = f->f + 14;
		CK(get32(ip + 12) == REM && get32(ip + 16) == H20 && tg16(ip + 22) == 5555, "to .20:5555");
		CK(sound(f), "checksums good");
	}
	CK(tcp_seen == 0, "not the board's own");
	/* the same connection keeps its port; another gets another */
	put(&to_router[0], fb, tcp(fb, r0, mac_h20, H20, 5555, REM, 80, 0x10));
	run();
	f = take(&from_router[1]);
	CK(f && tg16(f->f + 34) == wp, "the same connection, the same port");
	put(&to_router[0], fb, tcp(fb, r0, mac_h20, H20, 5556, REM, 80, 0x02));
	run();
	f = take(&from_router[1]);
	CK(f && tg16(f->f + 34) != wp && tg16(f->f + 34) >= NAT_PORT_LO, "another connection, another port");
	CK(nat_used() == 2, "two connections (%d)", nat_used());

	/* something to the wan address that is not a connection: the board's
	 * (not a SYN: the firewall's defaults leave that to the TCP stack) */
	put(&to_router[1], fb, tcp(fb, r1, mac_g, REM, 80, WAN, 23456, 0x10));
	run();
	CK(tcp_seen == 1 && !take(&from_router[0]), "an unknown port: the board's own");

	/* -- UDP: no checksum stays none; one is kept right -- */
	{
		uint8_t pl[5] = { 1, 2, 3, 4, 5 };
		put(&to_router[0], fb, udp(fb, r0, mac_h20, H20, 7000, REM, 9999, pl, 5, false));
		run();
		f = take(&from_router[1]);
		CK(f && get32(f->f + 26) == WAN && tg16(f->f + 40) == 0 && sound(f), "UDP without a checksum: still none");
		put(&to_router[0], fb, udp(fb, r0, mac_h20, H20, 7001, REM, 9999, pl, 5, true));
		run();
		f = take(&from_router[1]);
		CK(f && get32(f->f + 26) == WAN && tg16(f->f + 40) != 0 && sound(f), "UDP with one: right after");
		if (f) {
			uint16_t up = tg16(f->f + 34);
			put(&to_router[1], fb, udp(fb, r1, mac_g, REM, 9999, WAN, up, pl, 5, true));
			run();
			f = take(&from_router[0]);
			CK(f && get32(f->f + 30) == H20 && tg16(f->f + 36) == 7001 && sound(f), "and its answer back");
		}
	}

	/* -- ICMP echo: the id is translated -- */
	put(&to_router[0], fb, mk_echo(fb, r0, mac_h20, H20, REM, 8, 0x4242));
	run();
	f = take(&from_router[1]);
	CK(f && get32(f->f + 26) == WAN && sound(f), "an echo request out, from the wan address");
	if (f) {
		uint16_t id = tg16(f->f + 38);
		put(&to_router[1], fb, mk_echo(fb, r1, mac_g, REM, WAN, 0, id));
		run();
		f = take(&from_router[0]);
		CK(f && get32(f->f + 30) == H20 && tg16(f->f + 38) == 0x4242 && sound(f), "its reply back, with the host's id");
	}

	/* -- a port forward -- */
	put(&to_router[1], fb, tcp(fb, r1, mac_g, IP(198, 51, 100, 7), 40000, WAN, 2222, 0x02));
	run();
	f = take(&from_router[0]);
	CK(f && !memcmp(f->f, mac_h50, 6) && get32(f->f + 30) == H50 && tg16(f->f + 36) == 22 && sound(f),
		"wan:2222 goes to .50:22");
	put(&to_router[0], fb, tcp(fb, r0, mac_h50, H50, 22, IP(198, 51, 100, 7), 40000, 0x12));
	run();
	f = take(&from_router[1]);
	CK(f && get32(f->f + 26) == WAN && tg16(f->f + 34) == 2222 && sound(f), "and its answer leaves from :2222");

	/* -- an RST ends a connection in 10 s -- */
	put(&to_router[0], fb, tcp(fb, r0, mac_h20, H20, 5555, REM, 80, 0x04));
	run();
	take(&from_router[1]);
	ticks += 11 * Z_TICK_HZ;
	tcp_seen = 0;
	put(&to_router[1], fb, tcp(fb, r1, mac_g, REM, 80, WAN, wp, 0x10));
	run();
	CK(!take(&from_router[0]) && tcp_seen == 1, "after an RST and 11 s: the port is no one's");

	/* -- a full table drops -- */
	nat_init(16);
	{
		int k;
		clear(&from_router[1]);
		for (k = 0; k < 17; k++) {
			put(&to_router[0], fb, tcp(fb, r0, mac_h20, H20, (uint16_t)(6000 + k), REM, 80, 0x02));
			run();
		}
		CK(from_router[1].n == 16 && nat_full_drops == 1, "16 fit, the 17th is dropped (%d, %lu)",
			from_router[1].n, (unsigned long)nat_full_drops);
		ticks += 61 * Z_TICK_HZ;		/* half-open: 60 s */
		put(&to_router[0], fb, tcp(fb, r0, mac_h20, H20, 7777, REM, 80, 0x02));
		run();
		CK(from_router[1].n == 17, "after they time out, room again");
	}

	/* -- the DHCP server -- */
	{
		static const uint8_t mc[6] = { 0x02, 0xDD, 0, 0, 0, 0x01 };
		static const uint8_t md[6] = { 0x02, 0xDD, 0, 0, 0, 0x02 };
		static const uint8_t me[6] = { 0x02, 0xDD, 0, 0, 0, 0x03 };
		const uint8_t *o;
		clear(&from_router[0]); clear(&from_router[1]);
		put(&to_router[0], fb, dhcp_msg(fb, mc, 1, 0, 0, 0));
		run();
		f = take(&from_router[0]);
		CK(f && !memcmp(f->f, bcast, 6) && tg16(f->f + 34) == 67 && tg16(f->f + 36) == 68, "an OFFER, broadcast, 67 -> 68");
		if (f) {
			CK(get32(f->f + 42 + 16) == IP(192, 168, 7, 100), "of .100");
			CK((o = ropt(f, 53)) && o[0] == 2, "type OFFER");
			CK((o = ropt(f, 54)) && get32(o) == LAN, "server id: the lan address");
			CK((o = ropt(f, 1)) && get32(o) == 0xFFFFFF00u, "mask");
			CK((o = ropt(f, 3)) && get32(o) == LAN, "router: us");
			CK((o = ropt(f, 6)) && get32(o) == LAN, "DNS: us (the forwarder)");
			CK((o = ropt(f, 51)) && get32(o) == 3600, "lease 3600");
		}
		put(&to_router[0], fb, dhcp_msg(fb, mc, 3, IP(192, 168, 7, 100), LAN, 0));
		run();
		f = take(&from_router[0]);
		CK(f && (o = ropt(f, 53)) && o[0] == 5 && get32(f->f + 58) == IP(192, 168, 7, 100), "REQUEST: ACK .100");
		CK(dhcpd_leases() == 1, "one lease");
		put(&to_router[0], fb, dhcp_msg(fb, md, 1, 0, 0, 0));
		run();
		f = take(&from_router[0]);
		CK(f && get32(f->f + 58) == IP(192, 168, 7, 101), "another client: .101");
		put(&to_router[0], fb, dhcp_msg(fb, md, 3, IP(192, 168, 7, 100), LAN, 0));
		run();
		f = take(&from_router[0]);
		CK(f && (o = ropt(f, 53)) && o[0] == 6, "asking for someone else's: NAK");
		put(&to_router[0], fb, dhcp_msg(fb, mc, 1, 0, 0, 0));
		run();
		f = take(&from_router[0]);
		CK(f && get32(f->f + 58) == IP(192, 168, 7, 100), "the first comes back: .100 again");
		put(&to_router[1], fb, dhcp_msg(fb, me, 1, 0, 0, 0));
		run();
		CK(!take(&from_router[1]) && !take(&from_router[0]), "no DHCP server on the wan side");
		put(&to_router[0], fb, dhcp_msg(fb, mc, 7, 0, LAN, IP(192, 168, 7, 100)));
		run();
		CK(dhcpd_leases() == 0, "RELEASE: no lease");
		put(&to_router[0], fb, dhcp_msg(fb, me, 1, IP(192, 168, 7, 100), 0, 0));
		run();
		f = take(&from_router[0]);
		CK(f && get32(f->f + 58) == IP(192, 168, 7, 100), "a released address is free again");
		/* a pool that holds the router's own address never hands it out */
		{
			static const uint8_t mf[6] = { 0x02, 0xDD, 0, 0, 0, 0x04 };
			netif[0].pool_lo = LAN;
			netif[0].pool_hi = IP(192, 168, 7, 2);
			put(&to_router[0], fb, dhcp_msg(fb, mf, 1, LAN, 0, 0));
			run();
			f = take(&from_router[0]);
			CK(f && get32(f->f + 58) == IP(192, 168, 7, 2), "not the router's own address, even when asked");
			netif[0].pool_lo = netif[0].pool_hi = 0;
		}
	}

	/* -- the DNS forwarder -- */
	{
		uint8_t q[29] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
			7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1 };
		uint16_t id;
		clear(&from_router[0]); clear(&from_router[1]);
		put(&to_router[0], fb, udp(fb, r0, mac_h20, H20, 5353, LAN, 53, q, sizeof(q), false));
		run();
		f = take(&from_router[1]);
		CK(f && !memcmp(f->f, mac_ns, 6) && get32(f->f + 26) == WAN && get32(f->f + 30) == IP(10, 0, 2, 53) &&
			tg16(f->f + 34) == DNSFWD_PORT && tg16(f->f + 36) == 53, "the query goes to the upstream, from :40053");
		id = f ? tg16(f->f + 42) : 0;
		CK(f && id != 0x1234 && !memcmp(f->f + 44, q + 2, sizeof(q) - 2), "with an ID of its own, the rest as asked");
		{
			uint8_t a[29];
			memcpy(a, q, sizeof(a));
			put16(a, id); a[2] = 0x81; a[3] = 0x80;
			put(&to_router[1], fb, udp(fb, r1, mac_ns, IP(10, 0, 2, 53), 53, WAN, DNSFWD_PORT, a, sizeof(a), false));
			run();
			f = take(&from_router[0]);
			CK(f && get32(f->f + 26) == LAN && get32(f->f + 30) == H20 && tg16(f->f + 34) == 53 &&
				tg16(f->f + 36) == 5353 && tg16(f->f + 42) == 0x1234 && f->f[44] == 0x81,
				"the answer back to .20:5353 from :53, with its ID");
			put(&to_router[1], fb, udp(fb, r1, mac_ns, IP(10, 0, 2, 53), 53, WAN, DNSFWD_PORT, a, sizeof(a), false));
			run();
			CK(!take(&from_router[0]), "the same answer again: nobody waits for it");
		}
		upstream_ns = 0;
		put(&to_router[0], fb, udp(fb, r0, mac_h20, H20, 5353, LAN, 53, q, sizeof(q), false));
		run();
		f = take(&from_router[0]);
		CK(f && tg16(f->f + 42) == 0x1234 && (f->f[45] & 0x0F) == 2, "no nameserver: SERVFAIL");
		upstream_ns = IP(10, 0, 2, 53);
		put(&to_router[1], fb, udp(fb, r1, mac_g, REM, 9000, WAN, 53, q, sizeof(q), false));
		run();
		CK(!take(&from_router[1]), "not on the wan side");
	}

	printf("netx gw: %d checks, %d failed\n", checks, fails);
	if (!fails) printf("PASS\n");
	return fails != 0;
}
