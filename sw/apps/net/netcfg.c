/*
 * Zeitlos -- net.cfg parser. See netcfg.h and docs/esp32link.md.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "../../common/zpaths.h"
#include "../../common/zfsapp.h"
#include "netcfg.h"

// A dotted-quad IPv4 address, "192.168.1.10", into host order. 0 on
// success, -1 if it is not one.
//
// By hand rather than sscanf("%u.%u.%u.%u%c"), which is what this was:
// that one call linked sscanf's engine, strtod, the multi-precision
// helpers, soft double arithmetic and 28KB of wide-character locale
// tables into net -- about 74KB of a core app, for four numbers
// (docs/networking.md, "Why net does not call sscanf"). It accepts what
// that accepted from a config file: whitespace before each number,
// leading zeros of any length ("010" is ten, as %u reads it), and
// nothing after the last number. tests/test_netcfg.c checks it.
int netcfg_parse_ipv4(const char *s, uint32_t *out)
{
	uint32_t v = 0;

	for (int part = 0; part < 4; part++) {
		uint32_t n = 0;
		int digits = 0;
		while (*s == ' ' || *s == '\t') s++;
		while (*s >= '0' && *s <= '9') {
			n = n * 10 + (uint32_t)(*s++ - '0');
			if (n > 255) return -1;		// also stops overflow
			digits++;
		}
		if (!digits) return -1;
		v = (v << 8) | n;
		if (part < 3 && *s++ != '.') return -1;
	}
	if (*s) return -1;

	*out = v;
	return 0;
}

static void rtrim(char *s)
{
	int n = (int)strlen(s);
	while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
			s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = 0;
}

static char *ltrim(char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

static int ascii_printable(const char *s)
{
	if (!s || !*s)
		return 0;
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c < 0x20 || c > 0x7e)
			return 0;
	}
	return 1;
}

#ifdef NETX
/* "a.b.c.d/len" or "a.b.c.d": the address, and the mask when a length
 * is given (else *mask untouched) */
static int parse_cidr(char *v, uint32_t *ip, uint32_t *mask)
{
	char *sl = strchr(v, '/');
	if (sl) {
		int n = atoi(sl + 1);
		*sl = 0;
		if (n < 0 || n > 32) return -1;
		*mask = n ? (0xFFFFFFFFu << (32 - n)) : 0;
	}
	return netcfg_parse_ipv4(v, ip);
}

static netcfg_if_t *if_named(netcfg_t *out, const char *name)
{
	int i;
	for (i = 0; i < out->n_if; i++)
		if (!strcmp(out->ifs[i].name, name))
			return &out->ifs[i];
	if (out->n_if >= NETCFG_IF_MAX)
		return 0;
	netcfg_if_t *f = &out->ifs[out->n_if++];
	memset(f, 0, sizeof(*f));
	strcpy(f->name, name);
	f->phy = -1;
	f->dhcp = -1;
	return f;
}

/* netx's keys (docs/netx.md): forward, route, and <role>.<key> for the
 * roles wan, lan and lan2. 1 taken, 0 not ours, -1 an error. */
