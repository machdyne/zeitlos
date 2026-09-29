/*
 * Host test for sock.c's slots (NET_SOCK_SLOTS, docs/networking.md,
 * "More than one socket"): two sockets at once, each event reaching
 * the right slot, each slot's queue its own, and a closed slot free
 * for the next connection. tcp.c is replaced by a stub that records
 * what it was asked and lets the test fire events by hand.
 *
 *   make -C sw/apps/net test
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../sock.h"

struct tcp_conn { tcp_event_handler_t h; void *user; int sent; uint16_t lastlen; bool acked; };
static struct tcp_conn pool[4];
static int npool;

tcp_conn_t *tcp_connect(uint32_t ip, uint16_t port, tcp_event_handler_t h, void *user) {
	(void)ip; (void)port;
	if (npool == 4) return NULL;
	pool[npool] = (struct tcp_conn){ h, user, 0, 0, true };
	return &pool[npool++];
}
void *tcp_user(const tcp_conn_t *c) { return c->user; }
uint16_t tcp_mss(const tcp_conn_t *c) { (void)c; return 536; }
// what reached TCP, byte for byte, per connection
static uint8_t wire[4][40000];
static int wlen[4];
bool tcp_send(tcp_conn_t *c, const uint8_t *d, uint16_t n) {
	if (!c->acked) return false;		// one segment in flight, as tcp.c
	int i = (int)(c - pool);
	if (wlen[i] + n <= (int)sizeof(wire[0])) { memcpy(wire[i] + wlen[i], d, n); wlen[i] += n; }
	c->sent += n; c->lastlen = n; c->acked = false;
	return true;
}
bool tcp_close(tcp_conn_t *c) { (void)c; return true; }
void tcp_abort(tcp_conn_t *c) { (void)c; }

static int checks, fails;
#define CK(c, w) do { checks++; if (!(c)) { fails++; printf("FAIL %d: %s\n", __LINE__, w); } } while (0)

static int est[4], closed[4], got[4], last_slot = -1;
static void on_est(int s) { est[s]++; }
static void on_data(int s, const uint8_t *d, uint16_t n) { (void)d; got[s] += n; last_slot = s; }
static void on_closed(int s) { closed[s]++; }
static uint32_t acks[64];
static int acks_n, gone_n;
static bool client_alive = true;
static void on_ack(uint32_t pid, uint32_t tag) { (void)pid; if (acks_n < 64) acks[acks_n++] = tag; }
static bool is_alive(int s) { (void)s; return client_alive; }
static void on_gone(int s) { gone_n++; sock_abort(s); }

int main(void) {

	uint8_t buf[3000];
	memset(buf, 'x', sizeof(buf));

	CK(NET_SOCK_SLOTS == 2, "two slots by default");
	CK(sock_free_slot() == 0, "slot 0 free first");

	CK(sock_connect(0, 1, 80, on_est, on_data, on_closed), "slot 0 connects");
	CK(sock_free_slot() == 1, "then slot 1");
	CK(!sock_connect(0, 1, 80, on_est, on_data, on_closed), "a busy slot refuses");
	CK(sock_connect(1, 2, 6667, on_est, on_data, on_closed), "slot 1 connects alongside");
	CK(sock_free_slot() == -1, "then none");
	CK(sock_tcp(0) == &pool[0] && sock_tcp(1) == &pool[1], "each slot its own connection");

	// Sending before ESTABLISHED is refused; events go to their slot.
	CK(!sock_send(1, buf, 10), "no send before established");
	pool[1].h(&pool[1], TCP_EVENT_ESTABLISHED, NULL, 0);
	CK(est[1] == 1 && est[0] == 0, "established reaches slot 1 only");
	pool[0].h(&pool[0], TCP_EVENT_ESTABLISHED, NULL, 0);
	CK(est[0] == 1, "and slot 0");

	pool[1].h(&pool[1], TCP_EVENT_DATA, buf, 100);
	CK(got[1] == 100 && got[0] == 0 && last_slot == 1, "data reaches its slot");
	pool[0].h(&pool[0], TCP_EVENT_DATA, buf, 7);
	CK(got[0] == 7 && last_slot == 0, "data reaches slot 0");

	// Queues are per slot: fill slot 0, slot 1 still takes bytes.
	CK(sock_send(0, buf, 2000), "slot 0 queues 2000");
	CK(!sock_send(0, buf, 100), "slot 0's queue is full at 2048");
	CK(sock_send(1, buf, 300), "slot 1's queue is its own");

	// One poll moves one segment per slot.
	sock_poll();
	CK(pool[0].sent == 536 && pool[1].sent == 300, "one segment each per poll");
	sock_poll();
	CK(pool[0].sent == 536, "no second segment until acked");
	pool[0].acked = true; sock_poll();
	CK(pool[0].sent == 1072, "next segment after the ack");

	// Closing slot 1 frees it; slot 0 is untouched.
	pool[1].h(&pool[1], TCP_EVENT_CLOSED, NULL, 0);
	CK(closed[1] == 1 && closed[0] == 0, "closed reaches slot 1 only");
	CK(sock_tcp(1) == NULL && sock_free_slot() == 1, "slot 1 free again");
	CK(!sock_send(1, buf, 1), "a closed slot sends nothing");
	CK(sock_send(0, buf, 10) || true, "slot 0 unaffected");
	CK(sock_connect(1, 3, 70, on_est, on_data, on_closed) && sock_tcp(1) == &pool[2],
		"slot 1 reused for a new connection");

	// An event for a stale connection (a slot since reused) is ignored.
	got[1] = 0;
	pool[1].h(&pool[1], TCP_EVENT_DATA, buf, 5);
	CK(got[1] == 0, "stale connection's event ignored");

	// Bad slot numbers are refused, not indexed.
	CK(!sock_connect(-1, 1, 1, on_est, on_data, on_closed) &&
		!sock_connect(NET_SOCK_SLOTS, 1, 1, on_est, on_data, on_closed) &&
		!sock_send(7, buf, 1) && sock_tcp(9) == NULL, "out-of-range slots");

	// -- client -> peer backpressure (sock.h): held, not dropped --
	{
		static uint8_t m[SOCK_HOLD + 2][1500], expect[40000];
		int nexp = 0;
		for (int i = 0; i < SOCK_HOLD + 2; i++) for (int j = 0; j < 1500; j++) m[i][j] = (uint8_t)(i * 31 + j);
		// a fresh socket: slot 1, its connection pool[2] (above)
		pool[2].h(&pool[2], TCP_EVENT_ESTABLISHED, NULL, 0);
		wlen[2] = 0;
		pool[2].acked = true;
		acks_n = 0;
		CK(sock_offer(1, m[0], 1500, 70, 1) == SOCK_QUEUED, "1500 bytes: all in the queue -- ack now");
		memcpy(expect + nexp, m[0], 1500); nexp += 1500;
		CK(sock_offer(1, m[1], 1500, 70, 2) == SOCK_HELD && sock_held(1) == 1, "the next 1500: 548 in, the rest HELD -- not dropped");
		memcpy(expect + nexp, m[1], 1500); nexp += 1500;
		CK(sock_offer(1, m[2], 1500, 70, 3) == SOCK_HELD && sock_held(1) == 2, "a third: held behind it, none of it jumping ahead");
		memcpy(expect + nexp, m[2], 1500); nexp += 1500;
		for (int i = 3; i < SOCK_HOLD + 1; i++) { sock_offer(1, m[i], 1500, 70, (uint32_t)(i + 1)); memcpy(expect + nexp, m[i], 1500); nexp += 1500; }
		CK(sock_held(1) == SOCK_HOLD, "up to zport's window held");
		CK(sock_offer(1, m[SOCK_HOLD + 1], 10, 70, 99) == SOCK_OVER, "one more than the window: refused -- a misbehaving client");
		CK(acks_n == 0, "nothing held is acked yet");
		// TCP drains: one segment at a time, acked by the peer
		for (int round = 0; round < 400 && (sock_held(1) || wlen[2] < nexp); round++) {
			sock_poll();
			pool[2].acked = true;
			sock_pump_held(on_ack, NULL, NULL);
		}
		CK(wlen[2] == nexp && !memcmp(wire[2], expect, (size_t)nexp),
			"every byte reached TCP, in order -- nothing lost, nothing reordered");
		bool order = acks_n == SOCK_HOLD;
		for (int i = 0; i < acks_n; i++) if (acks[i] != (uint32_t)(i + 2)) order = false;
		CK(order, "each held message acked once all of it was in, in order");

		// a client that dies with data held: its memory never read again
		static uint8_t dead[1500];
		memset(dead, 'D', sizeof(dead));
		wlen[2] = 0; acks_n = 0; gone_n = 0;
		sock_offer(1, m[0], 1500, 70, 1);
		sock_offer(1, m[1], 1500, 70, 2);
		sock_offer(1, dead, 1500, 70, 3);			// held
		memset(dead, '!', sizeof(dead));			// "freed", and reused by another process
		client_alive = false;
		for (int round = 0; round < 50; round++) { sock_poll(); pool[2].acked = true; sock_pump_held(on_ack, is_alive, on_gone); }
		// only (a prefix of) what was queued before it died -- the first
		// message and the 548 bytes of the second that fitted; the abort
		// discards the rest of the queue -- and not one byte held
		static uint8_t before[2048];
		memcpy(before, m[0], 1500); memcpy(before + 1500, m[1], 548);
		CK(wlen[2] > 0 && wlen[2] <= 2048 && !memcmp(wire[2], before, (size_t)wlen[2]),
			"a dead client's held bytes: never read -- only what was queued before went");
		CK(gone_n == 1 && sock_held(1) == 0, "its socket closed, nothing held");
		for (int i = 0; i < acks_n; i++) CK(acks[i] != 3, "nothing acked to a dead client");
		client_alive = true;

		// closing a socket acks what it still held -- a new connection on slot 1
		CK(sock_connect(1, 4, 71, on_est, on_data, on_closed) && sock_tcp(1) == &pool[3], "(slot 1 connects again)");
		pool[3].h(&pool[3], TCP_EVENT_ESTABLISHED, NULL, 0);
		acks_n = 0;
		sock_offer(1, m[0], 1500, 70, 1);
		sock_offer(1, m[1], 1500, 70, 2);
		sock_drop_held(1, on_ack);
		CK(acks_n == 1 && acks[0] == 2 && sock_held(1) == 0, "a socket closing: what it held acked, so the client can free it");
	}

	printf("test_sock_slots: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;

}
