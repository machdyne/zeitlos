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
 *
 *   Reviewed once SSH was working, and deliberately LEFT AS IS. This
 *   is a throughput property, not a correctness one: tcp_handle()
 *   delivers only on an exact seq == rcv_nxt match, so the byte
 *   stream handed to a listener is always in order and never
 *   duplicated. SSH's per-packet MAC is therefore never at risk from
 *   it, which is worth stating plainly because the opposite was
 *   assumed for a while.
 *
 *   The window advertised below is a fixed 2048 bytes, so a peer can
 *   have at most ~4 segments in flight and a single loss costs at
 *   most that much retransmission. Reassembly would save around 1.5KB
 *   of resend -- the window, not the algorithm, is what caps the win,
 *   so reassembly is only worth doing together with a larger window.
 *
 *   The cheaper half of the same problem is still open: this file
 *   does NOT send a duplicate ACK when it drops an out-of-order
 *   segment, so the peer never sees the three dup-ACKs that would
 *   trigger fast retransmit and always waits a full RTO (200ms on
 *   Linux) instead of recovering in about one RTT. RFC 5681 asks for
 *   that immediate dup-ACK and it is one line -- `send_ack()` in the
 *   gap branch of tcp_handle(). Not done yet, on purpose: nothing has
 *   been measured losing packets on this hardware, and any change
 *   here wants a host harness that deliberately drops, reorders and
 *   duplicates segments first.
 * - **One option: MSS, on the SYN.** No window scaling, no SACK, no
 *   timestamps. Without the MSS option both ends fall back to RFC
 *   879's 536 bytes, and a 258KB transfer then arrives as roughly 480
 *   segments rather than 180 -- each one an interrupt, a wake-up and
 *   an ACK. Measured on hardware, that per-segment cost rather than
 *   the 10Mbit link was what held a body transfer to about 14 KB/s.
 *
 *   We advertise TCP_MAX_RX_PAYLOAD, which is the Ethernet MTU less
 *   headers and is also the largest segment this stack will deliver
 *   whole. Those two must stay in step.
 *
 *   TCP_MAX_PAYLOAD below still governs what we SEND, and stays at
 *   536: the peer's MSS is whatever it advertises, which is not
 *   parsed here.
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

// max application-data bytes we ever SEND in one segment -- see the
// "no options at all" note above for why this is exactly RFC 879's
// default MSS rather than something derived from ETH_MTU. This is a
// SEND-side convention, not an enforced limit on what a peer can send
// TO us -- see TCP_MAX_RX_PAYLOAD below for the receive-side version
// of this constant, which is NOT the same number and must not be
// confused with it.
#define TCP_MAX_PAYLOAD  536

// max application-data bytes we can ever RECEIVE in one segment --
// this file's own receive path (tcp_handle(), tcp.c) never sends an
// MSS option in its SYN, so RFC 879 says a well-behaved peer SHOULD
// default to sending us no more than the 536-byte TCP_MAX_PAYLOAD
// above -- but "should" isn't "must", and nothing here actually
// enforces it against what a peer genuinely sends. A real,
// server-controlled TCP segment can be as large as this link's own
// physical MTU allows (ip.c's own IP_MAX_PAYLOAD, 1480, minus this
// file's own 20-byte TCP_HDR_LEN) regardless of what we'd prefer.
// Confirmed as a real gap on real hardware: telnet.c's own
// clean[TCP_MAX_PAYLOAD] buffer (its receive-side parse output,
// sized off the WRONG one of these two constants) could overflow if
// a real server ever sent a single segment larger than 536 bytes --
// nothing between tcp_handle()'s own data_len computation and that
// buffer ever clamped it. Fixed at both ends: tcp_handle() itself now
// clamps data_len to this constant before ever handing it to a
// listener (defense at the source, correct regardless of any one
// listener's own buffer size), and telnet.c's clean[] is now sized to
// match this constant instead of the send-side one.
#define TCP_MAX_RX_PAYLOAD  (1480 - 20)

