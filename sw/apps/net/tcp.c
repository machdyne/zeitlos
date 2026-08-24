/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * TCP client. See tcp.h for the simplifications this takes relative
 * to a general-purpose TCP (one connection at a time, stop-and-wait
 * sending, no out-of-order reassembly, no options, no half-close).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "tcp.h"
#include "ip.h"
#include "../../common/zeitlos.h"

#define TCP_HDR_LEN  20

#define TCP_FLAG_FIN  0x01
#define TCP_FLAG_SYN  0x02
#define TCP_FLAG_RST  0x04
#define TCP_FLAG_PSH  0x08
#define TCP_FLAG_ACK  0x10

// retransmit tuning -- ticks at ~732Hz, see z_uptime_ticks()'s own
// comment (docs/networking.md, "z_uptime_ticks()") and tftp.c's use
// of the same tick rate for its own retry timer. Backoff doubles per
// retry (capped) rather than staying constant, same idea as a real
// TCP's RTO backoff, cheap to add and meaningfully better than a
// fixed timeout under real packet loss.
#define TCP_RTO_TICKS_BASE   366     // ~0.5s
#define TCP_RTO_MAX_SHIFT    4       // caps backoff at base*16 (~8s)
#define TCP_MAX_RETRIES      7

// shortened from the textbook 2*MSL -- this is a dev-tool client, not
// a public-facing server that needs to guard against duplicate SYNs
// from a long-dead connection reusing the same 4-tuple; a few seconds
// is plenty to let any last stray segment drain.
#define TCP_TIME_WAIT_TICKS  (732 * 3)

typedef enum {
	TCP_CLOSED = 0,
	TCP_SYN_SENT,
	TCP_ESTABLISHED,
	TCP_FIN_WAIT_1,
	TCP_FIN_WAIT_2,
	TCP_CLOSING,
	TCP_TIME_WAIT,
	TCP_LAST_ACK,
} tcp_state_t;

typedef struct {

	tcp_state_t state;

	uint32_t remote_ip;
	uint16_t remote_port;
	uint16_t local_port;

	uint32_t snd_una;	// oldest unacked seq we've sent
	uint32_t snd_nxt;	// next seq we'll use
	uint32_t rcv_nxt;	// next seq we expect from the peer

	// -- the single outstanding (unacked) segment, if any -- see
	// tcp.h's "stop-and-wait" note. tx_pending is the only thing
	// that distinguishes "nothing outstanding" from "a bare
	// SYN/FIN/ACK with 0 data bytes is outstanding", since tx_len
	// alone can legitimately be 0 in both cases.
	bool     tx_pending;
	uint32_t tx_seq;
	uint8_t  tx_flags;
	uint16_t tx_len;
	uint8_t  tx_buf[TCP_MAX_PAYLOAD];
	uint32_t last_send_tick;
	uint8_t  retries;

	uint32_t time_wait_start;

	tcp_event_handler_t handler;

} tcp_tcb_t;

static tcp_tcb_t tcb;
static uint32_t our_ip;
static uint16_t next_local_port;	// seeded in tcp_init() -- see its own comment

void tcp_init(uint32_t ip) {
	our_ip = ip;
	tcb.state = TCP_CLOSED;
	tcb.tx_pending = false;
	tcb.handler = NULL;

	// seed from boot-time ticks rather than a fixed 49152 every time
	// -- with our_ip also fixed (net.c's own OUR_IP), a hardcoded
	// starting port meant EVERY connection to the same remote
	// server:port, across every reboot of this board, reused the
	// exact same (our_ip, port, remote_ip, remote_port) 4-tuple.
	// Found as the actual root cause of a real "TCP handshake never
	// completes, even against a server confirmed listening and
	// reachable" symptom: the remote server had lingering state for
	// that exact tuple from an earlier test (a normal TCP stack keeps
	// a connection's state around for a while after its peer goes
	// silent, whether that's minutes or hours depending on the OS),
	// and responded to our fresh SYN with a plain challenge ACK
	// (RFC 5961 -- "unexpected segment on what looks like an existing
	// connection") instead of a SYN-ACK, every single retry, every
	// single reboot, since we kept presenting the identical tuple it
	// already had state for. z_uptime_ticks() (docs/networking.md) is
	// the only source of "varies across boots" this codebase has --
	// not cryptographically random and doesn't need to be, just
	// different enough each boot to stop colliding with whatever the
	// last boot's connections used.
	next_local_port = 49152 + (z_uptime_ticks() % 16384);
}

