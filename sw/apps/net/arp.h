#ifndef ARP_H
#define ARP_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * ARP: a small fixed-size IP-to-MAC cache, request/reply handling.
 * Non-blocking by design, matching the rest of this stack -- there's
 * no sleep/yield primitive to block on, so resolution is a poll loop:
 * call arp_request() once, then call arp_lookup() on your own
 * schedule (e.g. every eth_poll() iteration) until it succeeds or you
 * give up.
 */

#include <stdint.h>
#include <stdbool.h>

void arp_init(uint32_t our_ip);

// dispatched from eth_poll() for ETHERTYPE_ARP frames. handles
// requests for our own IP (replies automatically) and opportunistically
// learns the sender's IP->MAC mapping from any ARP traffic it sees,
// request or reply -- same behavior real ARP implementations use.
void arp_handle(const uint8_t src_mac[6], const uint8_t *payload, uint16_t len);

// non-blocking lookup. returns true and fills mac_out if ip is
// cached, false otherwise -- does NOT itself send a request.
bool arp_lookup(uint32_t ip, uint8_t mac_out[6]);

// sends an ARP request (broadcast) for ip. does not block; poll
// arp_lookup() afterward.
void arp_request(uint32_t ip);

#endif
