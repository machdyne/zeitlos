/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host test for tcp.c's connection pool and listening side.
 *
 *   make test          (or: cc -std=gnu99 -Wall -I. -I../../common \
 *                         -o /tmp/t tests/test_tcp_pool.c && /tmp/t)
 *
 * INCLUDES tcp.c, with ip_send(), the clock and the RNG stubbed, and
 * plays the network: it builds the segments a peer would send, feeds
 * them to tcp_handle(), and parses every segment the stack sends back.
 * So what is tested is the handshake on the wire -- flags, sequence and
 * acknowledgement numbers, the MSS option, windows, RSTs -- not only
 * the state variables.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* -- platform stubs -- */

typedef struct {
	uint32_t ip;
	uint16_t sport, dport;
	uint32_t seq, ack;
	uint8_t flags;
	uint16_t window, mss;
	uint16_t len;
	uint8_t data[1600];
} seg_t;

static seg_t out[64];
static int nout;

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool ip_send(uint32_t ip, uint8_t proto, const uint8_t *p, uint16_t len) {
	seg_t *s;
	uint8_t off;
	(void)proto;
	if (nout >= 64) { printf("FATAL: too many segments\n"); return true; }
	s = &out[nout++];
	memset(s, 0, sizeof(*s));
	s->ip = ip;
	s->sport = be16(p);
	s->dport = be16(p + 2);
	s->seq = be32(p + 4);
	s->ack = be32(p + 8);
	off = (uint8_t)((p[12] >> 4) * 4);
	s->flags = p[13];
	s->window = be16(p + 14);
	if (off >= 24 && p[20] == 2 && p[21] == 4) s->mss = be16(p + 22);
	s->len = (uint16_t)(len - off);
	memcpy(s->data, p + off, s->len);
	return true;
}

static uint32_t now = 1000;
uint32_t z_uptime_ticks(void) { return now; }
static uint32_t rng = 0x12345678;
void z_rng_bytes(void *b, uint32_t n) {
	uint8_t *p = b;
	while (n--) { rng = rng * 1103515245u + 12345u; *p++ = (uint8_t)(rng >> 16); }
}
bool z_rng_secure(void) { return true; }
void z_rng_stir_event(void) { }

#include "../tcp.c"

/* -- checks -- */

static int fails, checks;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

#define F_FIN TCP_FLAG_FIN
#define F_SYN TCP_FLAG_SYN
#define F_RST TCP_FLAG_RST
#define F_PSH TCP_FLAG_PSH
#define F_ACK TCP_FLAG_ACK

#define US    0x0A000001u        /* 10.0.0.1 */
#define PEER  0x0A000002u        /* 10.0.0.2 */
#define PEER2 0x0A000003u

/* Builds a segment from the peer and hands it to the stack. */
static void rx(uint32_t ip, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack,
		uint8_t flags, uint16_t mss, const void *data, uint16_t len) {
	uint8_t p[1600];
	uint8_t off = mss ? 24 : 20;
	memset(p, 0, sizeof(p));
	p[0] = (uint8_t)(sport >> 8); p[1] = (uint8_t)sport;
	p[2] = (uint8_t)(dport >> 8); p[3] = (uint8_t)dport;
	p[4] = (uint8_t)(seq >> 24); p[5] = (uint8_t)(seq >> 16); p[6] = (uint8_t)(seq >> 8); p[7] = (uint8_t)seq;
	p[8] = (uint8_t)(ack >> 24); p[9] = (uint8_t)(ack >> 16); p[10] = (uint8_t)(ack >> 8); p[11] = (uint8_t)ack;
	p[12] = (uint8_t)((off / 4) << 4);
	p[13] = flags;
	p[14] = 0x20; p[15] = 0x00;
	if (mss) { p[20] = 2; p[21] = 4; p[22] = (uint8_t)(mss >> 8); p[23] = (uint8_t)mss; }
	if (len) memcpy(p + off, data, len);
	tcp_handle(ip, p, (uint16_t)(off + len));
}

static seg_t none;
static seg_t *last(void) { return nout ? &out[nout - 1] : &none; }

/* -- what the handlers saw -- */

