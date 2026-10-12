/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host test for tcp.c's send window (tcp.h, "a send window").
 *
 *   make test          (or: cc -std=gnu99 -Wall -I. -I../../common \
 *                         -o /tmp/t tests/test_tcp_window.c && /tmp/t)
 *
 * INCLUDES tcp.c, with ip_send(), the clock, the RNG and the NIC's room
 * stubbed, and plays the peer: what is checked is the segments on the
 * wire -- how many are in flight, their sequence numbers, what a lost
 * one costs, and that a full NIC delays rather than loses them.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef struct {
	uint32_t seq, ack;
	uint8_t flags;
	uint16_t window, mss, len;
	uint8_t data[1600];
} seg_t;

static seg_t out[256];
static int nout;
static int room = 255;          /* frames the "NIC" takes */
static bool parked;             /* ip_send() "failed" by parking for ARP */
static bool nic_full_drop;      /* ip_send() fails when room is 0 */

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool ip_send(uint32_t ip, uint8_t proto, const uint8_t *p, uint16_t len) {
	(void)ip; (void)proto;
	if (nic_full_drop && room <= 0) return false;
	if (parked) return false;       /* "parked for ARP" (ip_tx_parked()) */
	if (nout >= 256) return true;
	seg_t *s = &out[nout++];
	uint8_t off = (uint8_t)((p[12] >> 4) * 4);
	memset(s, 0, sizeof(*s));
	s->seq = be32(p + 4);
	s->ack = be32(p + 8);
	s->flags = p[13];
	s->window = be16(p + 14);
	if (off >= 24 && p[20] == 2 && p[21] == 4) s->mss = be16(p + 22);
	s->len = (uint16_t)(len - off);
	memcpy(s->data, p + off, s->len);
	if (room < 255 && room > 0) room--;
	return true;
}

int tcp_link_room(void) { return room; }
bool ip_tx_parked(void) { return parked; }

static uint32_t now = 1000;
uint32_t z_uptime_ticks(void) { return now; }
static uint32_t rng = 0x2468ace0;
void z_rng_bytes(void *b, uint32_t n) {
	uint8_t *p = b;
	while (n--) { rng = rng * 1103515245u + 12345u; *p++ = (uint8_t)(rng >> 16); }
}
bool z_rng_secure(void) { return true; }
void z_rng_stir_event(void) { }

/* tcp.c builds its segments in the outgoing frame (ip.h): here, a
 * buffer of its own, handed to the ip_send() above. */
static uint8_t ip_txb[1600];
uint8_t *ip_tx_payload(void) { return ip_txb; }
bool ip_send_built(uint32_t ip, uint8_t proto, uint16_t len) { return ip_send(ip, proto, ip_txb, len); }

#include "../tcp.c"

static int fails, checks;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

#define PEER 0x0A000002u
#define F_FIN TCP_FLAG_FIN
#define F_SYN TCP_FLAG_SYN
#define F_ACK TCP_FLAG_ACK
#define F_PSH TCP_FLAG_PSH

static uint16_t lport;
static uint32_t peer_seq;

static void rx(uint32_t seq, uint32_t ack, uint8_t flags, uint16_t win, uint16_t mss,
		const void *data, uint16_t len) {
	uint8_t p[1600];
	uint8_t off = mss ? 24 : 20;
	memset(p, 0, sizeof(p));
	p[0] = 0; p[1] = 80;
	p[2] = (uint8_t)(lport >> 8); p[3] = (uint8_t)lport;
	p[4] = (uint8_t)(seq >> 24); p[5] = (uint8_t)(seq >> 16); p[6] = (uint8_t)(seq >> 8); p[7] = (uint8_t)seq;
	p[8] = (uint8_t)(ack >> 24); p[9] = (uint8_t)(ack >> 16); p[10] = (uint8_t)(ack >> 8); p[11] = (uint8_t)ack;
	p[12] = (uint8_t)((off / 4) << 4);
	p[13] = flags;
	p[14] = (uint8_t)(win >> 8); p[15] = (uint8_t)win;
	if (mss) { p[20] = 2; p[21] = 4; p[22] = (uint8_t)(mss >> 8); p[23] = (uint8_t)mss; }
	if (len) memcpy(p + off, data, len);
	tcp_handle(PEER, p, (uint16_t)(off + len));
}

