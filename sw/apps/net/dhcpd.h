#ifndef DHCPD_H
#define DHCPD_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx's DHCP server (dhcpd.c, docs/netx.md). Built with -DNETX only.
 */

#include <stdint.h>

#define DHCPD_LEASES	32

// Serve every interface with dhcpd set (netif.h), once they have their
// addresses. lan_dns: the nameserver handed out, 0 for the interface's
// own address (the DNS forwarder).
void dhcpd_init(uint32_t lan_dns);

// Bound leases, for the status page and the tests.
int dhcpd_leases(void);
extern uint32_t dhcpd_offers, dhcpd_acks, dhcpd_naks;

#endif
