#ifndef ZERDESK_DESKCFG_H
#define ZERDESK_DESKCFG_H

/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The decisions zerdesk makes before it touches a socket: which port,
 * how many viewers, who may connect, and which connection id to hand
 * net. Pure, so a host can check them.
 *
 * The connection id only grows, and it skips 0. net finds a relay by
 * (listener, id) among every relay that is still around, including one
 * that is closing. Reusing an id while that relay lives delivers the
 * new connection's acks to the old one, and the new connection stalls.
 */

#include <stdint.h>
#include <string.h>

#define ZD_PORT_DEFAULT     8080
#define ZD_VIEWERS_DEFAULT  6
#define ZD_VIEWERS_MAX      6

static inline int zd_clamp_port(int p)
{
	if (p < 1 || p > 65535)
		return ZD_PORT_DEFAULT;
	return p;
}

static inline int zd_clamp_viewers(int n)
{
	if (n < 1 || n > ZD_VIEWERS_MAX)
		return ZD_VIEWERS_DEFAULT;
	return n;
}

/* "any" is the only word that opens the desktop past this subnet.
 * Anything else, including a missing value, stays on the subnet. */
static inline int zd_allow_any(const char *s)
{
	return s && !strcmp(s, "any");
}

/* Take the TCP connection. Otherwise refuse it before it has a slot. */
static inline int zd_take_peer(int allow_any, int on_subnet)
{
	return allow_any || on_subnet;
}

/* A browser sends Origin on the WebSocket upgrade. A tool does not,
 * and a page cannot omit the header, so a missing value is taken.
 * When the header is present it must be http:// or https://, in either
 * case, followed by the Host header and nothing else. null, an empty
 * value, a path or a different host is refused. Lengths, because the
 * header value is not terminated. */
static inline int zd_origin_ok(const char *origin, int olen,
		const char *host, int hlen)
{
	int sn = 0;
	int i;

	if (!origin)
		return 1;
	if (olen < 1 || !host || hlen < 1)
		return 0;
	if (olen >= 8) {
		sn = 8;
		for (i = 0; i < 8; i++) {
			char a = origin[i];
			if (a >= 'A' && a <= 'Z')
				a = (char)(a + 32);
			if (a != "https://"[i]) {
				sn = 0;
				break;
			}
		}
	}
	if (!sn && olen >= 7) {
		sn = 7;
		for (i = 0; i < 7; i++) {
			char a = origin[i];
			if (a >= 'A' && a <= 'Z')
				a = (char)(a + 32);
			if (a != "http://"[i]) {
				sn = 0;
				break;
			}
		}
	}
	if (!sn)
		return 0;
	return olen - sn == hlen &&
		memcmp(origin + sn, host, (size_t)hlen) == 0;
}

static inline int zd_viewers_full(int connected, int limit)
{
	return connected >= limit;
}

static inline uint32_t zd_next_conn(uint32_t prev)
{
	uint32_t n = prev + 1;
	return n ? n : 1;
}

#endif
