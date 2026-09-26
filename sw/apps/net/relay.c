/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See relay.h. docs/netserve.md.
 *
 * -- one relay --
 *
 *   OFFERED   TCP is established; the listener has been sent CONNECT
 *             and has not answered. Bytes from the peer are buffered.
 *   OPEN      both sides connected; bytes flow.
 *   CLOSING   the listener closed: what it sent is still being written
 *             to TCP, and then the TCP connection is closed gracefully.
 *
 * A relay is taken when the SYN is accepted (a pool slot and a relay
 * slot both), so a connection is never established that could not be
 * relayed.
 *
 * -- backpressure, both ways --
 *
 * Peer to listener: bytes from TCP go into rx[]; the advertised window
 * is what is left of it, so a listener that falls behind (z_port_send()
 * refusing past 8 unacked sends) slows the peer down rather than losing
 * data. TCP has already acked what it delivered, so dropping is never
 * an option.
 *
 * Listener to peer: DATA goes into tx[] and is acked. When tx[] is full
 * the DATA is HELD, unacked -- its payload stays valid in the
 * listener's memory until acked (zport.h) -- and the listener, having 8
 * sends unacked, stops. Acks go out as held data moves into tx[].
 */
#include <stdio.h>
#include <string.h>

#include "relay.h"
#include "tcp.h"
#include "ip.h"
#include "../../common/zeitlos.h"
#include "../../common/zobj.h"
#include "../../common/zport.h"
#include "../../common/znet.h"
#include "../../common/zsoc.h"		// Z_TICK_HZ

#define RELAY_MAX   (TCP_MAX_CONN - TCP_MIN_OUTBOUND_FREE)
#define RELAY_TX    1024        // listener -> peer, awaiting tcp_send()
#define RELAY_RX    512         // peer -> listener, awaiting z_port_send()
#define RELAY_HOLD  Z_PORT_MAX_PENDING_SENDS

// R_DRAIN: TCP is done and the listener told, but sends to it are
// still unacked -- their payloads live in this process until they are
// (zport.h), so the slot waits for the acks, or a deadline, before it
// is reused.
enum { R_FREE = 0, R_OFFERED, R_OPEN, R_CLOSING, R_DRAIN };

typedef struct {
	uint8_t state;
	bool tcp_gone;              // TCP closed while OFFERED: close on the answer
	bool peer_eof;              // the peer's FIN is in (half-close)
	bool eof_told;              // ... and the listener has Z_NET_EOF
	tcp_conn_t *tcp;
	uint32_t owner;             // the listener's pid
	z_port_t port;              // to the listener, once it has answered

	z_net_accept_t info;        // what CONNECT carries; lives as long as the relay
	z_blob_t info_blob;

	uint8_t tx[RELAY_TX];
	uint16_t tx_len;
	uint8_t rx[RELAY_RX];
	uint16_t rx_len;
	uint16_t win_last;          // the window last advertised
	uint32_t active;            // tick anything last moved, either way (stall reports)

	// DATA not yet wholly copied into tx[], unacked; `off` is how much
	// has been. Consumed in pieces: one message may be larger than tx[]
	// (netserve.c, held_t, has the hardware story).
	struct { const uint8_t *data; uint32_t len, off, tag; } held[RELAY_HOLD];
	uint8_t held_n;
} relay_t;

static relay_t relays[RELAY_MAX];

static struct { uint16_t port; uint32_t owner; } listening[TCP_MAX_LISTEN];

// -- helpers --

static int relay_id(const relay_t *r) {
	return (int)(r - relays) + 1;
}

static relay_t *by_id(uint32_t id) {
	if (id < 1 || id > RELAY_MAX) return NULL;
	return relays[id - 1].state ? &relays[id - 1] : NULL;
}

int relay_count(void) {
	int n = 0;
	for (int i = 0; i < RELAY_MAX; i++) if (relays[i].state) n++;
	return n;
}

