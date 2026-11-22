#ifndef NETX_H
#define NETX_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netx, the full net (docs/netx.md): its start. Built with -DNETX only.
 */

#include <stdint.h>
#include "netcfg.h"

// Every interface net.cfg names, or the board has, started and given
// its address (netx.c). 0 with at least one up, -1 with none. *dns gets
// the first nameserver a DHCP server gave, if it is still 0.
int netx_bringup(const netcfg_t *cfg, const uint8_t base_mac[6], uint32_t *dns);

// The DHCP server and the DNS forwarder (netx.c), once the nameserver
// is set.
void netx_services(const netcfg_t *cfg);

// The main loop's wait (netx.c): 0 when no interface polls.
uint32_t netx_idle_ticks(void);

#endif