static int netx_key(netcfg_t *out, char *key, char *val)
{
	if (!strcmp(key, "forward")) {
		out->forward = (val[0] != '0');
		return 1;
	}
	if (!strcmp(key, "nat_size")) {
		out->nat_size = atoi(val);
		if (out->nat_size < 16 || out->nat_size > 8192) {
			printf("netcfg: nat_size is 16 to 8192\n");
			return -1;
		}
		return 1;
	}
	if (!strcmp(key, "dnsfwd")) {
		out->dnsfwd = (val[0] != '0');
		return 1;
	}
	if (!strcmp(key, "portfwd")) {
		/* portfwd = tcp|udp PORT a.b.c.d:PORT */
		char *sp = strchr(val, ' '), *colon;
		int wp, lp;
		uint32_t ip;
		if (out->n_portfwd >= NETCFG_PORTFWD_MAX || !sp) {
			printf("netcfg: portfwd = tcp|udp port address:port (at most %d)\n",
				NETCFG_PORTFWD_MAX);
			return -1;
		}
		*sp = 0;
		char *rest = ltrim(sp + 1);
		char *sp2 = strchr(rest, ' ');
		if (!sp2 || (strcmp(val, "tcp") && strcmp(val, "udp"))) {
			printf("netcfg: portfwd = tcp|udp port address:port\n");
			return -1;
		}
		*sp2 = 0;
		wp = atoi(rest);
		rest = ltrim(sp2 + 1);
		colon = strchr(rest, ':');
		if (!colon) {
			printf("netcfg: portfwd: address:port\n");
			return -1;
		}
		*colon = 0;
		lp = atoi(colon + 1);
		if (wp <= 0 || wp > 65535 || lp <= 0 || lp > 65535 || netcfg_parse_ipv4(rest, &ip)) {
			printf("netcfg: bad portfwd\n");
			return -1;
		}
		out->portfwd[out->n_portfwd].proto = !strcmp(val, "tcp") ? 6 : 17;
		out->portfwd[out->n_portfwd].wan_port = (uint16_t)wp;
		out->portfwd[out->n_portfwd].lan_port = (uint16_t)lp;
		out->portfwd[out->n_portfwd].lan_ip = ip;
		out->n_portfwd++;
		return 1;
	}
	if (!strcmp(key, "route")) {
		/* route = net/len via gw */
		char *via = strstr(val, " via ");
		uint32_t net = 0, mask = 0xFFFFFFFFu, gw = 0;
		if (!via || out->n_routes >= NETCFG_ROUTES_MAX) {
			printf("netcfg: route = net/len via gateway (at most %d)\n",
				NETCFG_ROUTES_MAX);
			return -1;
		}
		*via = 0;
		rtrim(val);
		if (parse_cidr(val, &net, &mask) || netcfg_parse_ipv4(ltrim(via + 5), &gw)) {
			printf("netcfg: bad route\n");
			return -1;
		}
		out->routes[out->n_routes].net = net & mask;
		out->routes[out->n_routes].mask = mask;
		out->routes[out->n_routes].via = gw;
		out->n_routes++;
		return 1;
	}
	char *dot = strchr(key, '.');
	if (!dot)
		return 0;
	*dot = 0;
	const char *k = dot + 1;
	if (strcmp(key, "wan") && strcmp(key, "lan") && strcmp(key, "lan2")) {
		printf("netcfg: '%s.' -- the interfaces are wan., lan. and lan2.\n", key);
		return -1;
	}
	netcfg_if_t *f = if_named(out, key);
	if (!f)
		return -1;
	if (!strcmp(k, "phy")) {
		if (!strcmp(val, "builtin")) f->phy = 2;
		else if (!strcmp(val, "usb")) f->phy = 1;
		else if (!strcmp(val, "usbdev")) f->phy = 3;
		else if (!strcmp(val, "builtin2")) f->phy = 4;
		else if (!strcmp(val, "xlink")) f->phy = 5;
		else {
			printf("netcfg: %s.phy must be builtin, builtin2, usb, usbdev or xlink\n", key);
			return -1;
		}
	} else if (!strcmp(k, "dhcp")) {
		f->dhcp = (val[0] != '0');
	} else if (!strcmp(k, "ip")) {
		if (parse_cidr(val, &f->ip, &f->mask)) {
			printf("netcfg: bad %s.ip\n", key);
			return -1;
		}
	} else if (!strcmp(k, "mask") || !strcmp(k, "netmask")) {
		if (netcfg_parse_ipv4(val, &f->mask)) {
			printf("netcfg: bad %s.mask\n", key);
			return -1;
		}
	} else if (!strcmp(k, "gw") || !strcmp(k, "gateway")) {
		if (netcfg_parse_ipv4(val, &f->gw)) {
			printf("netcfg: bad %s.gw\n", key);
			return -1;
		}
	} else if (!strcmp(k, "allow")) {
		if (strlen(val) >= sizeof(f->allow)) {
			printf("netcfg: %s.allow too long\n", key);
			return -1;
		}
		strcpy(f->allow, val);
	} else if (!strcmp(k, "nat")) {
		f->nat = (val[0] != '0');
	} else if (!strcmp(k, "dhcpd")) {
		f->dhcpd = (val[0] != '0');
	} else if (!strcmp(k, "lease")) {
		f->lease = (uint32_t)atoi(val);
		if (f->lease < 60) {
			printf("netcfg: %s.lease is at least 60 (seconds)\n", key);
			return -1;
		}
	} else if (!strcmp(k, "pool")) {
		/* a.b.c.d-a.b.c.e */
		char *dash = strchr(val, '-');
		if (!dash) {
			printf("netcfg: %s.pool = first-last\n", key);
			return -1;
		}
		*dash = 0;
		rtrim(val);
		if (netcfg_parse_ipv4(val, &f->pool_lo) ||
				netcfg_parse_ipv4(ltrim(dash + 1), &f->pool_hi) ||
				f->pool_hi < f->pool_lo) {
			printf("netcfg: bad %s.pool\n", key);
			return -1;
		}
	}
	/* other keys of an interface: for later phases, ignored */
	return 1;
}
#endif

