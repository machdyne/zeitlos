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
static int rrqs;		// RRQs sent
static bool rrq_blksize;	// the last RRQ asked for blksize 1468
static char order[64];		// 'A' an ACK, 'C' a chunk, in the order sent
static int norder;

static uint8_t got[64 * 1024];	// what the receiver was given
static uint32_t got_len;
static int chunks, eofs;

uint32_t tftp_test_cycles(void) { return cyc += 48 * 100; }	// 100us a look
uint32_t z_uptime_ticks(void) { return now_ticks; }

void udp_listen(uint16_t port, udp_handler_t h) { (void)port; handler = h; }
void udp_close(uint16_t port) { (void)port; }
bool udp_send(uint32_t ip, uint16_t dport, uint16_t sport, const uint8_t *p, uint16_t len) {
	(void)ip; (void)dport; (void)sport;
	if (len == 4 && p[1] == 4 && nacks < 1024) {
		acks[nacks++] = (p[2] << 8) | p[3];
		if (norder < 63) order[norder++] = 'A';
	}
	if (p[1] == 1) {
		rrqs++;
		rrq_blksize = false;
		for (int i = 2; i + 12 < len; i++)
			if (!memcmp(p + i, "blksize\0" "1468\0", 13)) rrq_blksize = true;
	}
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
	if (norder < 63) order[norder++] = 'C';
}
// A lent chunk: copied as the receiver would, and remembered, so that
// pull() can check it was left untouched until then.
static const uint8_t *lent_ptr;
static uint8_t lent_copy[1468];
static uint32_t lent_len;
void zstream_send_chunk_ref(zstream_producer_t *st, z_blob_t *b) {
	zstream_send_chunk(st, b->data, b->len);
	lent_ptr = b->data;
	lent_len = b->len;
	memcpy(lent_copy, b->data, b->len);
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
	uint8_t p[4 + 1468];
	p[0] = 0; p[1] = 3; p[2] = blk >> 8; p[3] = blk & 0xff;
	for (int i = 0; i < len; i++) p[4 + i] = (uint8_t)(blk * 7 + i);
	handler(SRV_IP, 5000, p, (uint16_t)(4 + len));
}

static int lent_bad;
static void pull(void) {
	if (lent_ptr && memcmp(lent_ptr, lent_copy, lent_len)) lent_bad++;
	lent_ptr = NULL;
	z_msg_t m;
	memset(&m, 0, sizeof(m));
	m.subject = Z_STREAM_PULL;
	tftp_handle_stream_msg(&m);
}

static void oack(const char *opts, int optlen) {
	uint8_t p[64];
	p[0] = 0; p[1] = 6;
	memcpy(p + 2, opts, optlen);
	handler(SRV_IP, 5000, p, (uint16_t)(2 + optlen));
}

static void error(uint16_t code) {
	uint8_t p[8] = { 0, 5, code >> 8, code & 0xff, 'n', 'o', 0 };
	handler(SRV_IP, 5000, p, 7);
}

static uint16_t bsz = 512;

static bool got_ok(uint16_t blocks, uint16_t last_len) {
	uint32_t off = 0;
	for (uint16_t b = 1; b <= blocks; b++) {
		uint16_t len = (b == blocks) ? last_len : bsz;
		for (int i = 0; i < len; i++)
			if (got[off + i] != (uint8_t)(b * 7 + i)) return false;
		off += len;
	}
	return off == got_len;
}