static bool is_listener(uint32_t pid) {
	for (int i = 0; i < TCP_MAX_LISTEN; i++)
		if (listening[i].port && listening[i].owner == pid) return true;
	for (int i = 0; i < RELAY_MAX; i++)
		if (relays[i].state && relays[i].owner == pid) return true;
	return false;
}

static void print_ip(uint32_t ip) {
	printf("%lu.%lu.%lu.%lu", (unsigned long)(ip >> 24), (unsigned long)((ip >> 16) & 0xFF),
		(unsigned long)((ip >> 8) & 0xFF), (unsigned long)(ip & 0xFF));
}

// Acks one DATA message the listener sent: all z_port_send_ack() needs
// is the sender, the tag and that it was a blob.
static void ack_data(uint32_t from, uint32_t tag) {
	z_msg_t m;
	m.from = from;
	m.tag = tag;
	m.obj.type = Z_BLOB;
	z_port_send_ack(&m);
}

static void relay_free(relay_t *r);

// The end of a relay whose listener may still hold its sends: wait for
// the acks first (relay_poll()).
static void relay_done(relay_t *r) {
	if (!r->port.pending_count) { relay_free(r); return; }
	r->state = R_DRAIN;
	r->tcp = NULL;
	r->active = z_uptime_ticks();
}

static void relay_free(relay_t *r) {
	// Acks for anything still held: the listener must be able to free
	// the payloads, whatever became of the connection.
	for (int i = 0; i < r->held_n; i++) ack_data(r->owner, r->held[i].tag);
	memset(r, 0, sizeof(*r));
}

// -- TCP events --

static void on_tcp(tcp_conn_t *c, tcp_event_t ev, const uint8_t *data, uint16_t len) {
	relay_t *r = tcp_user(c);
	if (!r) return;

	switch (ev) {

	case TCP_EVENT_ESTABLISHED: {
		uint32_t mask = ip_our_netmask();
		r->info.ip = tcp_remote_ip(c);
		r->info.port = tcp_remote_port(c);
		r->info.lport = tcp_local_port(c);
		r->info.flags = (mask && ((r->info.ip ^ ip_our_addr()) & mask) == 0) ?
			Z_NET_ACCEPT_LOCAL : 0;
		r->info_blob.len = sizeof(r->info);
		r->info_blob.data = (uint8_t *)&r->info;
		z_obj_t o;
		o.type = Z_BLOB;
		o.val.ptr = &r->info_blob;
		printf("net: connection from ");
		print_ip(r->info.ip);
		printf(":%u to port %u, relay %d\n", (unsigned)r->info.port,
			(unsigned)r->info.lport, relay_id(r));
		if (z_msg_new_send(r->owner, Z_PORT_CONNECT, (uint32_t)relay_id(r), o) != Z_OK) {
			printf("net: relay %d: the listener (pid %lu) is not taking messages\n",
				relay_id(r), (unsigned long)r->owner);
			tcp_abort(c);
			relay_free(r);
			return;
		}
		r->state = R_OFFERED;
		break;
	}

	case TCP_EVENT_DATA: {
		// The window keeps the peer inside rx[]; a peer that ignores
		// it loses the excess, which is TCP's rule, not ours.
		if (len > RELAY_RX - r->rx_len) {
			printf("net: relay %d: %u bytes past the window dropped\n", relay_id(r),
				(unsigned)(len - (RELAY_RX - r->rx_len)));
			len = (uint16_t)(RELAY_RX - r->rx_len);
		}
		memcpy(r->rx + r->rx_len, data, len);
		r->rx_len = (uint16_t)(r->rx_len + len);
		r->active = z_uptime_ticks();
		// The window shrinks NOW, before tcp.c sends the ACK for this
		// segment -- not at the next relay_poll(), by which time the
		// peer has been told there is room that there is not.
		uint16_t w = (uint16_t)(RELAY_RX - r->rx_len);
		if (w < 64) w = 0;
		tcp_set_rx_window(c, w);
		r->win_last = w;
		break;
	}

	case TCP_EVENT_EOF:
		// Told to the listener once what came before it has gone
		// there too (relay_poll()).
		r->peer_eof = true;
		r->active = z_uptime_ticks();
		break;

	case TCP_EVENT_CLOSED:
		r->tcp = NULL;
		if (r->state == R_OFFERED) {
			r->tcp_gone = true;         // the answer is still to come
		} else {
			if (r->state == R_OPEN) z_port_close(&r->port);
			relay_done(r);
		}
		break;
	}
}