int netcfg_load(netcfg_t *out)
{
	memset(out, 0, sizeof(*out));
	out->dhcp = -1;
	out->full = 1;
#ifdef NETX
	out->dnsfwd = -1;
#endif

	char *buf = fs_mallocfile(Z_PATH_NET_CONFIG);
	if (!buf)
		buf = fs_mallocfile(Z_PATH_NET_CONFIG);
	if (!buf)
		return 0;

	out->has_file = 1;

	char *line = buf;
	/* UTF-8 BOM from macOS TextEdit */
	if ((unsigned char)line[0] == 0xEF &&
			(unsigned char)line[1] == 0xBB &&
			(unsigned char)line[2] == 0xBF)
		line += 3;

	while (line && *line) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = 0;
		rtrim(line);
		char *p = ltrim(line);
		if (*p && *p != '#') {
			char *eq = strchr(p, '=');
			if (!eq) {
				printf("netcfg: skip '%s' (no =)\n", p);
				line = nl ? (nl + 1) : 0;
				continue;
			}
			*eq = 0;
			rtrim(p);
			char *val = ltrim(eq + 1);
			rtrim(val);

			if (!strcmp(p, "ssid")) {
				if (strlen(val) > NETCFG_SSID_MAX) {
					printf("netcfg: ssid too long\n");
					free(buf);
					return -1;
				}
				if (!ascii_printable(val)) {
					printf("netcfg: ssid not printable ASCII\n");
					free(buf);
					return -1;
				}
				if (val[0] && out->n_wifi < NETCFG_WIFI_MAX) {
					strcpy(out->wifi[out->n_wifi].ssid, val);
					out->n_wifi++;
				} else if (val[0]) {
					printf("netcfg: more than %d networks, "
						"'%s' ignored\n", NETCFG_WIFI_MAX, val);
				}
				if (out->n_wifi == 1) {
					strcpy(out->ssid, val);	/* entry 0 mirror */
					out->has_wifi = 1;
				}
			} else if (!strcmp(p, "psk") || !strcmp(p, "pass") ||
					!strcmp(p, "password")) {
				if (strlen(val) > NETCFG_PSK_MAX) {
					printf("netcfg: psk too long\n");
					free(buf);
					return -1;
				}
				if (val[0] && !ascii_printable(val)) {
					printf("netcfg: psk not printable ASCII\n");
					free(buf);
					return -1;
				}
				if (out->n_wifi)
					strcpy(out->wifi[out->n_wifi - 1].psk, val);
				if (out->n_wifi <= 1)
					strcpy(out->psk, val);	/* entry 0 mirror */
			} else if (!strcmp(p, "phy")) {
				/* auto (default), usb, builtin. An unknown
				 * value is a typo, and silently falling back
				 * to auto on a board where that means "no
				 * network at all" is not helpful. */
				if (!strcmp(val, "usb")) out->phy = 1;
				else if (!strcmp(val, "builtin")) out->phy = 2;
				else if (!strcmp(val, "auto")) out->phy = 0;
				else if (!strcmp(val, "usbdev")) out->phy = 3;
				else {
					printf("netcfg: phy must be auto, usb, "
						"usbdev or builtin\n");
					free(buf);
					return -1;
				}
			} else if (!strcmp(p, "nfs")) {
				if (strlen(val) > NETCFG_NFS_MAX) {
					printf("netcfg: nfs= too long\n");
					free(buf);
					return -1;
				}
				strcpy(out->nfs, val);
			} else if (!strcmp(p, "dhcp")) {
				out->dhcp = (val[0] != '0');
			} else if (!strcmp(p, "full")) {
				out->full = (val[0] != '0');
			} else if (!strcmp(p, "ip")) {
				if (netcfg_parse_ipv4(val, &out->ip) != 0) {
					printf("netcfg: bad ip\n");
					free(buf);
					return -1;
				}
			} else if (!strcmp(p, "mask") || !strcmp(p, "netmask")) {
				if (netcfg_parse_ipv4(val, &out->mask) != 0) {
					printf("netcfg: bad mask\n");
					free(buf);
					return -1;
				}
			} else if (!strcmp(p, "gw") || !strcmp(p, "gateway")) {
				if (netcfg_parse_ipv4(val, &out->gw) != 0) {
					printf("netcfg: bad gw\n");
					free(buf);
					return -1;
				}
			} else if (!strcmp(p, "dns")) {
				if (netcfg_parse_ipv4(val, &out->dns) != 0) {
					printf("netcfg: bad dns\n");
					free(buf);
					return -1;
				}
			}
#ifdef NETX
			else if (netx_key(out, p, val) < 0) {
				free(buf);
				return -1;
			}
#endif
			/* unknown keys ignored */
		}
		line = nl ? (nl + 1) : 0;
	}

	free(buf);
	if (out->ssid[0])
		out->has_wifi = 1;
	return 0;
}
