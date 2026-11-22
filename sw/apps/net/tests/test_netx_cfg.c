/*
 * Host test for netcfg.c's netx keys (-DNETX, docs/netx.md): wan., lan.
 * and lan2., forward, route; full for both builds; and errors.
 *
 *   make test
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../netcfg.h"

static const char *file;
char *fs_mallocfile(char *filename)
{
	(void)filename;
	if (!file) return NULL;
	char *b = malloc(strlen(file) + 1);
	strcpy(b, file);
	return b;
}

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)
#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))

int main(void)
{
	netcfg_t c;

	file = NULL;
	CK(netcfg_load(&c) == 0 && c.full == 1 && c.n_if == 0, "no file: full, no interfaces");

	file = "# a router\n"
		"full = 1\n"
		"wan.phy = builtin\n"
		"wan.dhcp = 1\n"
		"lan.phy = usbdev\n"
		"lan.ip = 192.168.7.1/24\n"
		"lan2.ip = 10.1.0.1\n"
		"lan2.mask = 255.255.0.0\n"
		"lan2.gw = 10.1.0.254\n"
		"forward = 1\n"
		"route = 10.9.0.0/16 via 10.1.0.9\n"
		"route = 172.16.5.0/24 via 192.168.7.20\n"
		"ip = 192.168.178.50\n";
	CK(netcfg_load(&c) == 0, "parses");
	CK(c.n_if == 3, "three interfaces (%d)", c.n_if);
	CK(!strcmp(c.ifs[0].name, "wan") && c.ifs[0].phy == 2 && c.ifs[0].dhcp == 1, "wan: builtin, dhcp");
	CK(!strcmp(c.ifs[1].name, "lan") && c.ifs[1].phy == 3 && c.ifs[1].ip == IP(192, 168, 7, 1) &&
		c.ifs[1].mask == 0xFFFFFF00u && c.ifs[1].dhcp == -1, "lan: usbdev, 192.168.7.1/24");
	CK(c.ifs[2].ip == IP(10, 1, 0, 1) && c.ifs[2].mask == 0xFFFF0000u && c.ifs[2].gw == IP(10, 1, 0, 254) &&
		c.ifs[2].phy == -1, "lan2: ip, mask, gw");
	CK(c.forward == 1, "forward");
	CK(c.n_routes == 2 && c.routes[0].net == IP(10, 9, 0, 0) && c.routes[0].mask == 0xFFFF0000u &&
		c.routes[0].via == IP(10, 1, 0, 9), "route 1");
	CK(c.routes[1].net == IP(172, 16, 5, 0) && c.routes[1].mask == 0xFFFFFF00u &&
		c.routes[1].via == IP(192, 168, 7, 20), "route 2");
	CK(c.ip == IP(192, 168, 178, 50), "plain keys still read");

	file = "full = 0\n";
	CK(netcfg_load(&c) == 0 && c.full == 0, "full = 0");
	file = "lan.ip = 10.0.0.1/0\n";
	CK(netcfg_load(&c) == 0 && c.ifs[0].mask == 0, "a /0");
	file = "route = 10.0.0.0/8 via 10.0.0.1\nroute = 10.0.0.0/8 via 10.0.0.1\n";
	CK(netcfg_load(&c) == 0 && c.routes[0].net == IP(10, 0, 0, 0) && c.routes[0].mask == 0xFF000000u, "a /8");

	/* the gateway's keys */
	file = "wan.nat = 1\nlan.dhcpd = 1\nlan.pool = 192.168.7.50-192.168.7.60\nlan.lease = 600\n"
		"portfwd = tcp 2222 192.168.7.2:22\nportfwd = udp 5000 192.168.7.3:5001\n"
		"nat_size = 1024\ndnsfwd = 0\n";
	CK(netcfg_load(&c) == 0, "parses");
	CK(c.ifs[0].nat == 1 && c.ifs[1].dhcpd == 1, "nat, dhcpd");
	CK(c.ifs[1].pool_lo == IP(192, 168, 7, 50) && c.ifs[1].pool_hi == IP(192, 168, 7, 60) &&
		c.ifs[1].lease == 600, "pool, lease");
	CK(c.n_portfwd == 2 && c.portfwd[0].proto == 6 && c.portfwd[0].wan_port == 2222 &&
		c.portfwd[0].lan_ip == IP(192, 168, 7, 2) && c.portfwd[0].lan_port == 22, "portfwd tcp");
	CK(c.portfwd[1].proto == 17 && c.portfwd[1].lan_port == 5001, "portfwd udp");
	CK(c.nat_size == 1024 && c.dnsfwd == 0, "nat_size, dnsfwd");
	file = "lan.dhcpd = 1\n";
	CK(netcfg_load(&c) == 0 && c.dnsfwd == -1, "dnsfwd unset by default");
	file = "portfwd = icmp 1 1.2.3.4:1\n";
	CK(netcfg_load(&c) == -1, "portfwd: tcp or udp");
	file = "portfwd = tcp 70000 1.2.3.4:1\n";
	CK(netcfg_load(&c) == -1, "portfwd: a port too large");
	file = "lan.pool = 192.168.7.60-192.168.7.50\n";
	CK(netcfg_load(&c) == -1, "a pool backwards");
	file = "nat_size = 4\n";
	CK(netcfg_load(&c) == -1, "nat_size too small");

	file = "wan.allow = ssh, udp/5000, 8080\n";
	CK(netcfg_load(&c) == 0 && !strcmp(c.ifs[0].allow, "ssh, udp/5000, 8080"), "wan.allow");

	/* errors */
	file = "dmz.ip = 10.0.0.1\n";
	CK(netcfg_load(&c) == -1, "an unknown interface is an error");
	file = "lan.phy = wifi\n";
	CK(netcfg_load(&c) == -1, "an unknown phy is an error");
	file = "lan.ip = 10.0.0.1/33\n";
	CK(netcfg_load(&c) == -1, "a /33 is an error");
	file = "route = 10.0.0.0/8\n";
	CK(netcfg_load(&c) == -1, "a route without via is an error");
	file = "route = 10.0.0.0/8 via nowhere\n";
	CK(netcfg_load(&c) == -1, "a route via nothing is an error");
	file = "wan.dhcp = 1\nlan.dhcp = 1\nlan2.dhcp = 1\n";
	CK(netcfg_load(&c) == 0 && c.n_if == 3, "three is the most");
	file = "lan.dhcp = 0\nlan.ip = 10.0.0.1/24\n";
	CK(netcfg_load(&c) == 0 && c.n_if == 1 && c.ifs[0].dhcp == 0, "the same interface twice is one");

	printf("netx cfg: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