static tcp_event_handler_t on_accept(tcp_conn_t *c, uint16_t lport, uint32_t ip,
		uint16_t rport, void **user) {
	uint32_t owner = 0;
	(void)c; (void)ip; (void)rport;
	for (int i = 0; i < TCP_MAX_LISTEN; i++)
		if (listening[i].port == lport) owner = listening[i].owner;
	if (!owner) return NULL;
	for (int i = 0; i < RELAY_MAX; i++) {
		relay_t *r = &relays[i];
		if (r->state) continue;
		memset(r, 0, sizeof(*r));
		// Not yet OFFERED -- that is when the listener hears of it --
		// but taken: state is never FREE while a TCB points here.
		r->state = R_OFFERED;
		r->tcp = c;
		r->owner = owner;
		// before the SYN-ACK goes out, so the peer's first burst
		// fits rx[] -- the default is the NIC's whole share
		tcp_set_rx_window(c, RELAY_RX);
		// A peer's FIN ends only its half: the listener's reply still
		// goes out (tcp.h, tcp_set_half_close()).
		tcp_set_half_close(c, true);
		r->win_last = RELAY_RX;
		*user = r;
		return on_tcp;
	}
	return NULL;                        // every relay busy: RST
}

// -- listening --

void relay_init(void) {
	memset(relays, 0, sizeof(relays));
	memset(listening, 0, sizeof(listening));
}

static void reply(const z_msg_t *msg, uint32_t err) {
	z_msg_new_send(msg->from, Z_NET_LISTEN_REPLY, msg->tag, z_obj_uint32(err));
}

void relay_listen(const z_msg_t *msg) {
	uint32_t port = msg->obj.type == Z_UINT32 ? msg->obj.val.uint32 : 0;
	int slot = -1;

	if (!port || port > 65535) { reply(msg, Z_NET_LISTEN_E_PORT); return; }
	if (!ip_our_addr()) { reply(msg, Z_NET_LISTEN_E_NOIP); return; }

	for (int i = 0; i < TCP_MAX_LISTEN; i++) {
		if (listening[i].port == port) { slot = i; break; }
		if (!listening[i].port && slot < 0) slot = i;
	}
	if (slot < 0) { reply(msg, Z_NET_LISTEN_E_FULL); return; }

	if (listening[slot].port == port && listening[slot].owner != msg->from) {
		// Taken over: a restarted listener reclaiming its port. The old
		// owner's connections go with it.
		uint32_t old = listening[slot].owner;
		printf("net: port %lu taken over from pid %lu by pid %lu\n",
			(unsigned long)port, (unsigned long)old, (unsigned long)msg->from);
		for (int i = 0; i < RELAY_MAX; i++)
			if (relays[i].state && relays[i].owner == old && relays[i].info.lport == port) {
				if (relays[i].tcp) tcp_abort(relays[i].tcp);
				relay_free(&relays[i]);
			}
	} else if (listening[slot].port != port) {
		if (!tcp_listen((uint16_t)port, on_accept)) { reply(msg, Z_NET_LISTEN_E_FULL); return; }
	}
	listening[slot].port = (uint16_t)port;
	listening[slot].owner = msg->from;
	printf("net: listening on port %lu for pid %lu\n", (unsigned long)port,
		(unsigned long)msg->from);
	reply(msg, 0);
}

void relay_unlisten(const z_msg_t *msg) {
	uint32_t port = msg->obj.type == Z_UINT32 ? msg->obj.val.uint32 : 0;
	for (int i = 0; i < TCP_MAX_LISTEN; i++)
		if (listening[i].port == port && listening[i].owner == msg->from) {
			tcp_unlisten((uint16_t)port);
			listening[i].port = 0;
			listening[i].owner = 0;
			printf("net: no longer listening on port %lu\n", (unsigned long)port);
		}
}