// -- checksum: RFC 793 pseudo-header (src/dst IP, zero, protocol=6,
// TCP length) prepended to a plain 16-bit-word sum of the segment
// itself -- same one's-complement technique as ip.c's ip_checksum(),
// just with the pseudo-header's words folded in first. Unlike UDP
// (udp.h, checksum optional per RFC 768), a TCP checksum of 0 would
// mean the checksum genuinely IS zero, not "unused" -- it's mandatory
// here.
static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
	const uint8_t *seg, uint16_t seg_len) {

	uint32_t sum = 0;

	sum += (src_ip >> 16) & 0xFFFF;
	sum += src_ip & 0xFFFF;
	sum += (dst_ip >> 16) & 0xFFFF;
	sum += dst_ip & 0xFFFF;
	sum += 6;				// zero byte + protocol (TCP=6) as one 16-bit word
	sum += seg_len;			// TCP length (header+data), as one 16-bit word

	for (uint16_t i = 0; (uint16_t)(i + 1) < seg_len; i += 2)
		sum += ((uint32_t)seg[i] << 8) | seg[i + 1];
	if (seg_len & 1)
		sum += (uint32_t)seg[seg_len - 1] << 8;

	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);

	return (uint16_t)(~sum);

}

// builds and sends one segment. does NOT touch tcb.tx_* itself --
// callers that need retransmit tracking (send_tracked() below) handle
// that separately, since some sends (pure ACKs, RSTs) are never
// retransmitted at all.
static bool tcp_send_segment(uint32_t seq, uint8_t flags,
	const uint8_t *data, uint16_t len) {

	static uint8_t pkt[TCP_HDR_LEN + TCP_MAX_PAYLOAD];

	pkt[0] = (tcb.local_port >> 8) & 0xFF;
	pkt[1] = tcb.local_port & 0xFF;
	pkt[2] = (tcb.remote_port >> 8) & 0xFF;
	pkt[3] = tcb.remote_port & 0xFF;

	pkt[4] = (seq >> 24) & 0xFF;
	pkt[5] = (seq >> 16) & 0xFF;
	pkt[6] = (seq >> 8) & 0xFF;
	pkt[7] = seq & 0xFF;

	// we always ack our latest rcv_nxt on every outbound segment once
	// we've received anything (harmless/ignored by the peer if the
	// ACK flag isn't actually set) -- simpler than tracking whether
	// this exact segment "needs" an ack field filled in.
	uint32_t ack = tcb.rcv_nxt;
	pkt[8] = (ack >> 24) & 0xFF;
	pkt[9] = (ack >> 16) & 0xFF;
	pkt[10] = (ack >> 8) & 0xFF;
	pkt[11] = ack & 0xFF;

	pkt[12] = (5 << 4);	// data offset: 5 words (20 bytes), no options
	pkt[13] = flags;

	uint16_t window = 2048;	// fixed, generous relative to
								// TCP_MAX_PAYLOAD -- never actually
								// constrains anything since we only
								// ever have one segment outstanding
								// ourselves either way (see tcp.h)
	pkt[14] = (window >> 8) & 0xFF;
	pkt[15] = window & 0xFF;

	pkt[16] = 0; pkt[17] = 0;	// checksum, filled in below
	pkt[18] = 0; pkt[19] = 0;	// urgent pointer, unused

	if (len) memcpy(pkt + TCP_HDR_LEN, data, len);

	uint16_t seg_len = TCP_HDR_LEN + len;
	uint16_t csum = tcp_checksum(our_ip, tcb.remote_ip, pkt, seg_len);
	pkt[16] = (csum >> 8) & 0xFF;
	pkt[17] = csum & 0xFF;

	return ip_send(tcb.remote_ip, 6, pkt, seg_len);

}

// sends a new segment and arms it for retransmit tracking -- for
// anything that consumes a sequence number (SYN, FIN, or real data)
// and therefore needs the peer to actually ack it. advances snd_nxt
// by the number of sequence numbers this segment consumes. caller is
// responsible for checking !tcb.tx_pending first (single outstanding
// segment, see tcp.h).
static void send_tracked(uint8_t flags, const uint8_t *data, uint16_t len) {

	uint32_t seq = tcb.snd_nxt;

	tcp_send_segment(seq, flags, data, len);

	tcb.tx_pending = true;
	tcb.tx_seq = seq;
	tcb.tx_flags = flags;
	tcb.tx_len = len;
	if (len) memcpy(tcb.tx_buf, data, len);
	tcb.last_send_tick = z_uptime_ticks();
	tcb.retries = 0;

	uint32_t consumed = len;
	if (flags & TCP_FLAG_SYN) consumed++;
	if (flags & TCP_FLAG_FIN) consumed++;
	tcb.snd_nxt += consumed;

}

