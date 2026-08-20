#ifndef IP_H
#define IP_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * IPv4: header construct/parse, routing (local subnet vs gateway),
 * dispatch by protocol number. ICMP echo (ping) reply is handled
 * directly in here rather than a separate icmp.c, since it's a
 * handful of lines -- split it out if ICMP grows beyond echo.
 */

#include <stdint.h>
#include <stdbool.h>

void ip_init(uint32_t our_ip, uint32_t netmask, uint32_t gateway_ip);

// our own IP, as set by ip_init() -- needed by tcp.c for the TCP
// checksum's pseudo-header (which covers src/dst IP, unlike UDP's
// optional checksum -- see udp.h). ip.c already keeps this in a
// static; this is just an accessor for it.
uint32_t ip_our_addr(void);

// dispatched from eth_poll() for ETHERTYPE_IPV4 frames.
void ip_handle(const uint8_t src_mac[6], const uint8_t *payload, uint16_t len);

// builds and sends one IP packet. protocol is the IP protocol number
// (1=ICMP, 17=UDP, not yet used here). returns false if the next
// hop's MAC isn't ARP-resolved yet -- this also kicks off resolution
// (see arp.h), so the caller should just retry after giving eth_poll()
// a few more iterations to run, rather than treating false as a hard
// failure.
bool ip_send(uint32_t dst_ip, uint8_t protocol, const uint8_t *payload, uint16_t len);

#endif