// -- messages from listeners --

static relay_t *by_conn(uint32_t from, uint32_t tag) {
	for (int i = 0; i < RELAY_MAX; i++) {
		relay_t *r = &relays[i];
		if (r->state >= R_OPEN && r->owner == from && r->port.conn_id == tag) return r;
	}
	return NULL;
}

bool relay_msg(const z_msg_t *msg) {
	relay_t *r;

	if (!is_listener(msg->from)) return false;

	switch (msg->subject) {

	case Z_PORT_CONNECTED:
		r = by_id(msg->tag);
		if (!r || r->state != R_OFFERED || r->owner != msg->from || msg->obj.type != Z_UINT32)
			return true;
		r->port.peer_pid = r->owner;
		r->port.conn_id = msg->obj.val.uint32;
		r->port.connected = true;
		r->state = R_OPEN;
		if (r->tcp_gone) {
			// the peer left while the listener was deciding
			z_port_close(&r->port);
			relay_free(r);
		}
		return true;

	case Z_PORT_REFUSED:
		r = by_id(msg->tag);
		if (!r || r->state != R_OFFERED || r->owner != msg->from) return true;
		printf("net: relay %d refused by the listener%s%s\n", relay_id(r),
			msg->obj.type == Z_STR ? ": " : "",
			msg->obj.type == Z_STR ? msg->obj.val.str : "");
		if (r->tcp) tcp_abort(r->tcp);
		relay_free(r);
		return true;

	case Z_PORT_DATA: {
		r = by_conn(msg->from, msg->tag);
		if (r) r->active = z_uptime_ticks();
		if (!r || msg->obj.type != Z_BLOB) {
			if (msg->obj.type == Z_BLOB) z_port_send_ack(msg);
			return true;
		}
		if (r->state == R_DRAIN) { z_port_send_ack(msg); return true; }   // nowhere to go
		uint32_t len = z_blob_len(&msg->obj), used = 0;
		const uint8_t *data = (const uint8_t *)z_blob_data(&msg->obj);
		if (r->state == R_OPEN && !r->held_n) {
			used = (uint32_t)(RELAY_TX - r->tx_len);
			if (used > len) used = len;
			memcpy(r->tx + r->tx_len, data, used);
			r->tx_len = (uint16_t)(r->tx_len + used);
		}
		if (used == len) {
			z_port_send_ack(msg);
		} else if (r->held_n < RELAY_HOLD) {
			r->held[r->held_n].data = data;
			r->held[r->held_n].len = len;
			r->held[r->held_n].off = used;
			r->held[r->held_n].tag = msg->tag;
			r->held_n++;
		} else {
			// more than zport lets a sender have outstanding: a
			// misbehaving listener. Ack it and say so.
			printf("net: relay %d: listener exceeded its send window\n", relay_id(r));
			z_port_send_ack(msg);
		}
		return true;
	}

	case Z_PORT_DATA_ACK:
		r = by_conn(msg->from, msg->tag);
		if (r) { z_port_handle_ack_closed(&r->port, msg); r->active = z_uptime_ticks(); }
		return true;

	case Z_PORT_CLOSE:
		r = by_conn(msg->from, msg->tag);
		if (r && r->state == R_OPEN) {
			r->port.connected = false;
			r->state = R_CLOSING;       // flush, then close (relay_poll)
		}
		return true;

	default:
		return false;                   // anything else is for net itself
	}
}

// -- moving bytes --