// a pure ACK (or a best-effort RST) -- never retransmitted, doesn't
// touch snd_nxt/tx_pending at all.
static void send_ack(void) {
	tcp_send_segment(tcb.snd_nxt, TCP_FLAG_ACK, NULL, 0);
}

static void send_rst(void) {
	tcp_send_segment(tcb.snd_nxt, TCP_FLAG_RST, NULL, 0);
}

static void reset_to_closed(void) {
	tcb.state = TCP_CLOSED;
	tcb.tx_pending = false;
	tcb.handler = NULL;
}

// -- public API --

bool tcp_connect(uint32_t dst_ip, uint16_t dst_port, tcp_event_handler_t handler) {

	if (tcb.state != TCP_CLOSED) return false;

	tcb.remote_ip = dst_ip;
	tcb.remote_port = dst_port;
	tcb.local_port = next_local_port++;
	if (next_local_port == 0) next_local_port = 49152;	// wrap, avoid port 0

	// not cryptographically random and doesn't need to be -- just
	// varies per connection so two connections in a row don't reuse
	// the exact same ISN. z_uptime_ticks() (docs/networking.md) is
	// the only source of "changes over time" this codebase has.
	uint32_t isn = z_uptime_ticks() * 2654435761u;	// Knuth multiplicative hash, cheap decorrelation

	tcb.snd_una = isn;
	tcb.snd_nxt = isn;
	tcb.rcv_nxt = 0;
	tcb.handler = handler;
	tcb.tx_pending = false;

	tcb.state = TCP_SYN_SENT;
	send_tracked(TCP_FLAG_SYN, NULL, 0);

	return true;

}

bool tcp_is_connected(void) {
	return tcb.state == TCP_ESTABLISHED;
}

bool tcp_send(const uint8_t *data, uint16_t len) {

	if (tcb.state != TCP_ESTABLISHED) return false;
	if (tcb.tx_pending) return false;	// previous segment not yet acked -- retry on a later poll
	if (len > TCP_MAX_PAYLOAD) return false;

	send_tracked(TCP_FLAG_ACK | TCP_FLAG_PSH, data, len);
	return true;

}

bool tcp_close(void) {

	if (tcb.state != TCP_ESTABLISHED) return false;
	if (tcb.tx_pending) return false;	// FIN needs the same single slot -- see tcp.h

	send_tracked(TCP_FLAG_ACK | TCP_FLAG_FIN, NULL, 0);
	tcb.state = TCP_FIN_WAIT_1;
	return true;

}

void tcp_abort(void) {
	if (tcb.state != TCP_CLOSED) send_rst();
	reset_to_closed();
}

static void notify(tcp_event_t ev, const uint8_t *data, uint16_t len) {
	// diagnostic: a NULL handler here used to be silent -- this is
	// exactly what a real bug looked like (reset_to_closed() clearing
	// tcb.handler before notify() ran, see tcp_handle()'s RST branch
	// and tcp_poll()'s retry-giveup branch, both now fixed to notify
	// first) -- printing this instead of just returning means a
	// similar future ordering mistake shows up immediately instead of
	// as a silent "term never heard back" symptom two layers away.
	if (!tcb.handler) {
		printf("tcp: notify(event=%d) with no handler registered -- dropped\n",
			(int)ev);
		return;
	}
	tcb.handler(ev, data, len);
}

// called whenever an inbound segment's ACK flag is set and its ack
// number fully covers our single outstanding segment -- clears
// tx_pending and advances snd_una. does nothing (and returns false)
// for a partial or stale ack, which with only ever one segment
// outstanding at a time should only happen for a duplicate ack of
// data we already had confirmed -- the retransmit timer, not this
// function, is what would eventually re-send in that case.
static bool process_ack(uint32_t ack_num) {

	if (!tcb.tx_pending) return false;

	uint32_t consumed = tcb.tx_len;
	if (tcb.tx_flags & TCP_FLAG_SYN) consumed++;
	if (tcb.tx_flags & TCP_FLAG_FIN) consumed++;

	if (ack_num - tcb.tx_seq < consumed) return false;	// doesn't fully cover it yet

	tcb.snd_una = tcb.tx_seq + consumed;
	tcb.tx_pending = false;
	return true;

}