// -- MSS we advertise, and the window that goes with it --
//
// Set from MEASUREMENT, not from a buffer size. This tree supports
// three ethernet controllers -- RMII (rtl/ethmac_rmii.v), ENC28J60
// over SPI, and esp32link (rtl/esp32_rxfifo.v) -- with different
// buffering, some of it inside an external chip. `web` has to work on
// all three, so numbers derived from any one of them are wrong
// somewhere else.
//
// Measured on Lakritz (ENC28J60) against en.wikipedia.org, 258KB:
//
//   MSS  536, window 8192   18.3s
//   MSS 1460, window 8192   23.8s   208 in order, 0 dup, 125 gap
//   MSS  536, window 1608   23.5s   506 in order, 0 dup,  65 gap
//
// A larger MSS was worse, and so was a smaller window. 536 with a
// generous window is what actually performs, so that is the default.
//
//   make TCP_ADVERTISE_MSS=1460 TCP_RX_WINDOW=16384
//
// is how to try otherwise on a controller with more buffer. The MSS
// option is still sent explicitly: an absent option means 536 to the
// peer anyway, but saying it documents itself.
//
// What the `gap` counts show is that the remaining cost is LOSS, not
// packet rate -- and with out-of-order reassembly (below) a gap costs
// one retransmit instead of discarding everything behind it.
#ifndef TCP_ADVERTISE_MSS
#define TCP_ADVERTISE_MSS 536
#endif


#ifndef TCP_RX_WINDOW
#define TCP_RX_WINDOW 8192
#endif

// Lowers the advertised receive window, and announces it reopening.
//
// This is the only way a listener that cannot keep up can slow the
// peer down. Data arrives from the interrupt path and is ACKed
// immediately, so by the time the listener knows it is behind,
// refusing the bytes is no longer available -- the peer believes they
// were delivered. Shrinking the window is what stops more arriving.
//
// tcp_ack_now() matters as much as tcp_set_rx_window(): a peer told
// the window is zero waits for an update, and in a stop-and-wait
// sender nothing else will produce one.
// Receive-path counters since the last reset: segments accepted in
// order, duplicates re-ACKed, and out-of-order segments dropped.
//
// Throughput far below the link rate has two very different causes --
// per-segment overhead, or loss and retransmission -- and from
// outside they look the same. A clean transfer is nearly all
// in_order; anything else points at the RX ring or the window rather
// than at cost per packet.
// -- out-of-order reassembly: EXPERIMENTAL, OFF --
//
// tcp.c can hold one contiguous run of data that arrives ahead of
// rcv_nxt and deliver it when the hole fills. The logic passes
// sw/apps/net/tests/test_tcp_ooo.c. On hardware it did not pass
// anything:
//
//   without   506 in order,  0 dup,  65 gap   23.5s, page rendered
//   with      379 in order, 50 dup, 240 gap   body arrived 127 bytes
//                                             SHORT and the fetch failed
//
// More gaps, duplicates appearing where there were none, and lost
// bytes -- so an interaction with the real sender's retransmit
// behaviour that the host test does not model. It is kept behind
// this switch rather than deleted because the idea is right and the
// test is worth having; it is OFF because a stack that loses bytes
// is worse than one that is slow.
//
//   make TCP_REASSEMBLY=1
//
// to pick it up again. The first thing to instrument is which of the
// ooo_store() rejection branches fires against a real peer.
#ifndef TCP_REASSEMBLY
#define TCP_REASSEMBLY 0
#endif

#ifndef TCP_OOO_BUF
#define TCP_OOO_BUF 4096
#endif

void tcp_stats(uint32_t *in_order, uint32_t *dup, uint32_t *gap);
void tcp_stats_reset(void);

// The CEILING on the advertised window, set once from the PHY's
// receive capacity (net_phy.h). Until it is called the compile-time
// TCP_RX_WINDOW applies.
//
// This is the number that decides whether a bulk transfer runs or
// stalls, and it is a property of the NIC, not of TCP: with no
// out-of-order reassembly, a frame the hardware cannot hold is
// discarded along with everything behind it.
void tcp_set_rx_window_max(uint16_t w);

void tcp_set_rx_window(uint16_t w);
void tcp_ack_now(void);

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
