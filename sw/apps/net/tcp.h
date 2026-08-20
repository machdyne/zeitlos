#ifndef TCP_H
#define TCP_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * TCP (RFC 793), client-only (active open) -- there's no listening
 * side anywhere in Zeitlos yet, and none is needed for telnet.
 * Non-blocking, poll-driven, same shape as arp.c/tftp.c: kick
 * something off, then call tcp_poll() every loop iteration for
 * retransmits/timeouts; actual segment handling happens via
 * tcp_handle(), dispatched from ip_handle() for protocol 6.
 *
 * Deliberately simplified relative to a general-purpose TCP, in the
 * same spirit tftp.c/udp.h document their own simplifications:
 *
 * - **One connection at a time**, a single static TCB -- same
 *   constraint TFTP already accepted for its own "one transfer at a
 *   time" (docs/networking.md). Revisit (a TCB array/table) if
 *   something ever needs concurrent TCP connections; nothing does
 *   yet.
 * - **Stop-and-wait sending**: at most one unacknowledged outbound
 *   segment at a time (tcp_send() returns false if the previous one
 *   hasn't been acked yet -- caller should just retry on a later
 *   poll). No sliding window, no pipelining. Telnet traffic is small
 *   and bursty in both directions, not throughput-sensitive, so this
 *   costs nothing in practice while avoiding a real send-window/
 *   retransmit-queue implementation entirely.
 * - **No out-of-order reassembly.** A segment that doesn't arrive
 *   with the expected sequence number is dropped (not buffered) --
 *   relies on the peer's own retransmit timer to resend it in order.
 *   Expected to be a non-issue on a local, low-latency LAN; would
 *   matter more on a lossy/reordering path.
 * - **No options at all** -- no MSS negotiation, no window scaling.
 *   Both ends fall back to RFC 879's 536-byte default MSS since
 *   neither side advertises one. TCP_MAX_PAYLOAD below matches that.
 * - **No half-close support.** A remote-initiated FIN gets our own
 *   FIN sent right back immediately (see tcp.c) instead of going
 *   through CLOSE_WAIT and waiting for the application to decide --
 *   telnet has no use for keeping one direction open after the other
 *   closes, so this isn't implemented.
 * - **No listening/passive-open side, no SYN queue.** Nothing in
 *   Zeitlos needs to accept incoming TCP connections yet.
 *
 * None of this has been run against real hardware or a real TCP
 * stack yet -- see docs/networking.md's own template for what
 * "confirmed working" looks like once it has been.
 */

#include <stdint.h>
#include <stdbool.h>

// max application-data bytes in one segment, in either direction --
// see the "no options at all" note above for why this is exactly
// RFC 879's default MSS rather than something derived from ETH_MTU.
#define TCP_MAX_PAYLOAD  536

typedef enum {
	// the handshake completed -- data/tcp_send() usable from here.
	TCP_EVENT_ESTABLISHED,

	// data arrived, in order. `data`/`len` point into a buffer only
	// valid for the duration of this callback (same borrowed-data
	// convention message payloads use, docs/messaging.md) -- copy it
	// if you need to keep it past returning.
	TCP_EVENT_DATA,

	// the connection is gone -- either end closed it (FIN exchange
	// completed, or a FIN was received and immediately answered with
	// our own, see tcp.h's own "no half-close" note), a RST arrived,
	// or the handshake/a retransmit gave up after too many retries.
	// No further tcp_send() calls are valid until tcp_connect() is
	// called again. If this arrives before TCP_EVENT_ESTABLISHED
	// ever did, the connection attempt itself failed -- the caller
	// asked to connect but never got established.
	TCP_EVENT_CLOSED,

} tcp_event_t;

typedef void (*tcp_event_handler_t)(tcp_event_t ev, const uint8_t *data, uint16_t len);

// call once at startup (net.c, alongside arp_init()/ip_init()) --
// needed for tcp_checksum()'s pseudo-header, though ip_our_addr()
// (ip.h) could also supply this; kept as an explicit init call for
// symmetry with arp_init()/ip_init() rather than a hidden dependency
// on ip.c having already run.
void tcp_init(uint32_t our_ip);

// starts an active open to dst_ip:dst_port. returns false if a
// connection is already in progress/established (see "one connection
// at a time" above) -- close it first. `handler` is called for every
// event on this connection from here until TCP_EVENT_CLOSED.
bool tcp_connect(uint32_t dst_ip, uint16_t dst_port, tcp_event_handler_t handler);

// true only in the ESTABLISHED state -- tcp_send() is only valid
// while this is true.
bool tcp_is_connected(void);

// queues len bytes for sending. returns false if not established, if
// len exceeds TCP_MAX_PAYLOAD, or if the previous segment sent isn't
// acked yet (see "stop-and-wait" above) -- the caller should hold the
// data and just call this again on a later poll in that last case.
bool tcp_send(const uint8_t *data, uint16_t len);

// begins a graceful active close (sends FIN) from the ESTABLISHED
// state. Returns false (no-op) if not established, or if the
// previous segment isn't acked yet -- same stop-and-wait constraint
// tcp_send() has, since FIN consumes the same single outstanding-
// segment slot. TCP_EVENT_CLOSED fires once the close completes.
bool tcp_close(void);

// forces the connection down right away: RST's the peer (best
// effort -- doesn't wait for or retransmit it) if there's an active
// connection, and resets local state to CLOSED immediately, skipping
// any FIN exchange. For a caller that needs to abandon a connection
// NOW (e.g. the port client hung up) rather than waiting out a
// graceful close. Does not itself call the event handler -- the
// caller already knows why it's closing.
void tcp_abort(void);

// dispatched from ip_handle() for protocol 6 (TCP).
void tcp_handle(uint32_t src_ip, const uint8_t *payload, uint16_t len);

// call every main-loop iteration (alongside eth_poll()) -- handles
// retransmit timeouts and TIME_WAIT expiry. No-op if there's no
// connection in progress.
void tcp_poll(void);

#endif