void tcp_handle(uint32_t src_ip, const uint8_t *p, uint16_t len) {

	if (tcb.state == TCP_CLOSED) return;	// no active connection -- nothing to dispatch to
	if (len < TCP_HDR_LEN) return;

	uint16_t src_port = (p[0] << 8) | p[1];
	uint16_t dst_port = (p[2] << 8) | p[3];

	// only ever one connection -- anything not matching it by all
	// four of {src ip, src port, dst port, and (implicitly) our own
	// IP, which ip_handle() already filtered on before calling us}
	// isn't ours.
	if (src_ip != tcb.remote_ip || src_port != tcb.remote_port ||
		dst_port != tcb.local_port) return;

	uint32_t seq = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
		((uint32_t)p[6] << 8) | p[7];
	uint32_t ack = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) |
		((uint32_t)p[10] << 8) | p[11];

	uint8_t data_offset = (p[12] >> 4) * 4;
	uint8_t flags = p[13];

	if (data_offset < TCP_HDR_LEN || data_offset > len) return;	// malformed

	const uint8_t *data = p + data_offset;
	uint16_t data_len = len - data_offset;

	// RST: unconditional, any state -- the connection is gone right
	// now, no FIN exchange to wait for.
	if (flags & TCP_FLAG_RST) {
		bool was_established = (tcb.state != TCP_SYN_SENT);
		// notify() BEFORE reset_to_closed() -- reset_to_closed() sets
		// tcb.handler = NULL, and notify() only calls the handler if
		// it's non-NULL, so the old order silently dropped every
		// TCP_EVENT_CLOSED delivered this way: the callback (e.g.
		// net.c's telnet_on_closed(), which is what actually sends
		// Z_PORT_REFUSED back to a waiting `term`) never ran at all.
		// Found via a real symptom on real hardware: net's own
		// "giving up after N retries" print (this file's tcp_poll(),
		// same bug, same fix) but no corresponding
		// "telnet connect... failed" from net.c ever followed it, and
		// `term` timed out instead of seeing an explicit refusal.
		notify(TCP_EVENT_CLOSED, NULL, 0);
		reset_to_closed();
		(void)was_established;	// same event either way -- see tcp.h's TCP_EVENT_CLOSED doc
		return;
	}

	switch (tcb.state) {

	case TCP_SYN_SENT: {

		if (!(flags & TCP_FLAG_SYN) || !(flags & TCP_FLAG_ACK)) return;
		// require our SYN to actually be the thing acked -- see
		// tcp.h's "no options" note; we don't handle simultaneous
		// open (SYN without ACK) since we're never a listener.
		if (!process_ack(ack)) return;

		tcb.rcv_nxt = seq + 1;	// SYN consumes one sequence number
		tcb.state = TCP_ESTABLISHED;
		send_ack();
		notify(TCP_EVENT_ESTABLISHED, NULL, 0);
		break;

	}

	case TCP_ESTABLISHED: {

		if (flags & TCP_FLAG_ACK) process_ack(ack);

		if (data_len > 0) {
			if (seq == tcb.rcv_nxt) {
				tcb.rcv_nxt += data_len;
				send_ack();
				// deliver at most TCP_MAX_RX_PAYLOAD bytes to the
				// listener, NOT data_len itself -- see tcp.h's own
				// TCP_MAX_RX_PAYLOAD comment for the real-hardware
				// overflow this closes (telnet.c's clean[] buffer,
				// confirmed reachable since nothing here ever bounded
				// what a listener actually receives). Deliberately
				// only clamps the delivered length, not data_len
				// itself above: rcv_nxt/send_ack() must still reflect
				// the TRUE number of bytes this segment contained,
				// or our own ACK would silently claim to have
				// received less than the peer actually sent,
				// desyncing sequence tracking for every segment after
				// this one. Worst case here, an implausibly large
				// single segment's tail bytes are correctly ACKed but
				// never actually delivered to the application --
				// same "peer's own retransmit timer sorts out
				// whatever this end drops" philosophy this file's own
				// header comment already applies to out-of-order
				// segments.
				uint16_t deliver_len = data_len;
				if (deliver_len > TCP_MAX_RX_PAYLOAD)
					deliver_len = TCP_MAX_RX_PAYLOAD;
				notify(TCP_EVENT_DATA, data, deliver_len);
			} else if (seq < tcb.rcv_nxt) {
				// already-seen retransmit -- re-ack so the peer
				// stops retransmitting it, don't deliver it again
				send_ack();
			}
			// seq > rcv_nxt: a real gap -- no reassembly (tcp.h),
			// drop silently and let the peer's own timeout resend it
			// in order.
		}

		if (flags & TCP_FLAG_FIN) {
			// only accept an in-order FIN -- same reasoning as data
			// above. an out-of-order FIN (data gap before it) is
			// left for the peer's retransmit to resolve, same as any
			// other out-of-order segment.
			uint32_t fin_seq = seq + data_len;
			if (fin_seq == tcb.rcv_nxt) {
				tcb.rcv_nxt++;
				send_ack();
				// no half-close support (tcp.h) -- answer their FIN
				// with our own right away rather than lingering in
				// CLOSE_WAIT.
				if (!tcb.tx_pending) {
					send_tracked(TCP_FLAG_ACK | TCP_FLAG_FIN, NULL, 0);
					tcb.state = TCP_LAST_ACK;
				} else {
					// our own data segment was still outstanding --
					// rare (would need the peer to FIN mid-exchange
					// while we also had unacked data in flight).
					// finish that handshake via normal retransmit/ack
					// first; once tx_pending clears we're stuck
					// without a FIN queued behind it in this simple
					// model. Documented gap, not expected to matter
					// for interactive telnet traffic -- revisit if it
					// ever does (tcp.h's own "ship simple, iterate"
					// precedent).
					tcb.state = TCP_LAST_ACK;
				}
				notify(TCP_EVENT_CLOSED, NULL, 0);
			}
		}

		break;

	}

	case TCP_FIN_WAIT_1: {

		bool our_fin_acked = (flags & TCP_FLAG_ACK) && process_ack(ack);

		if (flags & TCP_FLAG_FIN) {
			tcb.rcv_nxt = seq + 1;
			send_ack();
			tcb.state = our_fin_acked ? TCP_TIME_WAIT : TCP_CLOSING;
			if (tcb.state == TCP_TIME_WAIT) tcb.time_wait_start = z_uptime_ticks();
		} else if (our_fin_acked) {
			tcb.state = TCP_FIN_WAIT_2;
		}

		break;

	}

	case TCP_FIN_WAIT_2: {

		if (flags & TCP_FLAG_FIN) {
			tcb.rcv_nxt = seq + 1;
			send_ack();
			tcb.state = TCP_TIME_WAIT;
			tcb.time_wait_start = z_uptime_ticks();
		}

		break;

	}

	case TCP_CLOSING: {

		if ((flags & TCP_FLAG_ACK) && process_ack(ack)) {
			tcb.state = TCP_TIME_WAIT;
			tcb.time_wait_start = z_uptime_ticks();
		}

		break;

	}

	case TCP_LAST_ACK: {

		if ((flags & TCP_FLAG_ACK) && process_ack(ack))
			reset_to_closed();	// fully done -- handler already got
								// TCP_EVENT_CLOSED when the peer's
								// FIN first arrived, above

		break;

	}

	case TCP_TIME_WAIT:
		// stray retransmits of the peer's final FIN/ACK can still
		// arrive here -- re-ack, don't otherwise react.
		if (flags & TCP_FLAG_FIN) send_ack();
		break;

	default:
		break;

	}

}