static void start(void) {
	nacks = 0; got_len = 0; chunks = eofs = 0; rrqs = 0; norder = 0;
	lent_ptr = NULL;		// the last transfer's receiver is done
	bsz = 512;
	CHECK(tftp_get_start(SRV_IP, "f", 1, 1));
	CHECK(nacks == 0);
	CHECK(rrqs == 1 && rrq_blksize);	// blksize asked for
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
	// With a pull waiting, the chunk goes before the ACK (the ACK is
	// the last message, so the hand-off is to usb).
	CHECK(norder >= 2 && order[0] == 'C' && order[1] == 'A');
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_OK);
	CHECK(n == 5 * 512 + 100);
	CHECK(got_ok(6, 100));

	// A slow receiver. Three slots, one of them lent to the receiver
	// until its next pull: blocks 1 and 2 are ACKed on arrival; block 3
	// fills the queue, so its ACK waits until a slot is free again.
	start();
	data(1, 512);
	CHECK(nacks == 1 && acks[0] == 1);
	data(2, 512);
	CHECK(nacks == 2 && acks[1] == 2);
	data(3, 512);
	CHECK(nacks == 2);			// deferred
	data(3, 512);				// the server's resend: ignored
	CHECK(nacks == 2);
	data(4, 512);				// no room, not ACKed: ignored
	CHECK(nacks == 2);
	// No retry while we wait on the receiver, however long.
	now_ticks += 10 * 732;
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_PENDING);
	CHECK(nacks == 2);
	pull();					// 1 lent: no slot free yet
	CHECK(chunks == 1 && nacks == 2);
	pull();					// 1 back, 2 lent: 3's ACK goes out
	CHECK(chunks == 2 && nacks == 3 && acks[2] == 3);
	data(2, 512);				// an old duplicate: ignored
	data(3, 512);				// the newest again: NOT ACKed
	CHECK(nacks == 3);			// (no Sorcerer's Apprentice)
	CHECK(st_dup == 2);			// and the resend of 3 earlier
	data(4, 512);				// fills the queue again
	CHECK(nacks == 3);
	pull();					// 2 back, 3 lent: 4's ACK
	CHECK(chunks == 3 && nacks == 4 && acks[3] == 4);
	data(5, 0);				// the empty block ends it
	CHECK(nacks == 4 && eofs == 0);		// deferred
	pull();					// 3 back, 4 lent: 5's ACK
	CHECK(chunks == 4 && nacks == 5 && acks[4] == 5 && eofs == 0);
	pull();					// 4 back, 5 delivered: the end
	CHECK(eofs == 1 && chunks == 5);
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_OK);
	CHECK(n == 4 * 512 && got_ok(4, 512));

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

	// Options: an OACK for 1468, our ACK 0, then 1468-byte blocks; a
	// block shorter than that ends it.
	start();
	pull();
	oack("blksize\0" "1468\0", 13);
	CHECK(nacks == 1 && acks[0] == 0);
	CHECK(blksize == 1468);
	bsz = 1468;
	data(1, 1468);
	CHECK(nacks == 2 && acks[1] == 1 && chunks == 1);
	pull();
	data(2, 1468);				// a full block does not end it
	CHECK(eofs == 0);
	pull();
	data(3, 512);				// shorter than 1468: the end
	CHECK(eofs == 1);
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_OK);
	CHECK(n == 2 * 1468 + 512 && got_ok(3, 512));

	// A server that asks for less gets less; one without options sends
	// block 1 at 512 (the first test above).
	start();
	oack("BLKSIZE\0" "1024\0", 13);
	CHECK(blksize == 1024);
	pull();
	data(1, 1100);				// bigger than agreed
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_ERROR);

	// A server that refuses options (error 8): asked again without.
	start();
	error(8);
	CHECK(rrqs == 2 && !rrq_blksize);
	pull();
	data(1, 100);
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_OK);
	// Any other error before the first block ends it.
	start();
	error(1);
	CHECK(rrqs == 1);
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_ERROR);

	// Big blocks agreed but none arrives (a smaller MTU on the way):
	// after two retries, a fresh RRQ without options.
	start();
	pull();
	oack("blksize\0" "1468\0", 13);
	CHECK(blksize == 1468 && rrqs == 1);
	for (int k = 0; k < 3; k++) { now_ticks += 400; tftp_poll(&n, err, sizeof err); }
	CHECK(rrqs == 2 && !rrq_blksize && blksize == 512);
	data(1, 100);
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_OK);

	// The retry timeout follows the round trip: a few quick blocks,
	// then a lost one is retried after ~41ms (30 ticks), not 0.5s,
	// and the next retry after twice that.
	start();
	pull();
	for (uint16_t b = 1; b <= 5; b++) { data(b, 512); pull(); }
	CHECK(srtt_us > 0 && srtt_us < 2000);
	uint32_t na = nacks;
	now_ticks += 29;
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_PENDING);
	CHECK(nacks == na);			// not yet
	now_ticks += 2;
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_PENDING);
	CHECK(nacks == na + 1 && acks[na] == 5);	// ACK 5 again
	now_ticks += 50;
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_PENDING);
	CHECK(nacks == na + 1);			// backed off: 60 ticks now
	now_ticks += 12;
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_PENDING);
	CHECK(nacks == na + 2);
	data(6, 1);
	CHECK(tftp_poll(&n, err, sizeof err) == TFTP_RESULT_OK);

	CHECK(lent_bad == 0);		// no lent block overwritten before its pull
	printf("test_tftp_get: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
