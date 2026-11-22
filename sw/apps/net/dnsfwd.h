#ifndef DNSFWD_H
#define DNSFWD_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx's DNS forwarder (dnsfwd.c, docs/netx.md). Built with -DNETX only.
 */

#include <stdint.h>

#define DNSFWD_PORT	40053		/* its queries' source port: clear of 49152+ */
#define DNSFWD_PENDING	32
#define DNSFWD_TIMEOUT	(5 * Z_TICK_HZ)

// Answer port 53 on every interface with a DHCP server, by asking
// nameserver() (the board's own: dns.c).
void dnsfwd_init(uint32_t (*nameserver)(void));

extern uint32_t dnsfwd_queries, dnsfwd_answers, dnsfwd_failed;

#endif
