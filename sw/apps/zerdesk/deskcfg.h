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