typedef struct {
	int established, closed, data_events, eofs;
	uint8_t got[4096];
	uint32_t got_len;
	tcp_conn_t *c;
	int shrink_on_data;         // lowers its window as it takes data, as relay.c does
	int abort_on_data;          // re-entrancy: abort inside DATA
	int reopen_on_data;         // ... and open a new connection in the freed slot
} peer_t;

static peer_t peers[16];
static int accept_calls, accept_refuse;
static uint16_t accepted_lport;
static uint32_t accepted_ip;
static tcp_conn_t *reopened;

static void on_event(tcp_conn_t *c, tcp_event_t ev, const uint8_t *d, uint16_t n) {
	peer_t *pr = tcp_user(c);
	if (!pr) return;
	if (ev == TCP_EVENT_ESTABLISHED) pr->established++;
	if (ev == TCP_EVENT_CLOSED) { pr->closed++; pr->c = NULL; }
	if (ev == TCP_EVENT_EOF) pr->eofs++;
	if (ev == TCP_EVENT_DATA) {
		pr->data_events++;
		if (pr->got_len + n <= sizeof(pr->got)) { memcpy(pr->got + pr->got_len, d, n); pr->got_len += n; }
		if (pr->shrink_on_data) tcp_set_rx_window(c, 100);
		if (pr->abort_on_data) {
			tcp_abort(c);
			pr->c = NULL;
			if (pr->reopen_on_data) reopened = tcp_connect(PEER2, 80, on_event, &peers[15]);
		}
	}
}

static int next_peer;
static tcp_event_handler_t on_accept(tcp_conn_t *c, uint16_t lport, uint32_t ip,
		uint16_t rport, void **user) {
	(void)rport;
	accept_calls++;
	accepted_lport = lport;
	accepted_ip = ip;
	if (accept_refuse) return NULL;
	peer_t *pr = &peers[next_peer++ % 15];
	memset(pr, 0, sizeof(*pr));
	pr->c = c;
	*user = pr;
	return on_event;
}

static void reset_all(void) {
	memset(conns, 0, sizeof(conns));
	memset(listens, 0, sizeof(listens));
	memset(peers, 0, sizeof(peers));
	tcp_init(US);
	tcp_set_rx_window_max(5360);
	nout = 0;
	next_peer = 0;
	accept_calls = accept_refuse = 0;
	reopened = NULL;
}

/* A passive open on port 23 from (ip, rport), completed. Returns the
 * stack's ISN + 1 (the peer's ack) via *our_seq. */
static peer_t *open_in(uint32_t ip, uint16_t rport, uint32_t peer_isn, uint32_t *our_seq) {
	int before = next_peer;
	nout = 0;
	rx(ip, rport, 23, peer_isn, 0, F_SYN, 1460, NULL, 0);
	if (next_peer == before || !nout || !(last()->flags & F_SYN)) return NULL;
	uint32_t s = last()->seq + 1;
	rx(ip, rport, 23, peer_isn + 1, s, F_ACK, 0, NULL, 0);
	if (our_seq) *our_seq = s;
	return &peers[before % 15];
}

