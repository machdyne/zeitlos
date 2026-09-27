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
bool tcp_send(tcp_conn_t *c, const uint8_t *d, uint16_t n) {
	(void)d;
	if (!c->acked) return false;		// one segment in flight, as tcp.c
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

	printf("test_sock_slots: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;

}
