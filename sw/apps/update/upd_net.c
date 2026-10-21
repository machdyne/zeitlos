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
#include <stdlib.h>
#include <string.h>

#include "upd_net.h"
#include "upd.h"
#include "zstream.h"
#include "zobj.h"
#include "zport.h"      /* Z_PORT_* subjects: net's raw sockets */

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
static bool http;

void upd_net_init(uint32_t np, uint32_t ip, void (*oth)(z_msg_t *m), uint32_t tick_hz) {
	net_pid = np;
	server_ip = ip;
	other = oth;
	hz = tick_hz ? tick_hz : 732;
	http = false;
}

static bool http_dispatch(z_msg_t *m);

static void q_clear(void) {
	q_head = q_n = 0;
	q_overflow = false;
}

static void dispatch(z_msg_t *m) {
	if (http && http_dispatch(m)) return;
	if (watching && !http) {
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

static int http_open(const char *name, char *err, int errlen);
static int http_read(uint8_t *buf, int cap, char *err, int errlen);
static void http_close(void);

int upd_net_open(const char *name, char *err, int errlen) {
	if (http) return http_open(name, err, errlen);
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
	if (http) return http_read(buf, cap, err, errlen);
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
	if (http) { http_close(); return; }
	if (watching && cons.active) zstream_abort(&cons);
	watching = false;
	q_clear();
}

// -- HTTP -------------------------------------------------------------------
//
// Plain HTTP/1.1 over one of net's raw TCP sockets (sw/apps/net/sock.h):
// Z_PORT_CONNECT with {ip, port}, then Z_PORT_DATA both ways. Trust is
// not the transport's: the manifest is signed and every file checked
// against it (docs/distribution.md), so the bytes may come from any
// server, mirror or proxy.
//
// One connection, kept open from file to file (keep-alive): a card is
// hundreds of files, and a connection costs a round trip or three. A
// server that closes it -- between files, or a "Connection: close" --
// is reconnected to at the next file. A file cut off half way is the
// caller's to retry (upd.c fetches a file twice before giving up).
//
// What comes back is a status line, headers, and a body framed by
// Content-Length, by chunks (Transfer-Encoding: chunked), or by the
// end of the connection. A redirect is not followed: to https:// it
// could not be, and to elsewhere it is something to fix in the
// configuration, said rather than done silently.
//
// -- flow --
//
// net relays what TCP brings in pieces of up to 4 KB (SOCK_CHUNK), at
// most Z_PORT_MAX_PENDING_SENDS of them unacknowledged. The bytes are
// copied into the ring at once -- they are borrowed only until the next
// message is read -- but each piece is acknowledged only once the ring
// has been read past it, so net holds back (and TCP's window closes)
// while the card is being written, and the ring cannot overflow.

#define H_RING      (Z_PORT_MAX_PENDING_SENDS * 4096 + 4096)
#define H_HDR_MAX   4096
#define H_ACKS      (Z_PORT_MAX_PENDING_SENDS * 2)

static uint16_t h_port;
static char h_host[80];
static uint32_t h_conn;                 // conn_id, net's for this connection
static bool h_up, h_connecting, h_closed, h_refused;
static char h_refusal[64];
static uint8_t h_ring[H_RING];
static uint32_t h_rd, h_wr;             // stream offsets (ring index: % H_RING)
static bool h_overflow;
static struct { uint32_t from, tag, end; } h_ack[H_ACKS];
static int h_nack;
static z_obj_t h_req;                   // our request, until net acks it
static bool h_req_out;

// the response being read
enum { B_NONE, B_LENGTH, B_CHUNKED, B_CLOSE };
static int h_body;
static uint32_t h_left;                 // B_LENGTH: bytes; B_CHUNKED: of this chunk
static int h_chunk_state;               // 0 size line, 1 data, 2 CRLF after data, 3 trailer
static bool h_in_file, h_done;

void upd_net_init_http(uint32_t np, uint32_t ip, uint16_t port, const char *host,
	void (*oth)(z_msg_t *m), uint32_t tick_hz) {
	upd_net_init(np, ip, oth, tick_hz);
	http = true;
	h_port = port ? port : 80;
	snprintf(h_host, sizeof h_host, "%s", host ? host : "");
	h_up = h_connecting = h_closed = false;
}

static void h_send_acks(void) {
	int k = 0;
	while (k < h_nack && h_ack[k].end <= h_rd) {
		z_msg_new_send(h_ack[k].from, Z_PORT_DATA_ACK, h_ack[k].tag, z_obj_none());
		k++;
	}
	if (k) {
		memmove(h_ack, h_ack + k, (h_nack - k) * sizeof h_ack[0]);
		h_nack -= k;
	}
}

// Everything still unacknowledged, acknowledged: net must not be left
// holding a connection's sends when it is given up.
static void h_ack_all(void) {
	h_rd = h_wr;
	h_send_acks();
}

static bool http_dispatch(z_msg_t *m) {
	if (m->from != net_pid) return false;
	switch (m->subject) {
	case Z_PORT_CONNECTED:
		if (h_connecting && m->obj.type == Z_UINT32) {
			h_conn = m->obj.val.uint32;
			h_up = true;
			h_connecting = false;
			h_closed = false;
		} else if (m->obj.type == Z_UINT32) {
			// one given up on: let it go
			z_msg_new_send(m->from, Z_PORT_CLOSE, m->obj.val.uint32, z_obj_none());
		}
		return true;
	case Z_PORT_REFUSED:
		if (h_connecting) {
			h_connecting = false;
			h_refused = true;
			snprintf(h_refusal, sizeof h_refusal, "%s",
				m->obj.type == Z_STR && m->obj.val.str ? m->obj.val.str : "refused");
		}
		return true;
	case Z_PORT_DATA: {
		uint32_t n = z_blob_len(&m->obj);
		const uint8_t *d = z_blob_data(&m->obj);
		if (!h_up || m->tag != h_conn || h_nack >= H_ACKS || h_wr - h_rd + n > H_RING) {
			// not this connection's, or more than net may have
			// outstanding: acked, and noted if it was ours
			if (h_up && m->tag == h_conn) h_overflow = true;
			z_msg_new_send(m->from, Z_PORT_DATA_ACK, m->tag, z_obj_none());
			return true;
		}
		for (uint32_t i = 0; i < n; i++) h_ring[(h_wr + i) % H_RING] = d[i];
		h_wr += n;
		h_ack[h_nack].from = m->from;
		h_ack[h_nack].tag = m->tag;
		h_ack[h_nack].end = h_wr;
		h_nack++;
		return true;
	}
	case Z_PORT_DATA_ACK:
		// for our request; the terminal's come from its own pid
		if (h_req_out) {
			z_obj_free(&h_req);
			h_req_out = false;
		}
		return true;
	case Z_PORT_CLOSE:
		if (h_up && m->tag == h_conn) {
			h_up = false;
			h_closed = true;
		}
		return true;
	default:
		return false;
	}
}

// Waits up to `ticks` for `cond`, pumping the mailbox.
#define H_WAIT(cond, ticks) do { \
	uint32_t t0_ = z_uptime_ticks(); \
	while (!(cond) && z_uptime_ticks() - t0_ <= (ticks)) \
		upd_net_pump(hz / 50 ? hz / 50 : 1); \
} while (0)

static void h_drop(void) {
	if (h_up) z_msg_new_send(net_pid, Z_PORT_CLOSE, h_conn, z_obj_none());
	h_up = false;
	h_ack_all();
	h_rd = h_wr = 0;
	h_nack = 0;
}

static int h_connect(char *err, int errlen) {
	z_obj_t arg = z_obj_map(2);
	z_map_set(&arg, "ip", z_obj_uint32(server_ip));
	z_map_set(&arg, "port", z_obj_uint32(h_port));
	h_connecting = true;
	h_refused = false;
	h_rd = h_wr = 0;
	h_nack = 0;
	h_overflow = false;
	z_msg_new_send(net_pid, Z_PORT_CONNECT, 0, arg);
	H_WAIT(!h_connecting, hz * 15);
	// net has read the request once it answered; unanswered, it might
	// still, so it is kept (as upd_net_open() does with TFTP's).
	if (!h_connecting) z_obj_free(&arg);
	if (h_up) return UPD_OK;
	if (h_refused)
		snprintf(err, errlen, "cannot connect: %s", h_refusal);
	else {
		h_connecting = false;
		snprintf(err, errlen, "no connection (net did not answer)");
	}
	return UPD_ERR;
}

// One byte of the response, waiting for it. -1: the connection ended,
// -2: nothing came in time.
static int h_byte(void) {
	if (h_rd == h_wr) {
		if (!h_up) return -1;
		H_WAIT(h_rd != h_wr || !h_up, hz * 25);
		if (h_rd == h_wr) return h_up ? -2 : -1;
	}
	int c = h_ring[h_rd % H_RING];
	h_rd++;
	if (h_nack && h_ack[0].end <= h_rd) h_send_acks();
	return c;
}

static bool h_hdr_is(const char *line, const char *name, const char **val) {
	size_t n = strlen(name);
	for (size_t i = 0; i < n; i++) {
		char a = line[i], b = name[i];
		if (a >= 'A' && a <= 'Z') a += 32;
		if (a != b) return false;
	}
	if (line[n] != ':') return false;
	const char *v = line + n + 1;
	while (*v == ' ' || *v == '\t') v++;
	*val = v;
	return true;
}

static bool h_contains_ci(const char *s, const char *word) {
	size_t n = strlen(word);
	for (; *s; s++) {
		size_t i = 0;
		while (i < n && s[i] && ((s[i] | 32) == word[i])) i++;
		if (i == n) return true;
	}
	return false;
}

static int h_request(const char *name, char *err, int errlen) {
	// one request outstanding: the last one's ack first
	if (h_req_out) H_WAIT(!h_req_out, hz * 5);
	if (h_req_out) {
		snprintf(err, errlen, "net did not take the request");
		return UPD_ERR;
	}
	char req[512];
	char hostport[96];
	if (h_port == 80) snprintf(hostport, sizeof hostport, "%s", h_host);
	else snprintf(hostport, sizeof hostport, "%s:%u", h_host, (unsigned)h_port);
	int n = snprintf(req, sizeof req,
		"GET /%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: zeitlos-update\r\n"
		"Accept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n", name, hostport);
	if (n <= 0 || n >= (int)sizeof req) {
		snprintf(err, errlen, "name too long");
		return UPD_ERR;
	}
	h_req = z_obj_blob(req, (uint32_t)n);       // copied: req may go
	if (h_req.type != Z_BLOB) {
		snprintf(err, errlen, "out of memory");
		return UPD_ERR;
	}
	h_req_out = true;
	if (z_msg_new_send(net_pid, Z_PORT_DATA, h_conn, h_req) != Z_OK) {
		z_obj_free(&h_req);
		h_req_out = false;
		snprintf(err, errlen, "net is not taking messages");
		return UPD_ERR;
	}
	return UPD_OK;
}

// The status line and headers. 0: a body follows; >0 the HTTP status
// otherwise (err says it); <0 the connection failed (retry on a new one).
static int h_response(char *err, int errlen) {
	static char line[H_HDR_MAX];
	int status = 0, nlines = 0;
	bool close_after = false, have_len = false, chunked = false;
	uint32_t clen = 0;
	char location[160];
	location[0] = 0;
	for (;;) {
		int n = 0, c;
		while ((c = h_byte()) >= 0 && c != '\n')
			if (n < (int)sizeof line - 1 && c != '\r') line[n++] = (char)c;
		line[n] = 0;
		if (c < 0) {
			snprintf(err, errlen, c == -2 ? "the server did not answer" :
				nlines ? "the connection closed in the headers" :
				"the connection closed");
			return -1;
		}
		if (!nlines++) {
			// HTTP/1.1 200 OK
			if (strncmp(line, "HTTP/1.", 7) || !line[8] || line[8] != ' ') {
				snprintf(err, errlen, "not an HTTP server");
				return 400;
			}
			status = atoi(line + 9);
			if (line[7] == '0') close_after = true;     // HTTP/1.0
			continue;
		}
		if (!n) break;
		const char *v;
		if (h_hdr_is(line, "content-length", &v)) {
			have_len = true;
			clen = (uint32_t)strtoul(v, NULL, 10);
		} else if (h_hdr_is(line, "transfer-encoding", &v)) {
			if (h_contains_ci(v, "chunked")) chunked = true;
		} else if (h_hdr_is(line, "connection", &v)) {
			if (h_contains_ci(v, "close")) close_after = true;
		} else if (h_hdr_is(line, "location", &v)) {
			snprintf(location, sizeof location, "%s", v);
		}
	}
	h_body = chunked ? B_CHUNKED : have_len ? B_LENGTH : B_CLOSE;
	h_left = have_len ? clen : 0;
	h_chunk_state = 0;
	h_done = h_body == B_LENGTH && clen == 0;
	h_in_file = true;
	if (close_after && h_body != B_CLOSE) h_closed = true;   // after this one
	if (status == 200) return 0;
	if (status >= 300 && status < 400) {
		if (!strncmp(location, "https:", 6))
			snprintf(err, errlen, "the server redirects to https (%.60s): update "
				"needs plain HTTP; signatures, not TLS, keep it safe "
				"(docs/distribution.md)", location);
		else
			snprintf(err, errlen, "redirected (%d) to %.80s: not followed -- "
				"set system.update.url to it", status, location[0] ? location : "?");
	} else if (status == 404) {
		snprintf(err, errlen, "not on the server (404)");
	} else {
		snprintf(err, errlen, "the server says %d", status);
	}
	return status ? status : 500;
}

static int http_open(const char *name, char *err, int errlen) {
	if (h_in_file) http_close();
	for (int attempt = 0; attempt < 2; attempt++) {
		if (h_closed || !h_up) {
			h_drop();
			h_closed = false;
			if (h_connect(err, errlen) != UPD_OK) return UPD_ERR;
		}
		if (h_request(name, err, errlen) != UPD_OK) return UPD_ERR;
		int r = h_response(err, errlen);
		if (r == 0) return UPD_OK;
		if (r > 0) {
			// an answer, not a broken connection: the body (an error
			// page) is not read, so this connection is done
			h_in_file = false;
			h_drop();
			return UPD_ERR;
		}
		// The connection failed before an answer: a server that closed
		// a kept-alive connection as the request went out. Once more,
		// on a new one.
		h_in_file = false;
		h_drop();
	}
	return UPD_ERR;
}

static int http_read(uint8_t *buf, int cap, char *err, int errlen) {
	if (!h_in_file) {
		snprintf(err, errlen, "not open");
		return -1;
	}
	if (h_overflow) {
		snprintf(err, errlen, "net sent more than it may");
		http_close();
		return -1;
	}
	int n = 0;
	while (n < cap && !h_done) {
		if (h_body == B_CHUNKED && h_chunk_state != 1) {
			// the framing: "1a2b[;ext]\r\n", data, "\r\n", ..., "0\r\n\r\n"
			char line[64];
			int ln = 0, c;
			while ((c = h_byte()) >= 0 && c != '\n')
				if (ln < (int)sizeof line - 1 && c != '\r') line[ln++] = (char)c;
			line[ln] = 0;
			if (c < 0) goto cut;
			if (h_chunk_state == 2) { h_chunk_state = 0; continue; }
			if (h_chunk_state == 3) {
				if (!ln) h_done = true;     // the end of the trailer
				continue;
			}
			h_left = (uint32_t)strtoul(line, NULL, 16);
			h_chunk_state = h_left ? 1 : 3;
			continue;
		}
		if (h_body != B_CLOSE && !h_left) {
			if (h_body == B_LENGTH) { h_done = true; break; }
			h_chunk_state = 2;
			continue;
		}
		// data, from what is in the ring
		if (h_rd == h_wr) {
			if (n) break;                   // what we have first
			int c = h_byte();               // waits
			if (c < 0) {
				if (h_body == B_CLOSE && c == -1) { h_done = true; break; }
				goto cut;
			}
			buf[n++] = (uint8_t)c;
			if (h_body != B_CLOSE) h_left--;
			continue;
		}
		uint32_t have = h_wr - h_rd;
		uint32_t take = (uint32_t)(cap - n);
		if (take > have) take = have;
		if (h_body != B_CLOSE && take > h_left) take = h_left;
		for (uint32_t i = 0; i < take; i++) buf[n + i] = h_ring[(h_rd + i) % H_RING];
		h_rd += take;
		n += (int)take;
		if (h_body != B_CLOSE) h_left -= take;
		h_send_acks();
	}
	if (h_done && !n) {
		h_in_file = false;
		if (h_body == B_CLOSE) h_closed = true;
		return 0;
	}
	return n;
cut:
	snprintf(err, errlen, "the connection ended in the middle of the file");
	h_in_file = false;
	h_drop();
	return -1;
}

static void http_close(void) {
	// A file not read to its end leaves the rest of its body on the
	// connection: a new connection is simpler than reading it away.
	if (h_in_file && !h_done) h_drop();
	h_in_file = false;
	if (h_closed) h_drop();
}
