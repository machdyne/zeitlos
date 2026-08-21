#ifndef DNS_H
#define DNS_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * DNS (RFC 1035) client -- A-record lookups only, one resolution in
 * flight at a time, same "one X at a time" simplification as
 * everything else in this app (TFTP's one transfer, TCP's one
 * connection). Exposed to other processes over messaging (znet.h's
 * Z_NET_DNS_RESOLVE/_REPLY); sw/common/zdns.h's z_dns_resolve()/
 * z_resolve_host() are the blocking wrapper most callers actually
 * want instead of sending those messages directly. See dns.c's own
 * header comment for the wire-level design (query construction,
 * name-compression handling on the reply, retry/timeout behavior).
 *
 * Unlike dhcp.c, this one DOES fit net.c's normal non-blocking
 * main-loop poll pattern (arp.c/tftp.c/tcp.c's own shape) -- a
 * resolution is kicked off by a message from some other process
 * (net.c's own message loop), not something net needs before it can
 * do anything else useful, so there's no reason to block net's own
 * main loop the way dhcp_acquire() blocks it once at startup.
 */

#include <stdint.h>
#include <stdbool.h>

// sets (or clears, with 0) the nameserver used for every future
// query. Doesn't affect a resolution already in flight. Called once
// from net.c's main() with whichever address wins (DHCP's option 6,
// the NET_STATIC_DNS build-time override, or neither -- see
// docs/networking.md's "Config"/"DNS client" sections), but nothing
// stops a future caller from re-calling this to change nameservers
// at runtime if that's ever needed.
void dns_set_nameserver(uint32_t ip);

// dispatched from net.c's message loop for Z_NET_DNS_RESOLVE.
// Handles EVERY outcome itself, including the immediate-failure ones
// (no nameserver configured, already busy with another resolution) --
// always sends exactly one Z_NET_DNS_RESOLVE_REPLY back to
// (requester_pid, tag), whether that happens synchronously in this
// call or later, once dns_poll() sees a reply or gives up. Callers
// (net.c) don't need to pre-check anything themselves.
void dns_resolve_start(const char *hostname, uint32_t requester_pid, uint32_t tag);

// dispatched from udp.c for whatever local port dns_set_nameserver()
// registered a listener on -- see dns.c, there's no reason for net.c
// itself to know that port number.

// call every main-loop iteration (net.c, alongside eth_poll()/
// ip_poll()/etc.) -- drives retransmission and the overall timeout
// for whatever resolution is currently in flight. Cheap no-op
// (single `if`) when nothing's pending, which is the overwhelming
// majority of calls in practice.
void dns_poll(void);

#endif
