/*
 * Host test for netsettings' net.cfg model (cfgfile.c): edits keep the
 * file's comments, order and the keys it does not touch, and what it
 * writes means to netx's own parser (sw/apps/net/netcfg.c, -DNETX)
 * what netsettings meant.
 *
 *   make test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../cfgfile.h"
#include "../../net/netcfg.h"

static const char *file;
char *fs_mallocfile(char *filename)
{
	(void)filename;
	char *b = malloc(strlen(file) + 1);
	strcpy(b, file);
	return b;
}

static int checks, fails;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)
#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (d))

static cf_t c;
static char out[CF_LINES * CF_LINE];

int main(void)
{
	netcfg_t n;
	const char *orig =
		"# net.cfg -- hand-written\n"
		"\n"
		"dhcp = 1   # the usual\n"
		"ssid = home\n"
		"route = 10.9.0.0/16 via 192.168.7.20\n"
		"route = 10.8.0.0/16 via 192.168.7.21\n"
		"lan.ip = 192.168.7.1/24\n"
		"lan.ip = 192.168.9.1/24\n"
		"# the end\n";

	CK(cf_load(&c, orig) == 0 && c.n == 9, "nine lines (%d)", c.n);
	CK(!strcmp(cf_get(&c, "dhcp"), "1"), "a value, without its comment");
	CK(!strcmp(cf_get(&c, "lan.ip"), "192.168.9.1/24"), "the last of two");
	CK(cf_get(&c, "gw") == 0, "absent");
	CK(cf_has_prefix(&c, "lan.") && !cf_has_prefix(&c, "wan."), "prefixes");

	cf_set(&c, "lan.ip", "192.168.7.1/24");
	cf_set(&c, "lan.dhcpd", "1");
	cf_set(&c, "wan.phy", "builtin");
	cf_set(&c, "wan.dhcp", "1");
	cf_set(&c, "wan.nat", "1");
	cf_set(&c, "wan.allow", "ssh, udp/5000");
	cf_unset(&c, "dhcp");
	CK(cf_save(&c, out, sizeof(out)) > 0, "saves");
	CK(!strncmp(out, "# net.cfg -- hand-written\n\nssid = home\nroute = 10.9.0.0/16 via 192.168.7.20\n"
		"route = 10.8.0.0/16 via 192.168.7.21\nlan.ip = 192.168.7.1/24\n# the end\n", 132),
		"comments, order and the routes kept; the second lan.ip gone; dhcp taken out:\n%s", out);
	CK(strstr(out, "wan.phy = builtin\n") && strstr(out, "lan.dhcpd = 1\n"), "new keys at the end");

	/* netx's parser reads what was meant */
	file = out;
	CK(netcfg_load(&n) == 0, "netx parses it");
	CK(n.n_if == 2, "two interfaces (%d)", n.n_if);
	CK(!strcmp(n.ifs[0].name, "lan") && n.ifs[0].ip == IP(192, 168, 7, 1) && n.ifs[0].dhcpd == 1, "lan");
	CK(!strcmp(n.ifs[1].name, "wan") && n.ifs[1].phy == 2 && n.ifs[1].dhcp == 1 && n.ifs[1].nat == 1 &&
		!strcmp(n.ifs[1].allow, "ssh, udp/5000"), "wan");
	CK(n.n_routes == 2 && n.dhcp == -1, "the routes kept, dhcp gone");

	/* an interface taken away entirely */
	cf_unset_prefix(&c, "wan.");
	cf_save(&c, out, sizeof(out));
	file = out;
	CK(netcfg_load(&n) == 0 && n.n_if == 1, "wan. gone: one interface");

	/* the checks netsettings makes before writing */
	CK(cf_valid_ip("192.168.7.1", 0) && cf_valid_ip("192.168.7.1/24", 1), "good addresses");
	CK(!cf_valid_ip("192.168.7.1/24", 0) && !cf_valid_ip("192.168.7", 1) && !cf_valid_ip("256.1.1.1", 1) &&
		!cf_valid_ip("1.2.3.4/33", 1) && !cf_valid_ip("1.2.3.4/", 1) && !cf_valid_ip("a.b.c.d", 1) &&
		!cf_valid_ip("", 1) && !cf_valid_ip("1.2.3.4x", 1), "bad ones");

	/* limits */
	{
		static char big[CF_LINES * 4 + 8];
		int i;
		for (i = 0; i < CF_LINES + 1; i++) strcat(big, "#\n");
		CK(cf_load(&c, big) == -1, "too many lines: not edited");
		char longl[CF_LINE + 8];
		memset(longl, 'x', CF_LINE + 2); longl[CF_LINE + 2] = 0;
		CK(cf_load(&c, longl) == -1, "a line too long: not edited");
	}
	CK(cf_load(&c, "\xEF\xBB\xBF" "dhcp = 0\r\n") == 0 && !strcmp(cf_get(&c, "dhcp"), "0"), "a BOM and CRLF");

	printf("cfgfile: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
