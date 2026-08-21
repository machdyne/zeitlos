#ifndef DHCP_H
#define DHCP_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DHCP (RFC 2131/2132) client -- DISCOVER/OFFER/REQUEST/ACK only,
 * run once at startup to replace the fully-static IP config
 * docs/networking.md's "Config" section used to describe. See dhcp.c
 * for the full design writeup; the short version:
 *
 * - Blocking-with-timeout, like z_port_connect_arg_timeout()
 *   (sw/common/zport.c) or zstream.c's own open/pull timeouts -- NOT
 *   folded into net.c's normal non-blocking main-loop poll pattern
 *   (unlike arp.c/tftp.c/tcp.c), because nothing else meaningful can
 *   happen before net has an address anyway (ip_send() has nowhere
 *   useful to route non-broadcast traffic yet, and no other process
 *   has any reason to talk to net before it's registered/listening).
 *   dhcp_acquire() drives its own eth_poll() loop internally and only
 *   returns once it has a lease or has genuinely given up.
 * - One-shot: acquires a lease at boot and keeps it for the process's
 *   whole run. No renewal (T1/T2 timers), no lease-expiry handling.
 *   Deliberate scope cut, not an oversight -- see dhcp.c's header
 *   comment for the reasoning and what re-adding it would need.
 * - On failure (no server responds within the retry budget, or a
 *   server NAKs), returns false and does NOT touch the ip/netmask/
 *   gateway out-params -- net.c's own caller falls back to its existing static
 *   OUR_IP/OUR_NETMASK/OUR_GATEWAY constants in that case, so a board
 *   with no DHCP server on its network (a bench setup with just a
 *   switch, say) behaves exactly as it did before this file existed.
 */

#include <stdint.h>
#include <stdbool.h>

// Runs the full DISCOVER -> OFFER -> REQUEST -> ACK exchange (with
// retries/backoff -- see dhcp.c) against whatever's on the wire.
// mac[6] is our own hardware address (used both in the DHCP packets
// themselves and, via eth.c, as the Ethernet source -- caller must
// have already called eth_init() with it). On success, fills in
// *out_ip/*out_netmask/*out_gateway from the lease and returns true.
// *out_gateway is left as 0 if the server's ack didn't include a
// router option (some do omit it, e.g. an isolated /24 with no
// upstream) -- 0 is ip.c's/net.c's own existing convention for "no
// gateway configured", not a special DHCP one. *out_dns works the
// same way for option 6 (domain name server) -- 0 if the server
// didn't offer one; net.c falls back to its own NET_STATIC_DNS
// override in that case (see docs/networking.md's "Config" section).
// Only the FIRST address in a multi-address option 6 is used --
// dns.c only ever talks to one nameserver at a time, there's nowhere
// to put a second one.
//
// Must be called after eth_init() but before arp_init()/ip_init()/
// tcp_init() are called with a REAL address -- it needs those three
// still at their "no address yet" (0) state itself while it runs
// (see ip_handle()'s own comment in ip.c for why), and net.c's own
// main() re-calls all three once this returns, with whichever
// address (DHCP's or the static fallback) actually won.
bool dhcp_acquire(const uint8_t mac[6],
	uint32_t *out_ip, uint32_t *out_netmask, uint32_t *out_gateway,
	uint32_t *out_dns);

#endif