static int closed_events;
static void on_event(tcp_conn_t *c, tcp_event_t ev, const uint8_t *d, uint16_t n) {
	(void)c; (void)d; (void)n;
	if (ev == TCP_EVENT_CLOSED) closed_events++;
}

/* An outbound connection, established; returns our first data seq. */
static uint32_t open_conn(tcp_conn_t **cp, uint16_t mss, uint16_t win) {
	nout = 0;
	*cp = tcp_connect(PEER, 80, on_event, NULL);
	lport = tcp_local_port(*cp);
	uint32_t iss = out[0].seq;
	peer_seq = 5000;
	rx(peer_seq, iss + 1, F_SYN | F_ACK, win, mss, NULL, 0);
	peer_seq++;
	nout = 0;
	return iss + 1;
}

static uint8_t blk[1460];

static int data_segs(int from) {
	int n = 0;
	for (int i = from; i < nout; i++) if (out[i].len) n++;
	return n;
}

static int free_pool(void) {
	int n = 0;
	for (int i = 0; i < TCP_TX_SEGS; i++) if (!txpool[i].used) n++;
	return n;
}

static void reset_all(void) {
	for (int i = 0; i < TCP_MAX_CONN; i++) if (conns[i].state != TCP_CLOSED) slot_free(&conns[i]);
	room = 255;
	nic_full_drop = false;
	closed_events = 0;
	nout = 0;
}

