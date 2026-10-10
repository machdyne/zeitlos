/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host test for tftp.c's GET side: the two-block queue, ACK on store,
 * the deferred ACK when the queue is full, duplicates, the end of the
 * file, and no retries while waiting on our own receiver.
 *
 *   make test   (or: cc -std=gnu99 -Wall -DTFTP_HOST_TEST -I. \
 *                  -I../../common -o /tmp/t tests/test_tftp_get.c && /tmp/t)
 *
 * INCLUDES tftp.c. Plays the server (feeds DATA to the handler
 * udp_listen() was given, reads the ACKs udp_send() is asked to send)
 * and the receiver (zstream pulls; chunks counted and checked).
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zstream.h"
#include "udp.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* -- stubs -- */

static udp_handler_t handler;
static uint32_t now_ticks;
static uint32_t cyc;

static int acks[1024];		// block numbers ACKed, in order
static int nacks;

static uint8_t got[64 * 1024];	// what the receiver was given
static uint32_t got_len;
static int chunks, eofs;

uint32_t tftp_test_cycles(void) { return cyc += 48 * 100; }	// 100us a look
uint32_t z_uptime_ticks(void) { return now_ticks; }

void udp_listen(uint16_t port, udp_handler_t h) { (void)port; handler = h; }
void udp_close(uint16_t port) { (void)port; }
bool udp_send(uint32_t ip, uint16_t dport, uint16_t sport, const uint8_t *p, uint16_t len) {
	(void)ip; (void)dport; (void)sport;
	if (len == 4 && p[1] == 4 && nacks < 1024) acks[nacks++] = (p[2] << 8) | p[3];
	return true;
}

void zstream_accept(zstream_producer_t *st, uint32_t pid, uint32_t tag) {
	(void)tag; memset(st, 0, sizeof(*st)); st->consumer_pid = pid; st->active = true;
}
zstream_event_t zstream_producer_handle(zstream_producer_t *st, z_msg_t *m) {
	(void)st;
	return m->subject == Z_STREAM_PULL ? ZSTREAM_EVENT_PULL : ZSTREAM_EVENT_ABORT;
}
void zstream_send_chunk(zstream_producer_t *st, const void *d, uint32_t n) {
	(void)st; memcpy(got + got_len, d, n); got_len += n; chunks++;
}
void zstream_send_eof(zstream_producer_t *st) { (void)st; eofs++; }
void zstream_send_error(zstream_producer_t *st, const char *m) { (void)st; (void)m; }
void zstream_open_async(zstream_consumer_t *c, uint32_t pid, z_obj_t o) { (void)c; (void)pid; (void)o; }
void zstream_pull_async(zstream_consumer_t *c) { (void)c; }
z_obj_t z_obj_none(void) { z_obj_t o; memset(&o, 0, sizeof o); return o; }
void zstream_abort(zstream_consumer_t *c) { (void)c; }
zstream_consumer_event_t zstream_consumer_handle(zstream_consumer_t *c, z_msg_t *m,
	const uint8_t **d, uint32_t *n, char *e, uint32_t el) {
	(void)c; (void)m; (void)d; (void)n; (void)e; (void)el; return ZSTREAM_CEVENT_NONE;
}

#include "../tftp.c"

/* -- the players -- */

#define SRV_IP 0x0a000001u

static void data(uint16_t blk, uint16_t len) {
	uint8_t p[4 + 512];
	p[0] = 0; p[1] = 3; p[2] = blk >> 8; p[3] = blk & 0xff;
	for (int i = 0; i < len; i++) p[4 + i] = (uint8_t)(blk * 7 + i);
	handler(SRV_IP, 5000, p, (uint16_t)(4 + len));
}

static void pull(void) {
	z_msg_t m;
	memset(&m, 0, sizeof(m));
	m.subject = Z_STREAM_PULL;
	tftp_handle_stream_msg(&m);
}

static bool got_ok(uint16_t blocks, uint16_t last_len) {
	uint32_t off = 0;
	for (uint16_t b = 1; b <= blocks; b++) {
		uint16_t len = (b == blocks) ? last_len : 512;
		for (int i = 0; i < len; i++)
			if (got[off + i] != (uint8_t)(b * 7 + i)) return false;
		off += len;
	}
	return off == got_len;
}

static void start(void) {
	nacks = 0; got_len = 0; chunks = eofs = 0;
	CHECK(tftp_get_start(SRV_IP, "f", 1, 1));
	CHECK(nacks == 0);
}

int main(void) {
	uint32_t n;
	char err[64];

	// A receiver that always has a pull out: every block ACKed at once.
	start();
	pull();
	for (uint16_t b = 1; b <= 5; b++) {
		data(b, 512);
		CHECK(nacks == b && acks[b - 1] == b);
		CHECK(chunks == b);
		pull();
	}
	data(6, 100);
	CHECK(nacks == 6 && acks[5] == 6);
	CHECK(eofs == 1);
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_OK);
	CHECK(n == 5 * 512 + 100);
	CHECK(got_ok(6, 100));

	// A slow receiver: block 1 is ACKed on arrival (a slot is left);
	// block 2 fills the queue, so its ACK waits for the first pull.
	start();
	data(1, 512);
	CHECK(nacks == 1 && acks[0] == 1);
	data(2, 512);
	CHECK(nacks == 1);			// deferred
	data(2, 512);				// the server's resend: ignored
	CHECK(nacks == 1);
	data(3, 512);				// no room, not ACKed: ignored
	CHECK(nacks == 1);
	// No retry while we wait on the receiver, however long.
	now_ticks += 10 * 732;
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_PENDING);
	CHECK(nacks == 1);
	pull();					// takes 1: 2's ACK goes out
	CHECK(chunks == 1 && nacks == 2 && acks[1] == 2);
	data(3, 512);				// 3 stored behind 2: the queue is full
	CHECK(nacks == 2);			// 2 and 3 queued: deferred
	pull();
	CHECK(chunks == 2 && nacks == 3 && acks[2] == 3);
	data(2, 512);				// an old duplicate: ignored
	data(3, 512);				// 3's ACK sent... once more
	CHECK(nacks == 4 && acks[3] == 3);
	data(4, 0);				// the empty block ends it
	CHECK(eofs == 0);			// 3 and 4 not yet taken
	pull();
	CHECK(eofs == 0 && chunks == 3);
	CHECK(nacks == 5 && acks[4] == 4);	// 4's deferred ACK
	pull();
	CHECK(eofs == 1 && chunks == 4);
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_OK);
	CHECK(n == 3 * 512 && got_ok(3, 512));

	// Waiting on the server does retry: the last ACK again.
	start();
	pull();
	data(1, 512);
	CHECK(nacks == 1);
	now_ticks += 400;
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_PENDING);
	CHECK(nacks == 2 && acks[1] == 1);
	CHECK(st_retry_net == 1);
	pull();
	data(2, 10);
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_OK);
	CHECK(st_net_n == 1 && st_recv_n == 2);

	printf("test_tftp_get: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
