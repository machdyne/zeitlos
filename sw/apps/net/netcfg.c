/*
 * Zeitlos -- net.cfg parser. See netcfg.h and docs/esp32link.md.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

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

int netcfg_load(netcfg_t *out)
{
	memset(out, 0, sizeof(*out));
	out->dhcp = -1;

	char *buf = fs_mallocfile("net.cfg");
	if (!buf)
		buf = fs_mallocfile("net.cfg");
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
				else {
					printf("netcfg: phy must be auto, usb "
						"or builtin\n");
					free(buf);
					return -1;
				}
			} else if (!strcmp(p, "dhcp")) {
				out->dhcp = (val[0] != '0');
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
			/* unknown keys ignored */
		}
		line = nl ? (nl + 1) : 0;
	}

	free(buf);
	if (out->ssid[0])
		out->has_wifi = 1;
	return 0;
}