int main(void) {
	tcp_conn_t *c, *c2;
	uint32_t s0;

	for (int i = 0; i < (int)sizeof(blk); i++) blk[i] = (uint8_t)(i * 13);
	tcp_init(0x0A000001u);

	/* -- MSS from the peer, within what we send -- */
	s0 = open_conn(&c, 1460, 16384);
	CK(tcp_mss(c) == 1460, "peer 1460: segments of 1460 (%u)", tcp_mss(c));
	reset_all();
	s0 = open_conn(&c, 0, 16384);
	CK(tcp_mss(c) == 536, "no option: RFC 879's 536 (%u)", tcp_mss(c));
	reset_all();
	s0 = open_conn(&c, 9000, 16384);
	CK(tcp_mss(c) == TCP_MAX_PAYLOAD, "a jumbo MSS: capped at ours (%u)", tcp_mss(c));
	reset_all();

	/* -- slow start: two at first, one more per ACK -- */
	s0 = open_conn(&c, 1460, 60000);
	for (int i = 0; i < TCP_TX_MAX; i++)
		CK(tcp_send(c, blk, 1460), "queued %d", i);
	CK(!tcp_send(c, blk, 1460), "the per-connection queue is full at %d", TCP_TX_MAX);
	CK(data_segs(0) == 2, "two on the wire at first (%d)", data_segs(0));
	CK(out[0].seq == s0 && out[1].seq == s0 + 1460, "in sequence");
	nout = 0;
	rx(peer_seq, s0 + 1460, F_ACK, 60000, 0, NULL, 0);
	CK(data_segs(0) == 2 && out[0].seq == s0 + 2920 && out[1].seq == s0 + 4380,
		"one acked, cwnd 3: two more out (%d)", data_segs(0));
	CK(tcp_send(c, blk, 1460), "and the freed slot takes another");
	nout = 0;
	rx(peer_seq, s0 + 5840, F_ACK, 60000, 0, NULL, 0);
	CK(data_segs(0) == TCP_TX_MAX + 1 - 4, "four acked, cwnd 4: the rest still queued go (%d)",
		data_segs(0));
	nout = 0;
	rx(peer_seq, s0 + (TCP_TX_MAX + 1) * 1460, F_ACK, 60000, 0, NULL, 0);
	CK(c->qn == 0 && free_pool() == TCP_TX_SEGS, "all acked: the pool is empty again");
	reset_all();

	/* -- the peer's window -- */
	s0 = open_conn(&c, 1460, 3000);
	c->cwnd = TCP_TX_MAX;
	for (int i = 0; i < 4; i++) tcp_send(c, blk, 1460);
	CK(data_segs(0) == 2, "a 3000-byte window: two segments (%d)", data_segs(0));
	nout = 0;
	rx(peer_seq, s0 + 1460, F_ACK, 3000, 0, NULL, 0);
	CK(data_segs(0) == 1 && out[0].seq == s0 + 2920, "one acked, one more (%d)", data_segs(0));
	nout = 0;
	rx(peer_seq, s0 + 2920, F_ACK, 0, 0, NULL, 0);
	CK(data_segs(0) == 0, "window 0: nothing more while one is out");
	rx(peer_seq, s0 + 4380, F_ACK, 0, 0, NULL, 0);
	CK(data_segs(0) == 1 && out[nout - 1].seq == s0 + 4380,
		"nothing out and window 0: one goes anyway, as the probe");
	reset_all();

	/* -- three duplicate ACKs: the oldest again, at once -- */
	s0 = open_conn(&c, 1460, 60000);
	c->cwnd = TCP_TX_MAX;
	for (int i = 0; i < 5; i++) tcp_send(c, blk, 1460);
	CK(data_segs(0) == 5, "five in flight (%d)", data_segs(0));
	nout = 0;
	rx(peer_seq, s0, F_ACK, 60000, 0, NULL, 0);
	rx(peer_seq, s0, F_ACK, 60000, 0, NULL, 0);
	CK(data_segs(0) == 0, "two duplicates: nothing yet");
	rx(peer_seq, s0, F_ACK, 60000, 0, NULL, 0);
	CK(data_segs(0) == 1 && out[0].seq == s0 && !memcmp(out[0].data, blk, 1460),
		"the third: the lost one again, whole (%d)", data_segs(0));
	rx(peer_seq, s0, F_ACK, 61000, 0, NULL, 0);
	CK(data_segs(0) == 1, "a window update is not a duplicate");
	nout = 0;
	rx(peer_seq, s0 + 5 * 1460, F_ACK, 60000, 0, NULL, 0);
	CK(c->qn == 0, "the receiver had the rest: one ACK clears all five");
	reset_all();

	/* -- a timeout: everything unacked again, from the oldest -- */
	s0 = open_conn(&c, 1460, 60000);
	c->cwnd = TCP_TX_MAX;
	for (int i = 0; i < 4; i++) tcp_send(c, blk, 1460);
	nout = 0;
	now += TCP_RTO_TICKS_BASE - 1;
	tcp_poll();
	CK(nout == 0, "not before the timeout");
	now += 2;
	tcp_poll();
	CK(data_segs(0) == 1 && out[0].seq == s0, "timeout: the oldest, at a window of one (%d)", data_segs(0));
	nout = 0;
	rx(peer_seq, s0 + 1460, F_ACK, 60000, 0, NULL, 0);
	CK(data_segs(0) == 2 && out[0].seq == s0 + 1460 && out[1].seq == s0 + 2920,
		"acked: the next two, with their first sequence numbers (%d)", data_segs(0));
	CK(c->snd_nxt == s0 + 4 * 1460, "snd_nxt never moved back");
	reset_all();

	/* -- a partial ACK keeps the segment -- */
	s0 = open_conn(&c, 1460, 60000);
	tcp_send(c, blk, 1460);
	rx(peer_seq, s0 + 1000, F_ACK, 60000, 0, NULL, 0);
	CK(c->qn == 1 && c->snd_una == s0 + 1000, "a partial ACK: kept, snd_una moves");
	rx(peer_seq, s0 + 1460, F_ACK, 60000, 0, NULL, 0);
	CK(c->qn == 0, "the rest: freed");
	rx(peer_seq, s0 + 9999, F_ACK, 60000, 0, NULL, 0);
	CK(c->snd_una == s0 + 1460, "an ACK for what was never sent is ignored");
	reset_all();

	/* -- a full NIC: segments wait, they are not lost -- */
	s0 = open_conn(&c, 1460, 60000);
	c->cwnd = TCP_TX_MAX;
	room = 1;
	for (int i = 0; i < 3; i++) tcp_send(c, blk, 1460);
	CK(data_segs(0) == 1 && c->qsent == 1, "room for one: one sent (%d)", data_segs(0));
	CK(tcp_tx_waiting(), "net is told to look again soon");
	room = 255;
	tcp_poll();
	CK(data_segs(0) == 3 && c->qsent == 3 && !tcp_tx_waiting(), "room again: the rest (%d)", data_segs(0));
	CK(out[1].seq == s0 + 1460 && out[2].seq == s0 + 2920, "in order");
	nout = 0;
	room = 0;
	nic_full_drop = true;
	rx(peer_seq, s0 + 3 * 1460, F_ACK, 60000, 0, NULL, 0);
	tcp_send(c, blk, 100);
	CK(c->qsent == 0 && c->qn == 1, "a send() that fails: held, unsent");
	room = 255;
	tcp_poll();
	CK(c->qsent == 1 && data_segs(0) == 1 && out[0].len == 100, "and sent when it can be");
	reset_all();

	/* -- a NIC that never has room times the connection out -- */
	s0 = open_conn(&c, 1460, 60000);
	room = 0;
	tcp_send(c, blk, 1460);
	for (int i = 0; i < 200 && !closed_events; i++) { now += TCP_RTO_TICKS_BASE * 20; tcp_poll(); }
	CK(closed_events == 1 && free_pool() == TCP_TX_SEGS, "given up, CLOSED, its segment freed");
	reset_all();

	/* -- the pool is shared -- */
	s0 = open_conn(&c, 1460, 60000);
	for (int i = 0; i < TCP_TX_MAX; i++) tcp_send(c, blk, 1460);
	{
		uint32_t s2 = open_conn(&c2, 1460, 60000);
		int got = 0;
		while (tcp_send(c2, blk, 1460)) got++;
		CK(got == TCP_TX_SEGS - TCP_TX_MAX, "a second connection gets what is left (%d)", got);
		tcp_abort(c);
		CK(tcp_send(c2, blk, 1460), "an abort frees the first one's");
		(void)s2;
	}
	reset_all();

	/* -- close: the FIN follows the data, once acked -- */
	s0 = open_conn(&c, 1460, 60000);
	tcp_send(c, blk, 1460);
	tcp_send(c, blk, 700);
	nout = 0;
	CK(tcp_close(c) && nout == 0, "close with data out: taken, no FIN yet");
	CK(tcp_close(c), "asked again: still fine");
	CK(!tcp_send(c, blk, 10), "no data after close");
	rx(peer_seq, s0 + 1460, F_ACK, 60000, 0, NULL, 0);
	CK(nout == 0, "partly acked: still no FIN");
	rx(peer_seq, s0 + 2160, F_ACK, 60000, 0, NULL, 0);
	CK(nout == 1 && (out[0].flags & F_FIN) && out[0].seq == s0 + 2160, "all acked: the FIN");
	reset_all();

	/* -- the peer's FIN while ours is still queued -- */
	s0 = open_conn(&c, 1460, 60000);
	room = 0;
	tcp_send(c, blk, 1460);
	tcp_send(c, blk, 1460);
	rx(peer_seq, s0, F_ACK | F_FIN, 60000, 0, NULL, 0);
	CK(c->state == TCP_LAST_ACK && c->fin_pending, "LAST_ACK, our FIN held for the data");
	room = 255;
	nout = 0;
	tcp_poll();
	CK(data_segs(0) == 2, "the data still goes (%d)", data_segs(0));
	nout = 0;
	rx(peer_seq + 1, s0 + 2920, F_ACK, 60000, 0, NULL, 0);
	CK(nout == 1 && (out[0].flags & F_FIN), "acked: then our FIN");
	rx(peer_seq + 1, s0 + 2921, F_ACK, 60000, 0, NULL, 0);
	CK(c->state == TCP_CLOSED && free_pool() == TCP_TX_SEGS, "and the slot is free");
	reset_all();

	/* -- parked for ARP: counts as sent, not as a full NIC -- */
	s0 = open_conn(&c, 1460, 60000);
	parked = true;
	tcp_send(c, blk, 500);
	CK(c->qsent == 1 && !tcp_tx_waiting(), "parked: sent as far as TCP knows, no tight retry");
	parked = false;
	reset_all();

	/* -- close queued, then the peer's data and FIN in one segment -- */
	s0 = open_conn(&c, 1460, 60000);
	tcp_send(c, blk, 300);
	tcp_close(c);
	nout = 0;
	rx(peer_seq, s0 + 300, F_ACK | F_PSH | F_FIN, 60000, 0, "xy", 2);
	{
		int fin_i = -1;
		for (int i = 0; i < nout; i++) if (out[i].flags & F_FIN) fin_i = i;
		CK(fin_i >= 0 && out[fin_i].ack == peer_seq + 3,
			"our FIN acks the peer's data and FIN (%u, want %u)",
			fin_i >= 0 ? out[fin_i].ack : 0, peer_seq + 3);
		CK(c->state == TCP_LAST_ACK, "LAST_ACK (%d)", c->state);
	}
	reset_all();

	/* -- a zero window answered for a long time is not a dead peer -- */
	s0 = open_conn(&c, 1460, 60000);
	tcp_send(c, blk, 1460);
	tcp_send(c, blk, 1460);
	rx(peer_seq, s0, F_ACK, 0, 0, NULL, 0);
	for (int i = 0; i < 30; i++) {
		now += TCP_RTO_TICKS_BASE * 20;
		tcp_poll();
		rx(peer_seq, s0, F_ACK, 0, 0, NULL, 0);     /* answers each probe: still full */
	}
	CK(closed_events == 0 && c->state == TCP_ESTABLISHED, "still connected after 30 probes");
	nout = 0;
	rx(peer_seq, s0, F_ACK, 8000, 0, NULL, 0);
	CK(data_segs(0) >= 1 && out[0].seq == s0, "the window reopens: the probe again at once (%d)",
		data_segs(0));
	reset_all();

	/* -- the last free segment is for a connection with none -- */
	s0 = open_conn(&c, 1460, 60000);
	for (int i = 0; i < TCP_TX_SEGS - 2; i++) tcp_send(c, blk, 100);
	{
		tcp_conn_t *b, *d;
		open_conn(&b, 1460, 60000);
		CK(tcp_send(b, blk, 100), "a second connection: one");
		CK(!tcp_send(c, blk, 100), "the first may not take the last free one");
		open_conn(&d, 1460, 60000);
		CK(tcp_send(d, blk, 100), "a third, with none out, may");
	}
	reset_all();

	/* -- the MSS we advertise -- */
	tcp_set_rx_mss(1460);
	nout = 0;
	c = tcp_connect(PEER, 80, on_event, NULL);
	CK(out[0].mss == 1460, "a NIC with room: 1460 on the SYN (%u)", out[0].mss);
	tcp_set_rx_mss(9000);
	CK(tcp_rx_mss() == TCP_MAX_RX_PAYLOAD, "never more than a frame holds");
	tcp_set_rx_mss(TCP_ADVERTISE_MSS);
	reset_all();

	printf("tcp window: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
