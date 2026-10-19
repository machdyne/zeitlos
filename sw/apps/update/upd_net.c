/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * update's mailbox and its TFTP source. See upd_net.h.
 *
 * -- why a queue --
 *
 * The first version kept one pending event, and dropped a stream
 * message that arrived while one was waiting. On the board that lost
 * every file's end: sw/apps/net/tftp.c's try_deliver_get() sends the
 * last chunk and then, in the same breath, the end-of-file -- it does
 * not wait for another pull, since there is nothing left to pull. The
 * pump drained both in one pass, kept the chunk, dropped the EOF, and
 * the next pull went to a net that had finished: "timed out" after a
 * transfer net had reported complete. zstream_pull() never meets this:
 * it stops reading at the first message that is its own.
 *
 * Events are queued in arrival order, the consumer's sequence numbers
 * having already been advanced by zstream_consumer_handle(). A chunk's
 * bytes are copied into its slot at once: they are borrowed from net
 * only until this process reads its next message.
 */

#include <stdio.h>
#include <string.h>

#include "upd_net.h"
#include "upd.h"
#include "zstream.h"
#include "zobj.h"

z_rv z_msg_read(z_msg_t *msg);
void z_proc_wait(uint32_t timeout_ticks);
uint32_t z_uptime_ticks(void);

#define QMAX 4                  // a chunk, an EOF, and room to spare
#define BUSY_TRIES 20           // net busy with another transfer: ~10 s

typedef struct {
	int ev;                     // ZSTREAM_CEVENT_*
	uint32_t len;
	char err[96];
	uint8_t data[UPD_CHUNK_MAX];
} event_t;

static zstream_consumer_t cons;
static bool watching;           // a stream is opening or open
static event_t q[QMAX];
static int q_head, q_n;
static bool q_overflow;
static uint32_t net_pid, server_ip, hz = 732;
static void (*other)(z_msg_t *m);

void upd_net_init(uint32_t np, uint32_t ip, void (*oth)(z_msg_t *m), uint32_t tick_hz) {
	net_pid = np;
	server_ip = ip;
	other = oth;
	hz = tick_hz ? tick_hz : 732;
}

static void q_clear(void) {
	q_head = q_n = 0;
	q_overflow = false;
}

static void dispatch(z_msg_t *m) {
	if (watching) {
		event_t *e = q_n < QMAX ? &q[(q_head + q_n) % QMAX] : NULL;
		static event_t spill;
		event_t *t = e ? e : &spill;
		const uint8_t *d = NULL;
		uint32_t n = 0;
		t->err[0] = 0;
		int ev = zstream_consumer_handle(&cons, m, &d, &n, t->err, sizeof t->err);
		if (ev != ZSTREAM_CEVENT_NONE) {
			if (!e) {
				// Cannot happen with one pull outstanding; said
				// rather than lost if it ever does.
				q_overflow = true;
				return;
			}
			e->ev = ev;
			e->len = 0;
			if (ev == ZSTREAM_CEVENT_CHUNK) {
				if (n > sizeof e->data) {
					snprintf(e->err, sizeof e->err, "a chunk of %lu bytes",
						(unsigned long)n);
					e->ev = ZSTREAM_CEVENT_ERROR;
				} else {
					if (n) memcpy(e->data, d, n);
					e->len = n;
				}
			}
			q_n++;
			return;
		}
	}
	if (other) other(m);
}

void upd_net_pump(uint32_t wait) {
	z_msg_t m;
	if (z_msg_read(&m) != Z_OK) {
		if (!wait) return;
		z_proc_wait(wait);
		if (z_msg_read(&m) != Z_OK) return;
	}
	do dispatch(&m); while (z_msg_read(&m) == Z_OK);
}

// The next event, waiting up to `ticks`. 0 if none came.
static int next_event(uint32_t ticks, event_t **out) {
	uint32_t t0 = z_uptime_ticks();
	while (!q_n) {
		if (z_uptime_ticks() - t0 > ticks) return 0;
		upd_net_pump(hz / 50 ? hz / 50 : 1);
	}
	*out = &q[q_head];
	q_head = (q_head + 1) % QMAX;
	q_n--;
	return (*out)->ev;
}

int upd_net_open(const char *name, char *err, int errlen) {
	upd_net_close();
	for (int tries = 0; ; tries++) {
		z_obj_t req = z_obj_map(2);
		z_map_set(&req, "ip", z_obj_uint32(server_ip));
		// z_map_set() stores a copy of the value (zobj.c): free ours.
		z_obj_t fname = z_obj_str(name);
		z_map_set(&req, "filename", fname);
		z_obj_free(&fname);

		q_clear();
		watching = true;
		zstream_open_async(&cons, net_pid, req);
		event_t *e = NULL;
		int ev = next_event(hz * 10, &e);
		// The request is borrowed until net has read it (messaging.md,
		// "Payload lifetime"); its reply says it has -- net copies the
		// name before answering. Without one it stays allocated, since
		// net might still read it.
		if (ev) z_obj_free(&req);
		if (ev == ZSTREAM_CEVENT_OPENED) {
			zstream_pull_async(&cons);
			return UPD_OK;
		}
		watching = false;
		// net does one transfer at a time; a tget elsewhere makes it
		// say so. Wait for it rather than fail the whole update.
		if (ev == ZSTREAM_CEVENT_OPEN_FAILED && strstr(e->err, "busy") &&
				tries < BUSY_TRIES) {
			uint32_t t1 = z_uptime_ticks();
			while (z_uptime_ticks() - t1 < hz / 2) upd_net_pump(hz / 50 ? hz / 50 : 1);
			continue;
		}
		snprintf(err, errlen, "%s", !ev ? "net did not answer" :
			e->err[0] ? e->err : "refused");
		return UPD_ERR;
	}
}

int upd_net_read(uint8_t *buf, int cap, char *err, int errlen) {
	if (!watching) {
		snprintf(err, errlen, "not open");
		return -1;
	}
	if (q_overflow) {
		snprintf(err, errlen, "more stream messages than expected");
		upd_net_close();
		return -1;
	}
	event_t *e = NULL;
	// net's own retries to the server come first; this is for a net
	// that stopped answering altogether.
	int ev = next_event(hz * 25, &e);
	if (!ev) {
		snprintf(err, errlen, "timed out");
		upd_net_close();
		return -1;
	}
	if (ev == ZSTREAM_CEVENT_CHUNK) {
		if ((int)e->len > cap) {
			snprintf(err, errlen, "a chunk larger than the buffer");
			upd_net_close();
			return -1;
		}
		memcpy(buf, e->data, e->len);
		// The next while this one is written. Not after an end
		// already queued: the consumer is no longer active then, and
		// zstream_pull_async() sends nothing.
		zstream_pull_async(&cons);
		if (e->len) return (int)e->len;
		// An empty chunk (net delivers the empty last block of a file
		// whose size is a multiple of the block size): the EOF follows.
		return upd_net_read(buf, cap, err, errlen);
	}
	watching = false;
	if (ev == ZSTREAM_CEVENT_EOF) return 0;
	snprintf(err, errlen, "%s", e->err[0] ? e->err : "transfer failed");
	return -1;
}

void upd_net_close(void) {
	if (watching && cons.active) zstream_abort(&cons);
	watching = false;
	q_clear();
}