int main(void) {

	/* -- 1. active open: SYN, MSS both ways -- */
	reset_all();
	{
		peer_t *pr = &peers[0];
		tcp_conn_t *c = tcp_connect(PEER, 80, on_event, pr);
		CK(c && nout == 1, "connect sends a SYN");
		seg_t *s = last();
		CK(s->flags == F_SYN && s->mss == TCP_ADVERTISE_MSS && s->dport == 80 &&
			s->sport >= 49152, "SYN: flags %02x mss %u ports %u->%u", s->flags, s->mss, s->sport, s->dport);
		uint32_t isn = s->seq;
		rx(PEER, 80, s->sport, 7000, isn + 1, F_SYN | F_ACK, 400, NULL, 0);
		CK(pr->established == 1 && tcp_is_connected(c), "established on SYN-ACK");
		CK(last()->flags == F_ACK && last()->ack == 7001, "handshake ACK");
		CK(tcp_mss(c) == 400, "the peer's smaller MSS is taken (%u)", tcp_mss(c));
		int before = nout;
		CK(tcp_send(c, (const uint8_t *)"x", 0) && nout == before, "a zero-length send sends nothing");
		uint8_t big[536]; memset(big, 'b', sizeof(big));
		CK(!tcp_send(c, big, 401), "a send over the peer's MSS is refused");
		CK(tcp_send(c, big, 400), "a send at the MSS is accepted");
		CK(!tcp_is_inbound(c), "outbound");
	}

	/* -- 2. passive open, data both ways, the peer closes -- */
	reset_all();
	CK(tcp_listen(23, on_accept), "listen on 23");
	CK(!tcp_listen(23, on_accept), "a second listen on 23 is refused");
	nout = 0;
	rx(PEER, 5000, 23, 1000, 0, F_SYN, 1460, NULL, 0);
	CK(accept_calls == 1 && accepted_lport == 23 && accepted_ip == PEER, "accept called with the right tuple");
	seg_t *sa = last();
	CK(sa && sa->flags == (F_SYN | F_ACK) && sa->ack == 1001 && sa->mss == TCP_ADVERTISE_MSS &&
		sa->sport == 23 && sa->dport == 5000, "SYN-ACK: flags %02x ack %u mss %u",
		sa ? sa->flags : 0, sa ? sa->ack : 0, sa ? sa->mss : 0);
	CK(peers[0].established == 0, "not established before the handshake's ACK");
	uint32_t s0 = sa->seq + 1;
	rx(PEER, 5000, 23, 1001, s0, F_ACK | F_PSH, 0, "hello", 5);
	peer_t *p0 = &peers[0];
	CK(p0->established == 1, "established by the ACK");
	CK(p0->got_len == 5 && !memcmp(p0->got, "hello", 5), "data riding on the handshake's ACK delivered");
	CK(last()->ack == 1006, "and acked");
	CK(tcp_is_inbound(p0->c) && tcp_remote_port(p0->c) == 5000 && tcp_local_port(p0->c) == 23, "accessors");
	CK(tcp_send(p0->c, (const uint8_t *)"login: ", 7) && last()->len == 7 && last()->seq == s0,
		"send from the server side");
	rx(PEER, 5000, 23, 1006, s0 + 7, F_ACK | F_FIN, 0, NULL, 0);
	CK(p0->closed == 1, "peer FIN: CLOSED");
	CK(last()->flags == (F_ACK | F_FIN) && last()->seq == s0 + 7, "and our FIN straight back");
	CK(tcp_count() == 1, "the slot is held in LAST_ACK");
	rx(PEER, 5000, 23, 1007, s0 + 8, F_ACK, 0, NULL, 0);
	CK(tcp_count() == 0 && conns[0].state == TCP_CLOSED, "freed once our FIN is acked");

	/* -- 3. RSTs -- */
	reset_all();
	nout = 0;
	rx(PEER, 5000, 99, 500, 0, F_SYN, 0, NULL, 0);
	CK(nout == 1 && last()->flags == (F_RST | F_ACK) && last()->ack == 501 && last()->seq == 0,
		"SYN to a closed port: RST|ACK acking it");
	nout = 0;
	rx(PEER, 5000, 99, 500, 8888, F_ACK, 0, "zz", 2);
	CK(nout == 1 && last()->flags == F_RST && last()->seq == 8888, "stray ACK: RST at its ack");
	nout = 0;
	rx(PEER, 5000, 99, 500, 0, F_RST, 0, NULL, 0);
	CK(nout == 0, "a RST is never answered");
	tcp_listen(23, on_accept);
	accept_refuse = 1;
	nout = 0;
	rx(PEER, 5000, 23, 500, 0, F_SYN, 0, NULL, 0);
	CK(accept_calls == 1 && nout == 1 && (last()->flags & F_RST) && tcp_count() == 0,
		"a refused accept: RST, slot freed");
	accept_refuse = 0;

	/* -- 4. the pool and the outbound reservation -- */
	reset_all();
	tcp_listen(23, on_accept);
	{
		int ok = 0;
		for (int i = 0; i < TCP_MAX_CONN; i++)
			if (open_in(PEER, (uint16_t)(6000 + i), 100u * (uint32_t)i + 1, NULL)) ok++;
		CK(ok == TCP_MAX_CONN - TCP_MIN_OUTBOUND_FREE, "inbound stops at %d (got %d)",
			TCP_MAX_CONN - TCP_MIN_OUTBOUND_FREE, ok);
		CK(last()->flags & F_RST, "the next inbound SYN is refused with a RST");
		tcp_conn_t *a = tcp_connect(PEER2, 80, on_event, NULL);
		tcp_conn_t *b = tcp_connect(PEER2, 81, on_event, NULL);
		tcp_conn_t *x = tcp_connect(PEER2, 82, on_event, NULL);
		CK(a && b && !x, "outbound still gets its %d slots, then none", TCP_MIN_OUTBOUND_FREE);
		CK(a && b && tcp_local_port(a) != tcp_local_port(b), "distinct local ports");
	}

	/* -- 5. window sharing -- */
	reset_all();
	tcp_listen(23, on_accept);
	{
		peer_t *a = open_in(PEER, 7000, 1, NULL);
		nout = 0;
		tcp_ack_now(a->c);
		CK(last()->window == 5360, "one connection: the whole window (%u)", last()->window);
		peer_t *b = open_in(PEER, 7001, 1, NULL);
		nout = 0;
		tcp_ack_now(a->c);
		uint16_t wa = last()->window;
		tcp_ack_now(b->c);
		uint16_t wb = last()->window;
		CK(wa + wb <= 5360 && wa >= TCP_ADVERTISE_MSS && wb >= TCP_ADVERTISE_MSS,
			"two share it: %u + %u", wa, wb);
		tcp_set_rx_window(a->c, 600);
		tcp_ack_now(a->c);
		CK(last()->window == 600, "a connection's own lower limit applies (%u)", last()->window);
	}

	/* -- 6. a half-open connection gives its slot back -- */
	reset_all();
	tcp_listen(23, on_accept);
	nout = 0;
	rx(PEER, 8000, 23, 42, 0, F_SYN, 0, NULL, 0);
	peer_t *h = &peers[0];
	uint32_t synack_seq = last()->seq;
	nout = 0;
	rx(PEER, 8000, 23, 42, 0, F_SYN, 0, NULL, 0);
	CK(nout == 1 && last()->flags == (F_SYN | F_ACK) && last()->seq == synack_seq,
		"a repeated SYN gets the same SYN-ACK at once");
	{
		int resends = 0;
		for (int t = 0; t < 40 && tcp_count(); t++) {
			now += 400;
			nout = 0;
			tcp_poll();
			if (nout && (last()->flags & F_SYN)) resends++;
		}
		CK(resends == TCP_SYNACK_RETRIES, "SYN-ACK resent %d times (want %d)", resends, TCP_SYNACK_RETRIES);
		CK(tcp_count() == 0 && h->closed == 1 && h->established == 0,
			"then abandoned: CLOSED to its handler, slot free");
	}

	/* -- 7. RFC 5961: a RST must match exactly -- */
	reset_all();
	tcp_listen(23, on_accept);
	{
		uint32_t ours;
		peer_t *pr = open_in(PEER, 9000, 5000, &ours);
		nout = 0;
		rx(PEER, 9000, 23, 5001 + 100, 0, F_RST, 0, NULL, 0);
		CK(pr->closed == 0 && tcp_is_connected(pr->c), "an in-window but inexact RST is ignored");
		CK(nout == 1 && last()->flags == F_ACK && last()->ack == 5001, "and challenged with an ACK");
		rx(PEER, 9000, 23, 5001, 0, F_RST, 0, NULL, 0);
		CK(pr->closed == 1 && tcp_count() == 0, "an exact RST closes");
	}

	/* -- 8. TIME_WAIT: reclaimed when short, reopened by a new SYN -- */
	reset_all();
	tcp_listen(23, on_accept);
	{
		uint32_t ours;
		peer_t *pr = open_in(PEER, 9100, 1, &ours);
		tcp_conn_t *c = pr->c;
		CK(tcp_close(c), "we close first");
		rx(PEER, 9100, 23, 2, ours + 1, F_ACK | F_FIN, 0, NULL, 0);
		CK(c->state == TCP_TIME_WAIT && tcp_count() == 0, "TIME_WAIT, which does not count as in use");
		CK(pr->closed == 1, "CLOSED when we closed first, too (it used to be missing)");
		nout = 0;
		rx(PEER, 9100, 23, 999, 0, F_SYN, 0, NULL, 0);
		CK(nout == 1 && last()->flags == (F_SYN | F_ACK) && accept_calls == 2,
			"a new SYN on the same 4-tuple reopens it");
		/* fill the pool with TIME_WAIT slots, then ask for more */
		reset_all();
		for (int i = 0; i < TCP_MAX_CONN; i++) {
			conns[i].state = TCP_TIME_WAIT;
			conns[i].time_wait_start = now - (uint32_t)(10 * (TCP_MAX_CONN - i));
			conns[i].local_port = (uint16_t)(1 + i);
		}
		tcp_conn_t *n = tcp_connect(PEER2, 80, on_event, NULL);
		CK(n == &conns[0], "the OLDEST TIME_WAIT slot is reclaimed");
	}

	/* -- 9. we close while our data is unacked and the peer FINs -- */
	reset_all();
	tcp_listen(23, on_accept);
	{
		uint32_t ours;
		peer_t *pr = open_in(PEER, 9200, 10, &ours);
		tcp_conn_t *c = pr->c;
		CK(tcp_send(c, (const uint8_t *)"bye", 3), "data out");
		nout = 0;
		rx(PEER, 9200, 23, 11, ours, F_ACK | F_FIN, 0, NULL, 0);   /* acks nothing of ours */
		CK(pr->closed == 1 && c->state == TCP_LAST_ACK && c->fin_pending, "CLOSED; our FIN waits");
		CK(nout == 1 && last()->flags == F_ACK, "only an ACK of their FIN so far");
		nout = 0;
		rx(PEER, 9200, 23, 12, ours + 3, F_ACK, 0, NULL, 0);        /* acks our data */
		CK(nout == 1 && last()->flags == (F_ACK | F_FIN) && last()->seq == ours + 3,
			"our FIN goes once the data is acked");
		rx(PEER, 9200, 23, 12, ours + 4, F_ACK, 0, NULL, 0);
		CK(tcp_count() == 0 && c->state == TCP_CLOSED, "and the slot is freed");
	}

	/* -- 10. re-entrancy: a handler aborting, and reopening, inside DATA -- */
	reset_all();
	tcp_listen(23, on_accept);
	{
		uint32_t ours;
		peer_t *pr = open_in(PEER, 9300, 20, &ours);
		tcp_conn_t *slot = pr->c;
		pr->abort_on_data = 1;
		pr->reopen_on_data = 1;
		nout = 0;
		/* data AND a FIN in one segment: the FIN must not reach the new connection */
		rx(PEER, 9300, 23, 21, ours, F_ACK | F_PSH | F_FIN, 0, "q", 1);
		CK(pr->got_len == 1 && pr->closed == 0, "data delivered, then the handler aborted");
		CK(reopened == slot, "the new connection took the same slot");
		CK(reopened && reopened->state == TCP_SYN_SENT && tcp_remote_ip(reopened) == PEER2,
			"and is untouched by the rest of the old segment");
		bool rst = false, fin = false;
		for (int i = 0; i < nout; i++) {
			if (out[i].flags & F_RST) rst = true;
			if ((out[i].flags & F_FIN) && out[i].dport == 9300) fin = true;
		}
		CK(rst && !fin, "the old peer got a RST, not a FIN");
	}

	/* -- 11. three connections at once, interleaved -- */
	reset_all();
	tcp_listen(23, on_accept);
	tcp_listen(80, on_accept);
	{
		uint32_t oa, ob;
		peer_t *a = open_in(PEER, 10000, 100, &oa);
		peer_t *b;
		nout = 0;
		rx(PEER2, 10001, 80, 200, 0, F_SYN, 0, NULL, 0);
		ob = last()->seq + 1;
		rx(PEER2, 10001, 80, 201, ob, F_ACK, 0, NULL, 0);
		b = &peers[1];
		peer_t *o = &peers[14];
		memset(o, 0, sizeof(*o));
		tcp_conn_t *oc = tcp_connect(PEER, 443, on_event, o);
		uint32_t oisn = last()->seq;
		rx(PEER, 443, tcp_local_port(oc), 300, oisn + 1, F_SYN | F_ACK, 0, NULL, 0);
		CK(a->established && b->established && o->established && tcp_count() == 3, "three up");
		rx(PEER, 10000, 23, 101, oa, F_ACK, 0, "AAA", 3);
		rx(PEER, 443, tcp_local_port(oc), 301, oisn + 1, F_ACK, 0, "OOOO", 4);
		rx(PEER2, 10001, 80, 201, ob, F_ACK, 0, "BB", 2);
		rx(PEER, 10000, 23, 104, oa, F_ACK, 0, "aa", 2);
		CK(a->got_len == 5 && !memcmp(a->got, "AAAaa", 5), "A got its own bytes");
		CK(b->got_len == 2 && !memcmp(b->got, "BB", 2), "B got its own bytes");
		CK(o->got_len == 4 && !memcmp(o->got, "OOOO", 4), "the outbound one got its own");
		/* an out-of-order segment on one does not disturb another */
		rx(PEER2, 10001, 80, 999, ob, F_ACK, 0, "late", 4);
		CK(b->got_len == 2 && a->got_len == 5, "a gap on B delivers nothing anywhere");
		tcp_abort(a->c);
		CK(tcp_count() == 2 && b->established && o->established, "aborting A leaves the others");
		tcp_unlisten(80);
		nout = 0;
		rx(PEER2, 10002, 80, 500, 0, F_SYN, 0, NULL, 0);
		CK(last()->flags & F_RST, "after unlisten, a new SYN to 80 is refused");
		rx(PEER2, 10001, 80, 203, ob, F_ACK, 0, "!", 1);
		CK(b->got_len == 3, "but the connection already open on 80 carries on");
	}

	/* -- 11b. the ACK for data carries the window AFTER the data was taken --
	 * Sent before, it advertised room the listener had just used, and a
	 * relay with a fixed buffer lost what TCP had acked (docs/netserve.md,
	 * "Found on hardware"). */
	reset_all();
	tcp_listen(23, on_accept);
	{
		uint32_t ours;
		peer_t *pr = open_in(PEER, 12000, 1, &ours);
		pr->shrink_on_data = 1;
		nout = 0;
		rx(PEER, 12000, 23, 2, ours, F_ACK, 0, "abc", 3);
		CK(nout == 1 && last()->ack == 5 && last()->window == 100,
			"the ACK of the data shows the window the handler set while taking it (%u)", last()->window);
	}

	/* -- 11c. half-close, for a connection that asks for it -- */
	reset_all();
	tcp_listen(23, on_accept);
	{
		uint32_t ours;
		peer_t *pr = open_in(PEER, 13000, 1, &ours);
		tcp_conn_t *c = pr->c;
		tcp_set_half_close(c, true);
		nout = 0;
		rx(PEER, 13000, 23, 2, ours, F_ACK | F_PSH | F_FIN, 0, "req", 3);
		CK(pr->got_len == 3 && pr->eofs == 1 && pr->closed == 0, "data, then EOF -- not CLOSED");
		CK(nout == 1 && last()->ack == 6 && !(last()->flags & F_FIN), "the FIN acked; ours not sent");
		CK(tcp_can_send(c) && !tcp_is_connected(c), "CLOSE_WAIT: can still send");
		CK(tcp_send(c, (const uint8_t *)"answer", 6) && last()->len == 6, "and does");
		CK(!tcp_close(c), "close waits for that segment's ack");
		rx(PEER, 13000, 23, 6, ours + 6, F_ACK, 0, NULL, 0);
		nout = 0;
		CK(tcp_close(c) && (last()->flags & F_FIN) && last()->seq == ours + 6, "then our FIN");
		rx(PEER, 13000, 23, 6, ours + 7, F_ACK, 0, NULL, 0);
		CK(pr->closed == 1 && tcp_count() == 0, "acked: CLOSED once, slot free");
		rx(PEER, 13000, 23, 5, 0, F_ACK | F_FIN, 0, NULL, 0);
		CK(pr->closed == 1, "nothing after");
	}

	/* -- 12. CLOSED is the last event -- */
	reset_all();
	tcp_listen(23, on_accept);
	{
		uint32_t ours;
		peer_t *pr = open_in(PEER, 11000, 1, &ours);
		rx(PEER, 11000, 23, 2, ours, F_ACK | F_FIN, 0, NULL, 0);
		int ev = pr->closed + pr->data_events;
		rx(PEER, 11000, 23, 2, ours, F_ACK | F_FIN, 0, "x", 1);   /* retransmit, and junk */
		rx(PEER, 11000, 23, 3, 0, F_RST, 0, NULL, 0);             /* an exact RST in LAST_ACK */
		CK(pr->closed + pr->data_events == ev, "nothing after CLOSED");
		CK(tcp_count() == 0, "the RST freed the slot");
	}

	printf("tcp pool: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
