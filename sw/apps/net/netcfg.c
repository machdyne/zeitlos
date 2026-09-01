/*
 * Zeitlos -- NET.CFG parser. See netcfg.h and docs/esp32-net.md.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "../../common/zfsapp.h"
#include "netcfg.h"

static int parse_ipv4(const char *s, uint32_t *out)
{
	unsigned a, b, c, d;
	char extra;
	if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4)
		return -1;
	if (a > 255 || b > 255 || c > 255 || d > 255)
		return -1;
	*out = (a << 24) | (b << 16) | (c << 8) | d;
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

	char *buf = fs_mallocfile("NET.CFG");
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
				strcpy(out->ssid, val);
				out->has_wifi = (out->ssid[0] != 0);
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
				strcpy(out->psk, val);
			} else if (!strcmp(p, "dhcp")) {
				out->dhcp = (val[0] != '0');
			} else if (!strcmp(p, "ip")) {
				if (parse_ipv4(val, &out->ip) != 0) {
					printf("netcfg: bad ip\n");
					free(buf);
					return -1;
				}
			} else if (!strcmp(p, "mask") || !strcmp(p, "netmask")) {
				if (parse_ipv4(val, &out->mask) != 0) {
					printf("netcfg: bad mask\n");
					free(buf);
					return -1;
				}
			} else if (!strcmp(p, "gw") || !strcmp(p, "gateway")) {
				if (parse_ipv4(val, &out->gw) != 0) {
					printf("netcfg: bad gw\n");
					free(buf);
					return -1;
				}
			} else if (!strcmp(p, "dns")) {
				if (parse_ipv4(val, &out->dns) != 0) {
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
