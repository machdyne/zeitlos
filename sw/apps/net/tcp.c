/*
 * Zeitlos
 * Copyright (c) 2025-2026 Lone Dynamics Corporation. All rights reserved.
 *
 * TCP. See tcp.h for the simplifications this takes relative to a
 * general-purpose TCP (stop-and-wait sending, no out-of-order
 * reassembly by default, one option, no half-close) and for the pool
 * of connections and listening that replaced the single TCB.
 *
 * -- a handle is a pointer into the pool --
 *
 * A slot is reused as soon as it is free, so a handle is dead the
 * moment its connection is: after TCP_EVENT_CLOSED, or after the
 * caller's own tcp_abort(). Inside this file the same hazard applies to
 * every notify(): a handler may abort its own connection and open a new
 * one in the same slot before returning. Every place that keeps using a
 * connection after notifying checks its generation (still()).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "tcp.h"
#include "ip.h"
#include "../../common/zeitlos.h"
#include "../../common/zrng.h"

#define TCP_HDR_LEN  20

#define TCP_FLAG_FIN  0x01
#define TCP_FLAG_SYN  0x02
#define TCP_FLAG_RST  0x04
#define TCP_FLAG_PSH  0x08
#define TCP_FLAG_ACK  0x10

// retransmit tuning -- ticks at ~732Hz. Backoff doubles per retry
// (capped), like a real TCP's RTO backoff.
#define TCP_RTO_TICKS_BASE   366     // ~0.5s
#define TCP_RTO_MAX_SHIFT    4       // caps backoff at base*16 (~8s)
#define TCP_MAX_RETRIES      7

// A SYN-ACK is retried fewer times than anything else: a half-open
// connection holds a pool slot, and a peer that never completes the
// handshake -- a scan, a flood -- should give it back in seconds
// (0.5 + 1 + 2 + 4), not the half-minute an established connection
// deserves.
#define TCP_SYNACK_RETRIES   3

// shortened from the textbook 2*MSL: a few seconds is plenty to let
// any last stray segment drain, and a slot in TIME_WAIT is reclaimed
// early anyway when the pool runs short (alloc()).
#define TCP_TIME_WAIT_TICKS  (732 * 3)

typedef enum {
	TCP_CLOSED = 0,
	TCP_SYN_SENT,
	TCP_SYN_RCVD,
	TCP_ESTABLISHED,
	TCP_FIN_WAIT_1,
	TCP_FIN_WAIT_2,
	TCP_CLOSING,
	TCP_TIME_WAIT,
	TCP_LAST_ACK,
	TCP_CLOSE_WAIT,     // the peer's FIN is in; we may still send (half-close)
} tcp_state_t;

struct tcp_conn {

	tcp_state_t state;
	uint32_t gen;           // bumped every time the slot is freed

	uint32_t remote_ip;
	uint16_t remote_port;
	uint16_t local_port;

	uint32_t snd_una;       // oldest unacked seq we've sent
	uint32_t snd_nxt;       // next seq we'll use
	uint32_t rcv_nxt;       // next seq we expect from the peer

	// -- the single outstanding (unacked) segment, if any -- see
	// tcp.h's "stop-and-wait" note. tx_pending is the only thing that
	// tells "nothing outstanding" from "a bare SYN/FIN is", since
	// tx_len can legitimately be 0 in both.
	bool     tx_pending;
	uint32_t tx_seq;
	uint8_t  tx_flags;
	uint16_t tx_len;
	uint8_t  tx_buf[TCP_MAX_PAYLOAD];
	uint32_t last_send_tick;
	uint8_t  retries;

	// The peer closed while our own segment was still unacked: send
	// our FIN as soon as it is. It used to be left unsent, and the
	// connection sat in LAST_ACK until the retries gave up.
	bool     fin_pending;

	uint32_t time_wait_start;

	uint16_t rx_window;     // this connection's own limit (tcp_set_rx_window)
	uint16_t snd_mss;       // the most we send at once: the peer's MSS, capped
	bool     inbound;
	bool     half_close;    // tcp_set_half_close()

	tcp_event_handler_t handler;
	void *user;

};

static tcp_conn_t conns[TCP_MAX_CONN];

static struct {
	uint16_t port;
	tcp_accept_t accept;
} listens[TCP_MAX_LISTEN];

static uint32_t our_ip;
static uint16_t next_local_port;

// The ceiling on every advertised window, from the PHY. See tcp.h.
static uint16_t rx_window_max = TCP_RX_WINDOW;

// Receive-path counters, reported by tcp_stats(): in_order versus dup
// versus gap separates per-segment overhead from loss.
static uint32_t rx_in_order, rx_dup, rx_gap;

static void send_ack(tcp_conn_t *c);
static void notify(tcp_conn_t *c, tcp_event_t ev, const uint8_t *data, uint16_t len);

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
#if TCP_REASSEMBLY
// -- out-of-order reassembly --
//
// ONE contiguous run held beyond rcv_nxt.
//
// Without this, a single lost segment discards every segment behind
// it, and the peer has to resend all of them after a retransmit
// timeout. Measured on hardware that was 65 gaps in a 258KB transfer
// -- 65 stalls of a couple of hundred milliseconds each, which is
// most of the time the transfer took.
//
// With it, a gap costs exactly one retransmit: the data that arrived
// early is kept, the dup-ACKs this stack already sends prompt a fast
// retransmit of the hole, and the run is delivered the moment it is
// filled.
//
// One run rather than a queue because that is the case that actually
// happens -- one segment lost, the rest of the burst fine. A second
// gap opening while one is pending is still dropped, which needs a
// real queue and buys much less.
static uint8_t ooo_buf[TCP_OOO_BUF];
static uint32_t ooo_seq;			// sequence number of ooo_buf[0]
static uint32_t ooo_len;			// contiguous bytes held
static bool ooo_valid;
static tcp_conn_t *ooo_conn;		// the one connection that may use it

static void ooo_reset(void) {
	ooo_conn = NULL;
	ooo_valid = false;
	ooo_seq = 0;
	ooo_len = 0;
}

// Hands the listener whatever the buffer now completes, in
// TCP_MAX_RX_PAYLOAD pieces.
static void ooo_drain(tcp_conn_t *c) {

	uint32_t off;

	if (!ooo_valid || ooo_conn != c) return;

	// Only useful once rcv_nxt has reached the run. A run that starts
	// beyond rcv_nxt is still waiting for its hole to be filled.
	if ((int32_t)(c->rcv_nxt - ooo_seq) < 0) return;

	off = c->rcv_nxt - ooo_seq;
	if (off >= ooo_len) { ooo_reset(); return; }

	{
		uint32_t remain = ooo_len - off;

		c->rcv_nxt += remain;
		send_ack(c);

		while (remain) {
			uint16_t n = remain > TCP_MAX_RX_PAYLOAD
				? TCP_MAX_RX_PAYLOAD : (uint16_t)remain;
			notify(c, TCP_EVENT_DATA, ooo_buf + off, n);
			off += n;
			remain -= n;
		}
	}

	ooo_reset();

}

// Files a segment that arrived ahead of rcv_nxt.
static void ooo_store(tcp_conn_t *c, uint32_t seq, const uint8_t *data, uint16_t len) {

	if (len == 0) return;
	if (ooo_conn && ooo_conn != c) return;	// someone else's run is held
	ooo_conn = c;

	if (!ooo_valid) {
		if (len > TCP_OOO_BUF) return;
		memcpy(ooo_buf, data, len);
		ooo_seq = seq;
		ooo_len = len;
		ooo_valid = true;
		return;
	}

	// Contiguous with what is already held: extend it. Anything else
	// -- a second separate gap, or something that overlaps
	// awkwardly -- is dropped, and the peer's retransmit sorts it
	// out. Signed comparison throughout: sequence numbers wrap.
	{
		int32_t delta = (int32_t)(seq - ooo_seq);

		if (delta < 0) return;
		if ((uint32_t)delta > ooo_len) return;		// leaves a second hole

		{
			uint32_t at = (uint32_t)delta;
			uint32_t end = at + len;

			if (end <= ooo_len) return;				// wholly duplicate
			if (end > TCP_OOO_BUF) return;			// no room

			memcpy(ooo_buf + at, data, len);
			ooo_len = end;
		}
	}

}
#else
static void ooo_reset(void) { }
#endif


void tcp_stats(uint32_t *in_order, uint32_t *dup, uint32_t *gap) {
	if (in_order) *in_order = rx_in_order;
	if (dup) *dup = rx_dup;
	if (gap) *gap = rx_gap;
}

void tcp_stats_reset(void) { rx_in_order = rx_dup = rx_gap = 0; }

void tcp_set_rx_window_max(uint16_t w) {
	if (w < TCP_MAX_PAYLOAD) w = TCP_MAX_PAYLOAD;
	rx_window_max = (w > TCP_RX_WINDOW) ? TCP_RX_WINDOW : w;
	for (int i = 0; i < TCP_MAX_CONN; i++)
		if (conns[i].rx_window > rx_window_max) conns[i].rx_window = rx_window_max;
}

void tcp_set_rx_window(tcp_conn_t *c, uint16_t w) {
	c->rx_window = w > rx_window_max ? rx_window_max : w;
}

// -- the pool --

static bool active(const tcp_conn_t *c) {
	return c->state != TCP_CLOSED && c->state != TCP_TIME_WAIT;
}

void tcp_debug(const tcp_conn_t *c, bool *pending, uint8_t *retries, uint16_t *len) {
	*pending = c->tx_pending;
	*retries = c->retries;
	*len = c->tx_len;
}

int tcp_count(void) {
	int n = 0;
	for (int i = 0; i < TCP_MAX_CONN; i++) if (active(&conns[i])) n++;
	return n;
}

// The window to advertise: the connection's own limit, within an equal
// share of what the NIC can hold (rx_window_max). With one connection
// that is the whole of it, exactly as before the pool.
//
// The NIC's receive buffer is ONE buffer for every connection, and a
// frame it cannot hold is lost along with everything behind it
// (docs/networking.md, "Throughput"). So the sum of the windows is what
// must fit, not each. The floor of one MSS means that stops being true
// once more connections are open than the buffer holds segments -- on
// RMII (1608 bytes) that is a fourth. Interactive traffic rarely has
// several peers sending at once, and when they do a lost frame costs a
// retransmit, not correctness.
static uint16_t window_for(const tcp_conn_t *c) {
	int n = tcp_count();
	uint32_t share = rx_window_max / (uint32_t)(n > 0 ? n : 1);
	if (share < TCP_ADVERTISE_MSS) share = TCP_ADVERTISE_MSS;
	return c->rx_window < share ? c->rx_window : (uint16_t)share;
}

static void slot_free(tcp_conn_t *c) {
#if TCP_REASSEMBLY
	if (ooo_conn == c) ooo_reset();
#endif
	c->state = TCP_CLOSED;
	c->tx_pending = false;
	c->fin_pending = false;
	c->handler = NULL;
	c->user = NULL;
	c->gen++;
}

// A slot for a new connection: a free one, or else the oldest in
// TIME_WAIT. Inbound never takes the last TCP_MIN_OUTBOUND_FREE.
static tcp_conn_t *alloc(bool inbound) {
	tcp_conn_t *freec = NULL, *tw = NULL, *c;
	int nfree = 0;

	for (int i = 0; i < TCP_MAX_CONN; i++) {
		c = &conns[i];
		if (c->state == TCP_CLOSED) {
			nfree++;
			if (!freec) freec = c;
		} else if (c->state == TCP_TIME_WAIT) {
			nfree++;
			if (!tw || (int32_t)(c->time_wait_start - tw->time_wait_start) < 0) tw = c;
		}
	}
	if (inbound && nfree <= TCP_MIN_OUTBOUND_FREE) return NULL;
	c = freec ? freec : tw;
	if (!c) return NULL;
	if (c->state != TCP_CLOSED) slot_free(c);

	c->inbound = inbound;
	c->half_close = false;
	c->rx_window = rx_window_max;
	c->snd_mss = TCP_MAX_PAYLOAD;       // RFC 879's default until the peer says
	c->retries = 0;
	return c;
}

static tcp_conn_t *find(uint32_t ip, uint16_t rport, uint16_t lport) {
	for (int i = 0; i < TCP_MAX_CONN; i++) {
		tcp_conn_t *c = &conns[i];
		if (c->state != TCP_CLOSED && c->remote_ip == ip &&
				c->remote_port == rport && c->local_port == lport)
			return c;
	}
	return NULL;
}

// Not a cryptographic nonce, but not predictable either: a guessable
// initial sequence number lets someone off the path inject data into a
// connection, which started to matter the day this stack began to
// accept connections from the network.
static uint32_t new_isn(void) {
	uint32_t v;
	z_rng_bytes(&v, sizeof(v));
	return v;
}

// A still-live handle: the slot has not been freed (and perhaps reused)
// since `gen` was read.
static bool still(const tcp_conn_t *c, uint32_t gen) {
	return c->gen == gen && c->state != TCP_CLOSED;
}

void tcp_init(uint32_t ip) {
	our_ip = ip;
	for (int i = 0; i < TCP_MAX_CONN; i++) {
		conns[i].state = TCP_CLOSED;
		conns[i].tx_pending = false;
		conns[i].handler = NULL;
	}
	ooo_reset();

	// seeded from boot-time ticks rather than a fixed 49152 every time:
	// a fixed start meant every connection to the same server:port,
	// across every reboot, reused the same 4-tuple, and a server still
	// holding state for it answered each SYN with a challenge ACK
	// instead of a SYN-ACK. The real-hardware symptom was a handshake
	// that never completed.
	next_local_port = (uint16_t)(49152 + (z_uptime_ticks() % 16384));
}

// -- sending --

// Builds and sends one segment for any 4-tuple -- a connection's, or a
// RST to a port nothing is listening on.
static bool send_raw(uint32_t rip, uint16_t lport, uint16_t rport, uint32_t seq,
		uint32_t ack, uint8_t flags, uint16_t window, const uint8_t *data, uint16_t len) {

	static uint8_t pkt[TCP_HDR_LEN + 4 + TCP_MAX_PAYLOAD];
	uint16_t opt_len = 0;

	pkt[0] = (uint8_t)(lport >> 8); pkt[1] = (uint8_t)lport;
	pkt[2] = (uint8_t)(rport >> 8); pkt[3] = (uint8_t)rport;
	pkt[4] = (uint8_t)(seq >> 24); pkt[5] = (uint8_t)(seq >> 16);
	pkt[6] = (uint8_t)(seq >> 8);  pkt[7] = (uint8_t)seq;
	pkt[8] = (uint8_t)(ack >> 24); pkt[9] = (uint8_t)(ack >> 16);
	pkt[10] = (uint8_t)(ack >> 8); pkt[11] = (uint8_t)ack;

	// A SYN carries an MSS option; nothing else carries any option. It
	// must never exceed TCP_MAX_RX_PAYLOAD, the largest segment this
	// stack delivers to a listener whole. See tcp.h.
	if (flags & TCP_FLAG_SYN) {
		pkt[12] = (6 << 4);
		opt_len = 4;
	} else {
		pkt[12] = (5 << 4);
	}
	pkt[13] = flags;
	pkt[14] = (uint8_t)(window >> 8);
	pkt[15] = (uint8_t)window;
	pkt[16] = 0; pkt[17] = 0;       // checksum, below
	pkt[18] = 0; pkt[19] = 0;       // urgent pointer

	if (opt_len) {
		pkt[20] = 2;                // maximum segment size
		pkt[21] = 4;
		pkt[22] = (TCP_ADVERTISE_MSS >> 8) & 0xFF;
		pkt[23] = TCP_ADVERTISE_MSS & 0xFF;
	}
	if (len) memcpy(pkt + TCP_HDR_LEN + opt_len, data, len);

	uint16_t seg_len = (uint16_t)(TCP_HDR_LEN + opt_len + len);
	uint16_t csum = tcp_checksum(our_ip, rip, pkt, seg_len);
	pkt[16] = (uint8_t)(csum >> 8);
	pkt[17] = (uint8_t)csum;

	return ip_send(rip, 6, pkt, seg_len);
}

static bool seg_send(tcp_conn_t *c, uint32_t seq, uint8_t flags,
		const uint8_t *data, uint16_t len) {
	return send_raw(c->remote_ip, c->local_port, c->remote_port, seq,
		c->rcv_nxt, flags, window_for(c), data, len);
}

// Sends a new segment and arms it for retransmission -- anything that
// consumes a sequence number (SYN, FIN, data). Caller has checked
// !c->tx_pending.
static void send_tracked(tcp_conn_t *c, uint8_t flags, const uint8_t *data, uint16_t len) {
	uint32_t seq = c->snd_nxt, consumed = len;

	seg_send(c, seq, flags, data, len);
	c->tx_pending = true;
	c->tx_seq = seq;
	c->tx_flags = flags;
	c->tx_len = len;
	if (len) memcpy(c->tx_buf, data, len);
	c->last_send_tick = z_uptime_ticks();
	c->retries = 0;

	if (flags & TCP_FLAG_SYN) consumed++;
	if (flags & TCP_FLAG_FIN) consumed++;
	c->snd_nxt += consumed;
}

// A pure ACK -- never retransmitted.
static void send_ack(tcp_conn_t *c) {
	seg_send(c, c->snd_nxt, TCP_FLAG_ACK, NULL, 0);
}

// The RST that RFC 793 sends for a segment belonging to no connection.
// Never in answer to a RST.
static void reset_reply(uint32_t ip, uint16_t lport, uint16_t rport, uint32_t seq,
		uint32_t ack, uint8_t flags, uint16_t data_len) {
	if (flags & TCP_FLAG_RST) return;
	if (flags & TCP_FLAG_ACK) {
		send_raw(ip, lport, rport, ack, 0, TCP_FLAG_RST, 0, NULL, 0);
	} else {
		uint32_t n = seq + data_len + ((flags & TCP_FLAG_SYN) ? 1 : 0) +
			((flags & TCP_FLAG_FIN) ? 1 : 0);
		send_raw(ip, lport, rport, 0, n, TCP_FLAG_RST | TCP_FLAG_ACK, 0, NULL, 0);
	}
}

// The peer's MSS option, or 0 if its SYN carried none.
static uint16_t parse_mss(const uint8_t *p, uint8_t data_offset) {
	uint8_t i = TCP_HDR_LEN;
	while (i < data_offset) {
		uint8_t kind = p[i];
		if (kind == 0) break;                   // end of options
		if (kind == 1) { i++; continue; }       // no-op
		if (i + 1 >= data_offset) break;
		uint8_t l = p[i + 1];
		if (l < 2 || i + l > data_offset) break;
		if (kind == 2 && l == 4) return (uint16_t)((p[i + 2] << 8) | p[i + 3]);
		i = (uint8_t)(i + l);
	}
	return 0;
}

static void take_mss(tcp_conn_t *c, const uint8_t *p, uint8_t data_offset) {
	uint16_t m = parse_mss(p, data_offset);
	if (m >= 64 && m < c->snd_mss) c->snd_mss = m;
}

// -- the API --

tcp_conn_t *tcp_connect(uint32_t dst_ip, uint16_t dst_port,
		tcp_event_handler_t handler, void *user) {

	tcp_conn_t *c = alloc(false);
	uint32_t isn;
	int tries;

	if (!c) return NULL;

	// A local port no live connection is using.
	for (tries = 0; tries < 16; tries++) {
		uint16_t p = next_local_port++;
		if (next_local_port < 49152) next_local_port = 49152;  // wrap, avoid 0
		bool used = false;
		for (int i = 0; i < TCP_MAX_CONN; i++)
			if (conns[i].state != TCP_CLOSED && conns[i].local_port == p) used = true;
		if (!used) { c->local_port = p; break; }
	}

	c->remote_ip = dst_ip;
	c->remote_port = dst_port;
	isn = new_isn();
	c->snd_una = isn;
	c->snd_nxt = isn;
	c->rcv_nxt = 0;
	c->handler = handler;
	c->user = user;
	c->state = TCP_SYN_SENT;
	send_tracked(c, TCP_FLAG_SYN, NULL, 0);
	return c;
}

bool tcp_listen(uint16_t port, tcp_accept_t accept) {
	int i, slot = -1;
	if (!port || !accept) return false;
	for (i = 0; i < TCP_MAX_LISTEN; i++) {
		if (listens[i].port == port) return false;
		if (!listens[i].port && slot < 0) slot = i;
	}
	if (slot < 0) return false;
	listens[slot].port = port;
	listens[slot].accept = accept;
	return true;
}

void tcp_unlisten(uint16_t port) {
	for (int i = 0; i < TCP_MAX_LISTEN; i++)
		if (listens[i].port == port) { listens[i].port = 0; listens[i].accept = NULL; }
}

static tcp_accept_t listener(uint16_t port) {
	for (int i = 0; i < TCP_MAX_LISTEN; i++)
		if (listens[i].port == port) return listens[i].accept;
	return NULL;
}

bool tcp_is_connected(const tcp_conn_t *c) { return c && c->state == TCP_ESTABLISHED; }
bool tcp_can_send(const tcp_conn_t *c) {
	return c && (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT);
}
void tcp_set_half_close(tcp_conn_t *c, bool on) { c->half_close = on; }
void *tcp_user(const tcp_conn_t *c) { return c->user; }
uint32_t tcp_remote_ip(const tcp_conn_t *c) { return c->remote_ip; }
uint16_t tcp_remote_port(const tcp_conn_t *c) { return c->remote_port; }
uint16_t tcp_local_port(const tcp_conn_t *c) { return c->local_port; }
bool tcp_is_inbound(const tcp_conn_t *c) { return c->inbound; }
uint16_t tcp_mss(const tcp_conn_t *c) { return c->snd_mss; }

bool tcp_send(tcp_conn_t *c, const uint8_t *data, uint16_t len) {
	if (!tcp_can_send(c)) return false;
	if (len == 0) return true;          // nothing to send: not an empty segment
	if (c->tx_pending) return false;
	if (len > c->snd_mss) return false;
	send_tracked(c, TCP_FLAG_ACK | TCP_FLAG_PSH, data, len);
	return true;
}

bool tcp_close(tcp_conn_t *c) {
	if (!tcp_can_send(c)) return false;
	if (c->tx_pending) return false;
	send_tracked(c, TCP_FLAG_ACK | TCP_FLAG_FIN, NULL, 0);
	// From CLOSE_WAIT the peer's FIN is already in: only ours to be acked.
	c->state = (c->state == TCP_CLOSE_WAIT) ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
	return true;
}

// A bare ACK carrying the current window: needed when the window
// REOPENS, since a peer told zero waits for an update and nothing else
// will produce one.
void tcp_ack_now(tcp_conn_t *c) {
	if (c && c->state == TCP_ESTABLISHED) send_ack(c);
}

void tcp_abort(tcp_conn_t *c) {
	if (!c || c->state == TCP_CLOSED) return;
	if (c->state != TCP_TIME_WAIT)
		seg_send(c, c->snd_nxt, TCP_FLAG_RST, NULL, 0);
	slot_free(c);
}

// CLOSED is the last event: the handler is cleared BEFORE it is
// called, so nothing reaches it after, and a handler that aborts or
// reopens from inside it does no harm.
static void notify(tcp_conn_t *c, tcp_event_t ev, const uint8_t *data, uint16_t len) {
	tcp_event_handler_t h = c->handler;
	if (!h) return;
	if (ev == TCP_EVENT_CLOSED) c->handler = NULL;
	h(c, ev, data, len);
}

// Clears the outstanding segment if `ack_num` fully covers it. false
// for a partial or stale ack.
static bool process_ack(tcp_conn_t *c, uint32_t ack_num) {
	uint32_t consumed;
	if (!c->tx_pending) return false;
	consumed = c->tx_len;
	if (c->tx_flags & TCP_FLAG_SYN) consumed++;
	if (c->tx_flags & TCP_FLAG_FIN) consumed++;
	if (ack_num - c->tx_seq < consumed) return false;
	c->snd_una = c->tx_seq + consumed;
	c->tx_pending = false;
	return true;
}

// The peer's FIN, in order, on a connection that was ESTABLISHED. No
// half-close (tcp.h): our own FIN goes straight back, or as soon as our
// outstanding segment is acked.
static void peer_fin(tcp_conn_t *c) {
	c->rcv_nxt++;
	send_ack(c);
	if (c->half_close) {
		// Only the peer's half is over. CLOSED comes when ours is.
		c->state = TCP_CLOSE_WAIT;
		notify(c, TCP_EVENT_EOF, NULL, 0);
		return;
	}
	if (!c->tx_pending) send_tracked(c, TCP_FLAG_ACK | TCP_FLAG_FIN, NULL, 0);
	else c->fin_pending = true;
	c->state = TCP_LAST_ACK;
	notify(c, TCP_EVENT_CLOSED, NULL, 0);
}

// Data and FIN on an established connection.
static void established(tcp_conn_t *c, uint32_t seq, uint32_t ack, uint8_t flags,
		const uint8_t *data, uint16_t data_len) {

	uint32_t gen = c->gen;

	if (flags & TCP_FLAG_ACK) process_ack(c, ack);

	if (data_len > 0) {
		if (seq == c->rcv_nxt) {
			rx_in_order++;
			c->rcv_nxt += data_len;
			// At most TCP_MAX_RX_PAYLOAD to the listener, though rcv_nxt
			// covers the whole segment: see tcp.h's TCP_MAX_RX_PAYLOAD
			// for the real-hardware overflow that closed.
			notify(c, TCP_EVENT_DATA, data,
				data_len > TCP_MAX_RX_PAYLOAD ? TCP_MAX_RX_PAYLOAD : data_len);
			// The handler may have aborted, or reopened the slot.
			if (!still(c, gen)) return;
			// The ACK goes AFTER the listener has the data, so the
			// window it carries is the one that is true now. Sent first,
			// it advertised room the listener had just used up, the peer
			// filled it, and the listener -- net's relay, with a fixed
			// buffer -- had to drop bytes TCP had already acked: lost
			// for good. Found as 2488 of 3000 bytes echoed.
			// (Not when a FIN rides on the same segment: peer_fin()'s
			// ACK below covers both, and one is enough.)
			if (!(flags & TCP_FLAG_FIN)) send_ack(c);
#if TCP_REASSEMBLY
			ooo_drain(c);
			if (!still(c, gen)) return;
#endif
		} else if ((int32_t)(seq - c->rcv_nxt) < 0) {
			// Already seen: re-ack, deliver nothing. A SIGNED
			// difference, not `seq < rcv_nxt` -- sequence numbers wrap,
			// and an initial sequence number near the top of the range
			// is as likely as any other.
			rx_dup++;
			send_ack(c);
		} else {
			// A gap. An immediate duplicate ACK, as RFC 5681 asks, so
			// the peer fast-retransmits instead of waiting a full RTO.
			rx_gap++;
#if TCP_REASSEMBLY
			ooo_store(c, seq, data, data_len);
#endif
			send_ack(c);
		}
	}

	// Only an in-order FIN; one beyond a gap waits for the retransmit.
	if ((flags & TCP_FLAG_FIN) && seq + data_len == c->rcv_nxt && c->state == TCP_ESTABLISHED)
		peer_fin(c);
}

void tcp_handle(uint32_t src_ip, const uint8_t *p, uint16_t len) {

	// Entropy: WHEN a segment arrives depends on the remote machine,
	// the network and the NIC, none of which this board can predict.
	// Before the early returns on purpose -- a stray segment is as
	// good a timing sample as a valid one.
	z_rng_stir_event();

	if (len < TCP_HDR_LEN) return;

	uint16_t src_port = (uint16_t)((p[0] << 8) | p[1]);
	uint16_t dst_port = (uint16_t)((p[2] << 8) | p[3]);
	uint32_t seq = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
		((uint32_t)p[6] << 8) | p[7];
	uint32_t ack = ((uint32_t)p[8] << 24) | ((uint32_t)p[9] << 16) |
		((uint32_t)p[10] << 8) | p[11];
	uint8_t data_offset = (uint8_t)((p[12] >> 4) * 4);
	uint8_t flags = p[13];

	if (data_offset < TCP_HDR_LEN || data_offset > len) return;   // malformed

	const uint8_t *data = p + data_offset;
	uint16_t data_len = (uint16_t)(len - data_offset);

	tcp_conn_t *c = find(src_ip, src_port, dst_port);

	// A new SYN for the 4-tuple of a connection in TIME_WAIT reopens
	// it (RFC 1122 4.2.2.13): the old one is over.
	if (c && c->state == TCP_TIME_WAIT && (flags & TCP_FLAG_SYN) &&
			!(flags & TCP_FLAG_ACK) && listener(dst_port)) {
		slot_free(c);
		c = NULL;
	}

	if (!c) {
		if ((flags & TCP_FLAG_SYN) && !(flags & (TCP_FLAG_ACK | TCP_FLAG_RST))) {
			tcp_accept_t accept = listener(dst_port);
			if (accept && (c = alloc(true)) != NULL) {
				void *user = NULL;
				tcp_event_handler_t h;
				uint32_t isn = new_isn();
				c->remote_ip = src_ip;
				c->remote_port = src_port;
				c->local_port = dst_port;
				c->rcv_nxt = seq + 1;
				c->snd_una = isn;
				c->snd_nxt = isn;
				take_mss(c, p, data_offset);
				h = accept(c, dst_port, src_ip, src_port, &user);
				if (h) {
					c->handler = h;
					c->user = user;
					c->state = TCP_SYN_RCVD;
					send_tracked(c, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
					return;
				}
				slot_free(c);
			}
		}
		// No connection, no listener, no slot, or refused.
		reset_reply(src_ip, dst_port, src_port, seq, ack, flags, data_len);
		return;
	}

	// RST. Believed only at EXACTLY the next sequence number (RFC 5961
	// 3.2); anything else gets a challenge ACK, which a genuine peer
	// answers with a RST that does match. A connection that accepts
	// any in-window RST can be torn down by anyone who guesses a port.
	// In SYN_SENT a RST counts only if it acks our SYN.
	if (flags & TCP_FLAG_RST) {
		if (c->state == TCP_SYN_SENT) {
			if (!(flags & TCP_FLAG_ACK) || ack != c->snd_nxt) return;
		} else if (seq != c->rcv_nxt) {
			if (c->state != TCP_TIME_WAIT) send_ack(c);
			return;
		}
		// notify first, then free -- the other order silently lost the
		// CLOSED (a real bug, once: term waited out its own timeout).
		uint32_t gen = c->gen;
		notify(c, TCP_EVENT_CLOSED, NULL, 0);
		if (still(c, gen)) slot_free(c);
		return;
	}

	switch (c->state) {

	case TCP_SYN_SENT: {
		if (!(flags & TCP_FLAG_SYN) || !(flags & TCP_FLAG_ACK)) return;
		if (!process_ack(c, ack)) return;
		c->rcv_nxt = seq + 1;
		take_mss(c, p, data_offset);
		c->state = TCP_ESTABLISHED;
		send_ack(c);
		notify(c, TCP_EVENT_ESTABLISHED, NULL, 0);
		break;
	}

	case TCP_SYN_RCVD: {
		// Their SYN again: our SYN-ACK was lost. Send it again now
		// rather than waiting for the timer.
		if ((flags & TCP_FLAG_SYN) && seq + 1 == c->rcv_nxt) {
			seg_send(c, c->tx_seq, c->tx_flags, NULL, 0);
			return;
		}
		if (!(flags & TCP_FLAG_ACK) || !process_ack(c, ack)) return;
		c->state = TCP_ESTABLISHED;
		{
			uint32_t gen = c->gen;
			notify(c, TCP_EVENT_ESTABLISHED, NULL, 0);
			if (!still(c, gen)) return;
		}
		// The handshake's last ACK may carry data, or even a FIN.
		established(c, seq, ack, flags, data, data_len);
		break;
	}

	case TCP_ESTABLISHED:
		established(c, seq, ack, flags, data, data_len);
		break;

	case TCP_FIN_WAIT_1: {
		bool our_fin_acked = (flags & TCP_FLAG_ACK) && process_ack(c, ack);
		if (flags & TCP_FLAG_FIN) {
			c->rcv_nxt = seq + data_len + 1;
			send_ack(c);
			c->state = our_fin_acked ? TCP_TIME_WAIT : TCP_CLOSING;
			if (c->state == TCP_TIME_WAIT) c->time_wait_start = z_uptime_ticks();
			// Both FINs are through: the connection is over for its
			// owner, whoever closed first. This close order used to end
			// with no CLOSED at all -- nothing closed gracefully until
			// net's relay (relay.c), which leaked a relay per session.
			notify(c, TCP_EVENT_CLOSED, NULL, 0);
		} else if (our_fin_acked) {
			c->state = TCP_FIN_WAIT_2;
		}
		break;
	}

	case TCP_FIN_WAIT_2:
		if (flags & TCP_FLAG_FIN) {
			c->rcv_nxt = seq + data_len + 1;
			send_ack(c);
			c->state = TCP_TIME_WAIT;
			c->time_wait_start = z_uptime_ticks();
			notify(c, TCP_EVENT_CLOSED, NULL, 0);   // see FIN_WAIT_1
		}
		break;

	case TCP_CLOSING:
		if ((flags & TCP_FLAG_ACK) && process_ack(c, ack)) {
			c->state = TCP_TIME_WAIT;
			c->time_wait_start = z_uptime_ticks();
		}
		break;

	case TCP_CLOSE_WAIT:
		// Half-closed: nothing more comes in, but our data is acked.
		if (flags & TCP_FLAG_ACK) process_ack(c, ack);
		if (flags & TCP_FLAG_FIN) send_ack(c);          // a retransmitted FIN
		break;

	case TCP_LAST_ACK:
		if ((flags & TCP_FLAG_ACK) && process_ack(c, ack)) {
			if (c->fin_pending) {
				// that was our data; now the FIN it was holding up
				c->fin_pending = false;
				send_tracked(c, TCP_FLAG_ACK | TCP_FLAG_FIN, NULL, 0);
			} else {
				// Done. After a half-close this is the owner's CLOSED;
				// otherwise it had CLOSED already and this is a no-op.
				uint32_t gen = c->gen;
				notify(c, TCP_EVENT_CLOSED, NULL, 0);
				if (still(c, gen)) slot_free(c);
			}
		}
		break;

	case TCP_TIME_WAIT:
		// a retransmit of the peer's FIN: re-ack it
		if (flags & TCP_FLAG_FIN) send_ack(c);
		break;

	default:
		break;
	}
}

void tcp_poll(void) {

	uint32_t now = z_uptime_ticks();

	for (int i = 0; i < TCP_MAX_CONN; i++) {
		tcp_conn_t *c = &conns[i];

		if (c->state == TCP_CLOSED) continue;

		if (c->state == TCP_TIME_WAIT) {
			if (now - c->time_wait_start >= TCP_TIME_WAIT_TICKS) slot_free(c);
			continue;
		}

		if (!c->tx_pending) continue;

		uint32_t rto = (uint32_t)TCP_RTO_TICKS_BASE <<
			(c->retries < TCP_RTO_MAX_SHIFT ? c->retries : TCP_RTO_MAX_SHIFT);
		if (now - c->last_send_tick < rto) continue;

		if (c->retries >= (c->state == TCP_SYN_RCVD ? TCP_SYNACK_RETRIES : TCP_MAX_RETRIES)) {
			uint32_t gen = c->gen;
			if (c->state != TCP_SYN_RCVD)
				printf("tcp: giving up after %d retries, connection abandoned\n",
					TCP_MAX_RETRIES);
			// notify BEFORE freeing: freeing clears the handler
			notify(c, TCP_EVENT_CLOSED, NULL, 0);
			if (still(c, gen)) slot_free(c);
			continue;
		}

		seg_send(c, c->tx_seq, c->tx_flags, c->tx_buf, c->tx_len);
		c->last_send_tick = now;
		c->retries++;
	}
}
