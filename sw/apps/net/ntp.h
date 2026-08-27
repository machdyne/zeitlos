#ifndef NTP_H
#define NTP_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SNTP client (RFC 4330) -- sets the hardware RTC (rtl/rtc.v,
 * sw/common/zrtc.h) from a public time server, once shortly after the
 * network comes up and then periodically for as long as net runs.
 *
 * Non-blocking throughout, driven from net.c's main loop the same way
 * arp.c/tcp.c/dns.c are -- unlike dhcp.c, which blocks once at
 * startup because nothing else can usefully happen until it finishes.
 * The opposite is true here: a machine with no idea what time it is
 * is still a perfectly working machine, so nothing waits for this.
 *
 * -- one request in flight, and why there is no queue --
 *
 * Same "one X at a time" simplification as everything else in this app
 * (TFTP's one transfer, TCP's one connection, DNS's one resolution).
 * There is only ever one thing this wants to know and one place it
 * asks, so a queue would have nothing to hold.
 *
 * -- resolving the server name --
 *
 * The default server is a hostname, not an address (see NTP_SERVER in
 * the Makefile), which means a DNS lookup has to happen first. That
 * lookup cannot use sw/common/zdns.h's z_dns_resolve(): it blocks
 * waiting for a reply from net, and this code IS net -- net's message
 * loop is what would have to produce that reply, and it is the thing
 * that is blocked. A guaranteed deadlock, and a subtle one, since the
 * function looks perfectly innocent at the call site.
 *
 * So this calls dns_resolve_start() (dns.c) directly, naming net's own
 * pid as the requester. The reply comes back as an ordinary
 * Z_NET_DNS_RESOLVE_REPLY message into net's own mailbox -- the
 * kernel is happy to deliver a process's message to itself, it is
 * just a mailbox push (sw/os/msg.c) -- and net.c's message loop hands
 * it to ntp_handle_dns_reply() below. The tag distinguishes it from a
 * reply meant for some other process's resolution.
 *
 * -- what is deliberately NOT here --
 *
 * No clock discipline: the RTC is stepped straight to whatever the
 * server said, never slewed towards it. A step can move time
 * backwards, which anything measuring an interval must not be using
 * this clock for anyway (see zrtc.h's note on z_uptime_ticks()).
 *
 * No stratum/dispersion arithmetic, no server selection between
 * several candidates, no KoD rate-limit state machine beyond
 * declining to use a KoD packet, and no authentication. This is one
 * request to one server every hour on a hobby SOC, which is the case
 * plain SNTP was specified for.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zeitlos.h"

// Arms the client. `server_ip` non-zero pins a specific address and
// skips DNS entirely (the NET_STATIC_NTP build override); zero means
// resolve NTP_SERVER by name when the time comes.
//
// Does not send anything itself -- the first sync is scheduled a
// moment out, so it lands after net's own startup printing rather
// than in the middle of it, and so an ARP for the gateway has had a
// chance to resolve. Call once from net.c's main(), after the IP
// configuration is settled.
//
// Returns false if there is nothing for this client to do -- built
// with NTP_ENABLE=0, or a bitstream with no RTC to set. In that case
// every other function here is an inert no-op and net carries on
// exactly as it did before time sync existed.
bool ntp_init(uint32_t server_ip);

// Call every main-loop iteration. Drives the whole state machine:
// scheduling, DNS, request retransmission, timeout and the next
// sync's deadline. Cheap no-op (one comparison) in the idle state,
// which is where it spends essentially all of its time.
void ntp_poll(void);

// Ask for a sync now rather than at the scheduled time. Ignored if one
// is already in flight. Used by net.c for Z_NET_NTP_SYNC (see
// sw/common/zntp.h), which is how the clock app's Sync button reaches
// here.
void ntp_sync_now(void);

// Routes a Z_NET_DNS_RESOLVE_REPLY that belongs to this client.
// Returns true if it was ours and has been consumed, false if it is
// some other process's reply that net.c should keep handling itself.
bool ntp_handle_dns_reply(const z_msg_t *msg);

// -- status, for Z_NET_NTP_STATUS_REPLY (sw/common/zntp.h) --

// True once a sync has succeeded at least once since net started.
bool ntp_ever_synced(void);

// Uptime ticks (z_uptime_ticks()) at the last successful sync, or 0 if
// there has not been one. Ticks rather than wall-clock seconds
// deliberately: this answers "how stale is the clock", which is an
// elapsed-time question, and the wall clock is the thing being
// questioned.
uint32_t ntp_last_sync_ticks(void);

#endif