void tcp_poll(void) {

	if (tcb.state == TCP_CLOSED) return;

	if (tcb.state == TCP_TIME_WAIT) {
		if (z_uptime_ticks() - tcb.time_wait_start >= TCP_TIME_WAIT_TICKS)
			reset_to_closed();
		return;
	}

	if (!tcb.tx_pending) return;

	uint32_t rto = TCP_RTO_TICKS_BASE <<
		(tcb.retries < TCP_RTO_MAX_SHIFT ? tcb.retries : TCP_RTO_MAX_SHIFT);

	if (z_uptime_ticks() - tcb.last_send_tick < rto) return;

	if (tcb.retries >= TCP_MAX_RETRIES) {
		printf("tcp: giving up after %d retries, connection abandoned\n",
			TCP_MAX_RETRIES);
		// notify() BEFORE reset_to_closed() -- see tcp_handle()'s RST
		// branch above for why the old order (reset first) silently
		// dropped this event: reset_to_closed() clears tcb.handler,
		// and notify() only calls it if non-NULL. This was the
		// specific path behind a real symptom: this printf() would
		// fire, but net.c's telnet_on_closed() (registered as
		// tcb.handler by tcp_connect(), via telnet_connect()) never
		// ran, so its own "telnet connect... failed" print never
		// followed, and no Z_PORT_REFUSED was ever sent -- `term`
		// just timed out on its own end instead.
		notify(TCP_EVENT_CLOSED, NULL, 0);
		reset_to_closed();
		return;
	}

	tcp_send_segment(tcb.tx_seq, tcb.tx_flags, tcb.tx_buf, tcb.tx_len);
	tcb.last_send_tick = z_uptime_ticks();
	tcb.retries++;

}
