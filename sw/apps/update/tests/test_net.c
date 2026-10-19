/*
 * Zeitlos -- update's mailbox and TFTP source on the host
 * (docs/update.md, "Testing").
 *
 *   make -C sw/apps/update/tests
 *
 * upd_net.c, the real zstream.c and zobj.c, a mailbox per process, and
 * a `net` that answers the way sw/apps/net/tftp.c does: a stream
 * accepted (or refused "busy"), one chunk per pull, the chunk LENT from
 * one of two slots that are reused only after the next pull, and --
 * the case that failed on the board -- the last chunk and the
 * end-of-file sent together, without waiting for a pull. Port acks
 * for the terminal arrive in between, as they do on the board.
 *
 * What it checks: files of 0, 1, 1467, 1468, 1469, 2936, 39410 and
 * 100000 bytes arrive exactly, with net's end-of-file and with one that
 * waits for a pull; every port ack reaches the other hook; "busy" is
 * waited out, and given up on; an error sent without a pull (the
 * server's "file not found"); a net that never answers the open, and
 * one that stops mid-file; a file abandoned half way and another
 * fetched straight after, with the old stream's chunk still in flight.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "upd.h"
#include "upd_net.h"
#include "zstream.h"
#include "zobj.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("  FAIL  "); printf(__VA_ARGS__); printf("  (%s:%d)\n", __FILE__, __LINE__); } } while (0)

#define PID_UPD 7
#define PID_NET 2
#define PID_POSIX 9

// Stands for zport.h's PORT_ACK: any message that is not the stream's.
#define PORT_ACK 0x7A01

// -- the mailbox ---------------------------------------------------------------

#define QLEN 256
typedef struct { z_msg_t m[QLEN]; int head, n; } box_t;
static box_t boxes[16];
static uint32_t running = PID_UPD;
static uint32_t now;

z_rv z_msg_new_send(uint32_t to, uint32_t subject, uint32_t tag, z_obj_t obj) {
	box_t *b = &boxes[to];
	if (b->n == QLEN) return Z_FAIL;
	z_msg_t *m = &b->m[(b->head + b->n++) % QLEN];
	memset(m, 0, sizeof *m);
	m->to = to;
	m->from = running;
	m->subject = subject;
	m->tag = tag;
	m->obj = obj;		// borrowed, as on the board
	return Z_OK;
}

z_rv z_msg_send(z_msg_t *msg) {
	return z_msg_new_send(msg->to, msg->subject, msg->tag, msg->obj);
}

z_rv z_msg_read(z_msg_t *msg) {
	box_t *b = &boxes[running];
	if (!b->n) return Z_FAIL;
	*msg = b->m[b->head];
	b->head = (b->head + 1) % QLEN;
	b->n--;
	return Z_OK;
}

uint32_t z_uptime_ticks(void) { return now; }

z_rv z_msg_recv(z_msg_t *msg, uint32_t timeout) {
	(void)timeout;
	return z_msg_read(msg);
}

static void net_run(void);

// The other processes run while this one sleeps.
void z_proc_wait(uint32_t ticks) {
	now += ticks ? ticks : 1;
	net_run();
}

// -- a net that answers like tftp.c ----------------------------------------------

static struct {
	const uint8_t *data;
	uint32_t len;
	int busy;               // refuse this many opens with "busy"
	bool eof_on_pull;       // EOF only when pulled again (zstream's usual)
	bool error_unasked;     // the server says "file not found" at once
	bool silent_open;       // never answers an open
	int stop_after;         // stop answering pulls after this many chunks (-1: no)
	int acks_between;       // port acks to put in the mailbox per chunk
} sc;

static zstream_producer_t prod;
static bool active, last_sent, done;
static uint32_t off;
static int sent_chunks, opens, acks_sent, aborts;
static uint8_t slot[2][1468];
static z_blob_t blob[2];
static int which;
static char last_name[64];

static void deliver(void) {
	if (sc.stop_after >= 0 && sent_chunks >= sc.stop_after) return;
	uint32_t n = sc.len - off > 1468 ? 1468 : sc.len - off;
	memcpy(slot[which], sc.data + off, n);
	blob[which].data = slot[which];
	blob[which].len = n;
	zstream_send_chunk_ref(&prod, &blob[which]);	// lent, not copied
	which ^= 1;
	off += n;
	sent_chunks++;
	for (int i = 0; i < sc.acks_between; i++, acks_sent++)
		z_msg_new_send(PID_UPD, PORT_ACK, 1, z_obj_none());
	if (n < 1468) {			// the short (or empty) block: the end
		last_sent = true;
		if (!sc.eof_on_pull) {
			zstream_send_eof(&prod);	// with the chunk, as tftp.c does
			done = true;
		}
	}
}

static void net_run(void) {
	uint32_t was = running;
	running = PID_NET;
	z_msg_t m;
	while (z_msg_read(&m) == Z_OK) {
		if (m.subject == Z_STREAM_OPEN) {
			opens++;
			if (sc.silent_open) continue;
			z_obj_t *f = z_map_find(&m.obj, "filename");
			snprintf(last_name, sizeof last_name, "%s",
				f && f->type == Z_STR ? f->val.str : "?");
			if (sc.busy > 0) {
				sc.busy--;
				zstream_reject(m.from, m.tag, "busy: one transfer at a time");
				continue;
			}
			zstream_accept(&prod, m.from, m.tag);
			active = true;
			last_sent = done = false;
			off = 0;
			sent_chunks = 0;
			if (sc.error_unasked) {
				zstream_send_error(&prod, "file not found");
				active = false;
			}
		} else if (m.subject == Z_STREAM_PULL || m.subject == Z_STREAM_ABORT) {
			if (!active || done) continue;	// tftp.c: state != T_GET, ignored
			zstream_event_t ev = zstream_producer_handle(&prod, &m);
			if (ev == ZSTREAM_EVENT_ABORT) {
				aborts++;
				active = false;
				continue;
			}
			if (ev != ZSTREAM_EVENT_PULL) continue;
			if (last_sent) {		// eof_on_pull: the pull after the end
				zstream_send_eof(&prod);
				done = true;
				continue;
			}
			deliver();
		}
	}
	running = was;
}

// -- the update side ---------------------------------------------------------------

static int other_acks;
static void other(z_msg_t *m) {
	if (m->subject == PORT_ACK) other_acks++;
}

static uint8_t *pattern(uint32_t n, uint32_t seed) {
	uint8_t *p = malloc(n ? n : 1);
	for (uint32_t i = 0; i < n; i++) {
		seed = seed * 1103515245u + 12345u;
		p[i] = (uint8_t)(seed >> 16);
	}
	return p;
}

static void reset(void) {
	memset(&sc, 0, sizeof sc);
	sc.stop_after = -1;
	memset(boxes, 0, sizeof boxes);
	active = last_sent = done = false;
	opens = acks_sent = aborts = other_acks = 0;
	now = 0;
}

// Fetches into `out`; returns bytes, or -1 with err.
static long fetch(const char *name, uint8_t *out, uint32_t cap, char *err, int errlen) {
	err[0] = 0;
	if (upd_net_open(name, err, errlen) != UPD_OK) return -1;
	uint8_t buf[UPD_CHUNK_MAX];
	uint32_t got = 0;
	for (;;) {
		int n = upd_net_read(buf, sizeof buf, err, errlen);
		if (n < 0) { upd_net_close(); return -1; }
		if (n == 0) break;
		if (got + n > cap) { snprintf(err, errlen, "too long"); upd_net_close(); return -1; }
		memcpy(out + got, buf, n);
		got += n;
	}
	upd_net_close();
	return got;
}

static void sizes(bool eof_on_pull) {
	static const uint32_t sz[] = { 0, 1, 1467, 1468, 1469, 2936, 39410, 100000 };
	for (unsigned i = 0; i < sizeof sz / sizeof sz[0]; i++) {
		reset();
		uint8_t *data = pattern(sz[i], sz[i] + 1);
		sc.data = data;
		sc.len = sz[i];
		sc.eof_on_pull = eof_on_pull;
		sc.acks_between = 2;
		static uint8_t out[200000];
		char err[96];
		long n = fetch("release/v0.0.7/manifest", out, sizeof out, err, sizeof err);
		CHECK(n == (long)sz[i] && !memcmp(out, data, sz[i]),
			"%u bytes, EOF %s: got %ld (%s)", sz[i],
			eof_on_pull ? "on a pull" : "with the last chunk", n, err);
		CHECK(other_acks == acks_sent, "%u bytes: %d of %d port acks passed on",
			sz[i], other_acks, acks_sent);
		CHECK(!strcmp(last_name, "release/v0.0.7/manifest"), "net was asked for the name");
		free(data);
	}
}

int main(void) {
	upd_net_init(PID_NET, 0xC0A8B266, other, 732);
	char err[96];
	static uint8_t out[200000];

	printf("files of every awkward size\n");
	sizes(false);
	sizes(true);

	printf("busy\n");
	reset();
	uint8_t *d = pattern(5000, 3);
	sc.data = d; sc.len = 5000; sc.busy = 3;
	long n = fetch("release/x", out, sizeof out, err, sizeof err);
	CHECK(n == 5000 && opens == 4, "waited out three refusals (%ld, %d opens)", n, opens);
	reset();
	sc.data = d; sc.len = 5000; sc.busy = 1000;
	n = fetch("release/x", out, sizeof out, err, sizeof err);
	CHECK(n < 0 && strstr(err, "busy") && opens > 5 && opens < 40,
		"given up on after a while (%d opens): %s", opens, err);

	printf("errors\n");
	reset();
	sc.data = d; sc.len = 5000; sc.error_unasked = true;
	n = fetch("release/o/aa/missing", out, sizeof out, err, sizeof err);
	CHECK(n < 0 && strstr(err, "file not found"), "an error sent without a pull: %s", err);
	reset();
	sc.silent_open = true;
	n = fetch("release/x", out, sizeof out, err, sizeof err);
	CHECK(n < 0 && strstr(err, "did not answer"), "a net that never answers: %s", err);
	reset();
	sc.data = d; sc.len = 5000; sc.stop_after = 2;
	n = fetch("release/x", out, sizeof out, err, sizeof err);
	CHECK(n < 0 && strstr(err, "timed out"), "a net that stops mid-file: %s", err);
	net_run();		// net reads the abort when it next runs
	CHECK(aborts == 1, "and the stream was aborted (%d)", aborts);

	printf("abandoned half way, then another\n");
	reset();
	uint8_t *big = pattern(50000, 9);
	sc.data = big; sc.len = 50000;
	uint8_t buf[UPD_CHUNK_MAX];
	CHECK(upd_net_open("release/a", err, sizeof err) == UPD_OK, "open a");
	CHECK(upd_net_read(buf, sizeof buf, err, sizeof err) == 1468, "one chunk of a");
	// a pull for the second chunk is out; net answers it before it
	// sees the abort, so that chunk is in flight when b opens
	net_run();
	upd_net_close();
	uint8_t *small = pattern(3000, 11);
	sc.data = small; sc.len = 3000;
	n = fetch("release/b", out, sizeof out, err, sizeof err);
	CHECK(n == 3000 && !memcmp(out, small, 3000),
		"b arrives whole, a's stray chunk ignored (%ld: %s)", n, err);

	free(d); free(big); free(small);
	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