void relay_poll(void) {
	for (int i = 0; i < RELAY_MAX; i++) {
		relay_t *r = &relays[i];
		if (r->state == R_DRAIN) {
			if (!r->port.pending_count ||
					z_uptime_ticks() - r->active > 5u * Z_TICK_HZ)
				relay_free(r);
			continue;
		}
		if (!r->state || !r->tcp) continue;

		// listener -> peer
		if (r->tx_len && tcp_can_send(r->tcp)) {
			uint16_t mss = tcp_mss(r->tcp);
			uint16_t n = r->tx_len > mss ? mss : r->tx_len;
			if (tcp_send(r->tcp, r->tx, n)) {
				memmove(r->tx, r->tx + n, (size_t)(r->tx_len - n));
				r->tx_len = (uint16_t)(r->tx_len - n);
				r->active = z_uptime_ticks();
			}
		}
		while (r->held_n) {
			uint32_t left = r->held[0].len - r->held[0].off;
			uint32_t n = (uint32_t)(RELAY_TX - r->tx_len);
			if (n > left) n = left;
			memcpy(r->tx + r->tx_len, r->held[0].data + r->held[0].off, n);
			r->tx_len = (uint16_t)(r->tx_len + n);
			r->held[0].off += n;
			if (r->held[0].off < r->held[0].len) break;
			ack_data(r->owner, r->held[0].tag);
			memmove(&r->held[0], &r->held[1], sizeof(r->held[0]) * (size_t)(r->held_n - 1));
			r->held_n--;
		}

		// the listener closed: once all it sent has gone, close TCP.
		// (The tx/held test is belt and braces today: the lines above
		// always send first, and stop-and-wait refuses a FIN while a
		// segment is out -- the host test cannot tell it is missing.
		// It is for the day this function is reordered.)
		if (r->state == R_CLOSING && !r->tx_len && !r->held_n && tcp_can_send(r->tcp))
			tcp_close(r->tcp);          // retried until the last segment is acked

		// A stall report: something is waiting, in either direction,
		// and nothing has moved for 3 s. The whole state in one line:
		// where the bytes are, what the window says, what is unacked
		// on both sides.
		{
			uint32_t now = z_uptime_ticks();
			bool pend; uint8_t tries; uint16_t slen;
			tcp_debug(r->tcp, &pend, &tries, &slen);
			bool waiting = r->tx_len || r->held_n || r->rx_len || r->port.pending_count || pend;
			if (!waiting || !r->active) r->active = now;
			else if (now - r->active > 3u * Z_TICK_HZ) {
				uint32_t io = 0, dup = 0, gap = 0;
				tcp_stats(&io, &dup, &gap);
				printf("net: relay %d stalled: out %u buffered + %u msgs held; in %u buffered, "
					"window %u; %u sends to the listener unacked; tcp segment %s (%u bytes, "
					"%u retries); segments in %lu, dup %lu, gap %lu\n", relay_id(r),
					(unsigned)r->tx_len, (unsigned)r->held_n, (unsigned)r->rx_len,
					(unsigned)r->win_last, (unsigned)r->port.pending_count,
					pend ? "unacked" : "none", (unsigned)slen, (unsigned)tries,
					(unsigned long)io, (unsigned long)dup, (unsigned long)gap);
				r->active = now;
			}
		}

		// peer -> listener
		if (r->state == R_OPEN && r->rx_len &&
				z_port_send(&r->port, r->rx, r->rx_len) == Z_OK) {
			r->rx_len = 0;
			r->active = z_uptime_ticks();
		}

		// the peer's half-close, after the last of its bytes
		if (r->state == R_OPEN && r->peer_eof && !r->eof_told && !r->rx_len) {
			z_msg_new_send(r->owner, Z_NET_EOF, r->port.conn_id, z_obj_none());
			r->eof_told = true;
		}

		// The window: what rx[] has left, or zero below one segment's
		// worth -- and a reopening announced, since a peer told zero
		// waits for it.
		if (tcp_is_connected(r->tcp)) {
			uint16_t w = (uint16_t)(RELAY_RX - r->rx_len);
			if (w < 64) w = 0;
			if (w != r->win_last) {
				// Announced when it reopens from zero -- a peer told
				// zero waits for this -- or grows by half the buffer
				// (RFC 1122 4.2.3.3); smaller changes ride on the next
				// ACK.
				bool reopened = (r->win_last == 0 && w) ||
					(w > r->win_last && w - r->win_last >= RELAY_RX / 2);
				tcp_set_rx_window(r->tcp, w);
				r->win_last = w;
				if (reopened) tcp_ack_now(r->tcp);
			}
		}
	}
}
