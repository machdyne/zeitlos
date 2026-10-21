/*
 * Zeitlos -- update's HTTP source on the host (docs/update.md,
 * "Testing"; docs/distribution.md).
 *
 *   make -C sw/apps/update/tests
 *
 * upd_net.c's HTTP client, the real zobj.c, a mailbox per process, and
 * a `net` whose raw sockets behave like sw/apps/net/net.c's: CONNECT
 * answered CONNECTED with a connection id (or REFUSED), the client's
 * request taken as DATA and acknowledged, the server's bytes relayed in
 * pieces of at most 4 KB with no more than Z_PORT_MAX_PENDING_SENDS of
 * them unacknowledged, CLOSE when the server hangs up. Behind it, a web
 * server serving a byte pattern, as nginx does, or as the cases need.
 *
 * What it checks: files of 0, 1, 4095, 4096, 4097 and 300000 bytes
 * arrive exactly, by Content-Length and by chunks, in reads of any
 * size; one connection for many files (keep-alive), and a new one when
 * the server closes it (Connection: close, HTTP/1.0, or a hang-up
 * between files); a body ended by the close; the request line and Host
 * header; 404, a redirect to https and one elsewhere, said; a connection
 * refused, a net that never answers, a server that hangs up mid-file; a
 * file abandoned half way, and the next one on a new connection; and
 * that net never has more than its limit of pieces unacknowledged.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "upd.h"
#include "upd_net.h"
#include "zobj.h"
#include "zport.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
	printf("  FAIL  "); printf(__VA_ARGS__); printf("  (%s:%d)\n", __FILE__, __LINE__); } } while (0)

#define PID_UPD 7
#define PID_NET 2

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

static void net_run(void);

void z_proc_wait(uint32_t ticks) {
	now += ticks ? ticks : 1;
	net_run();
}

// zstream.c is linked for upd_net.c's TFTP half; not used here.
z_rv z_msg_recv(z_msg_t *msg, uint32_t timeout) {
	(void)timeout;
	return z_msg_read(msg);
}

// -- a net with raw sockets, and a web server behind it --------------------------

static struct {
	uint32_t len;               // the file's size
	bool chunked;               // Transfer-Encoding: chunked
	bool no_length;             // neither: the body ends with the close
	bool http10;                // HTTP/1.0 (closes after each)
	int keep;                   // requests a connection serves, then closes (0: no limit)
	bool hangup_idle;           // closes the connection after each response, unsaid
	int status;                 // 200 unless set
	const char *location;
	int drop_at;                // hang up after this many body bytes (-1: no)
	bool refuse, silent;        // the CONNECT: refused, or never answered
	uint32_t piece;             // the most per DATA (default 4096)
	int ignore_requests;        // hang up on this many requests, unanswered
} sc;

static uint32_t conn_id = 100, live = 0;   // live: the open connection's id, 0 none
static int connects, requests, closes_by_client;
static char last_req[1024];
static int req_len;
static uint8_t *pattern_buf;

// The server's response, queued for the client.
static uint8_t *outq;
static uint32_t out_len, out_off;
static bool close_after;            // hang up once outq is sent
static int pending;                 // DATA sent and not acked
static int max_pending;
static z_obj_t inflight[64];
static int in_head, in_n;

static uint8_t pat(uint32_t i) { return (uint8_t)(i * 7 + (i >> 8) * 13 + 1); }

static void q_add(const void *p, uint32_t n) {
	outq = realloc(outq, out_len + n + 1);
	memcpy(outq + out_len, p, n);
	out_len += n;
}

static void q_str(const char *s) { q_add(s, (uint32_t)strlen(s)); }

static void respond(void) {
	requests++;
	char h[512];
	const char *ver = sc.http10 ? "HTTP/1.0" : "HTTP/1.1";
	int status = sc.status ? sc.status : 200;
	bool last = sc.http10 || (sc.keep && requests % sc.keep == 0);
	if (status != 200) {
		snprintf(h, sizeof h, "%s %d Something\r\nContent-Length: 9\r\n%s%s%s\r\n"
			"not here\n", ver, status,
			sc.location ? "Location: " : "", sc.location ? sc.location : "",
			sc.location ? "\r\n" : "");
		q_str(h);
		return;
	}
	if (sc.chunked) {
		snprintf(h, sizeof h, "%s 200 OK\r\nServer: fake\r\nTransfer-Encoding: chunked\r\n%s\r\n",
			ver, last ? "Connection: close\r\n" : "");
		q_str(h);
		uint32_t off = 0, step = 1;
		while (off < sc.len) {
			uint32_t n = sc.len - off < step ? sc.len - off : step;
			snprintf(h, sizeof h, "%lx%s\r\n", (unsigned long)n, step == 3 ? ";ext=1" : "");
			q_str(h);
			q_add(pattern_buf + off, n);
			q_str("\r\n");
			off += n;
			step = step * 3 + 1;       // 1, 4, 13, 40, ... sizes of every kind
		}
		q_str("0\r\nX-Trailer: yes\r\n\r\n");
	} else if (sc.no_length) {
		snprintf(h, sizeof h, "%s 200 OK\r\n\r\n", ver);
		q_str(h);
		q_add(pattern_buf, sc.len);
		last = true;
	} else {
		snprintf(h, sizeof h, "%s 200 OK\r\nContent-Type: application/octet-stream\r\n"
			"Content-Length: %lu\r\n%s\r\n", ver, (unsigned long)sc.len,
			last ? "Connection: close\r\n" : "");
		q_str(h);
		q_add(pattern_buf, sc.len);
	}
	if (last || sc.hangup_idle) close_after = true;
}

static void hang_up(void) {
	if (!live) return;
	z_msg_new_send(PID_UPD, Z_PORT_CLOSE, live, z_obj_none());
	live = 0;
	out_len = out_off = 0;
	close_after = false;
}

static void push(void) {
	uint32_t piece = sc.piece ? sc.piece : 4096;
	while (live && out_off < out_len && pending < Z_PORT_MAX_PENDING_SENDS) {
		uint32_t n = out_len - out_off > piece ? piece : out_len - out_off;
		// a hang-up partway: what is before it, then CLOSE
		if (sc.drop_at >= 0) {
			uint32_t body_start = 0;
			for (uint32_t i = 0; i + 3 < out_len; i++)
				if (!memcmp(outq + i, "\r\n\r\n", 4)) { body_start = i + 4; break; }
			uint32_t stop = body_start + (uint32_t)sc.drop_at;
			if (out_off >= stop) { hang_up(); return; }
			if (out_off + n > stop) n = stop - out_off;
		}
		z_obj_t b = z_obj_blob(outq + out_off, n);
		inflight[(in_head + in_n++) % 64] = b;
		z_msg_new_send(PID_UPD, Z_PORT_DATA, live, b);
		out_off += n;
		pending++;
		if (pending > max_pending) max_pending = pending;
	}
	// as net does: CLOSE after the last of the data, in order
	if (live && out_off == out_len && close_after) hang_up();
	if (out_off == out_len) { out_len = out_off = 0; }
}

static void net_run(void) {
	uint32_t was = running;
	running = PID_NET;
	z_msg_t m;
	while (z_msg_read(&m) == Z_OK) {
		if (m.subject == Z_PORT_CONNECT) {
			connects++;
			if (sc.silent) continue;
			if (sc.refuse) {
				z_msg_new_send(m.from, Z_PORT_REFUSED, m.tag, z_obj_str("connection refused"));
				continue;
			}
			z_obj_t *ip = z_map_find(&m.obj, "ip");
			z_obj_t *port = z_map_find(&m.obj, "port");
			CHECK(ip && ip->type == Z_UINT32 && ip->val.uint32 == 0x5DB8D822 &&
				port && port->type == Z_UINT32 && port->val.uint32 == 8080,
				"CONNECT carries the server's address and port");
			if (live) hang_up();
			live = ++conn_id;
			req_len = 0;
			z_msg_new_send(m.from, Z_PORT_CONNECTED, 0, z_obj_uint32(live));
		} else if (m.subject == Z_PORT_DATA) {
			uint32_t n = z_blob_len(&m.obj);
			const uint8_t *d = z_blob_data(&m.obj);
			z_msg_new_send(m.from, Z_PORT_DATA_ACK, m.tag, z_obj_none());
			if (m.tag != live) continue;
			if (req_len + n < sizeof last_req) {
				memcpy(last_req + req_len, d, n);
				req_len += n;
				last_req[req_len] = 0;
			}
			if (strstr(last_req, "\r\n\r\n")) {
				req_len = 0;
				if (sc.ignore_requests > 0) {
					// a kept-alive connection the server closed as the
					// request went out
					sc.ignore_requests--;
					requests++;
					hang_up();
					continue;
				}
				respond();
			}
		} else if (m.subject == Z_PORT_DATA_ACK) {
			if (pending > 0) {
				pending--;
				z_obj_free(&inflight[in_head]);
				in_head = (in_head + 1) % 64;
				in_n--;
			}
		} else if (m.subject == Z_PORT_CLOSE) {
			if (m.tag == live) {
				closes_by_client++;
				live = 0;
				out_len = out_off = 0;
				close_after = false;
			}
		}
	}
	push();
	running = was;
}

// -- the update side ---------------------------------------------------------------

static void other(z_msg_t *m) { (void)m; }

static void reset(void) {
	memset(&sc, 0, sizeof sc);
	sc.drop_at = -1;
}

// Fetches into `out` in reads of `step` bytes; bytes, or -1 with err.
static long fetch(const char *name, uint8_t *out, uint32_t cap, int step,
	char *err, int errlen) {
	err[0] = 0;
	if (upd_net_open(name, err, errlen) != UPD_OK) return -1;
	uint8_t buf[UPD_CHUNK_MAX];
	uint32_t got = 0;
	for (;;) {
		int n = upd_net_read(buf, step, err, errlen);
		if (n < 0) { upd_net_close(); return -1; }
		if (n == 0) break;
		if (got + n > cap) { snprintf(err, errlen, "too long"); upd_net_close(); return -1; }
		memcpy(out + got, buf, n);
		got += n;
	}
	upd_net_close();
	return got;
}

static bool same(const uint8_t *p, uint32_t n) {
	for (uint32_t i = 0; i < n; i++)
		if (p[i] != pat(i)) return false;
	return true;
}

int main(void) {
	static uint8_t out[400000];
	char err[160];
	pattern_buf = malloc(400000);
	for (uint32_t i = 0; i < 400000; i++) pattern_buf[i] = pat(i);
	upd_net_init_http(PID_NET, 0x5DB8D822, 8080, "zeitlos.example.org", other, 732);

	printf("files, by Content-Length and by chunks\n");
	static const uint32_t sz[] = { 0, 1, 4095, 4096, 4097, 300000 };
	static const int steps[] = { 1, 7, 1468, 2048 };
	for (int ch = 0; ch < 2; ch++)
		for (unsigned i = 0; i < sizeof sz / sizeof sz[0]; i++)
			for (unsigned k = 0; k < sizeof steps / sizeof steps[0]; k++) {
				if (sz[i] > 5000 && steps[k] < 1000) continue;
				reset();
				sc.len = sz[i];
				sc.chunked = ch;
				long n = fetch("release/v0.0.7/manifest", out, sizeof out, steps[k],
					err, sizeof err);
				CHECK(n == (long)sz[i] && same(out, sz[i]), "%u bytes, %s, reads of %d: "
					"got %ld (%s)", sz[i], ch ? "chunked" : "Content-Length", steps[k],
					n, err);
			}
	CHECK(strstr(last_req, "GET /release/v0.0.7/manifest HTTP/1.1\r\n") &&
		strstr(last_req, "\r\nHost: zeitlos.example.org:8080\r\n"),
		"the request line and the Host header");
	CHECK(max_pending <= Z_PORT_MAX_PENDING_SENDS && max_pending > 1,
		"net never had more than %d pieces unacknowledged (%d)",
		Z_PORT_MAX_PENDING_SENDS, max_pending);

	printf("one connection for many files\n");
	reset();
	sc.len = 10000;
	int c0 = connects;
	for (int i = 0; i < 20; i++) {
		long n = fetch("release/o/ab/x.lz", out, sizeof out, 2048, err, sizeof err);
		if (n != 10000 || !same(out, 10000)) { CHECK(false, "file %d: %ld %s", i, n, err); break; }
	}
	CHECK(connects - c0 == 0, "20 files, no new connection (%d)", connects - c0);
	reset();
	sc.len = 5000;
	sc.keep = 3;
	c0 = connects;
	for (int i = 0; i < 9; i++)
		CHECK(fetch("release/f", out, sizeof out, 2048, err, sizeof err) == 5000,
			"Connection: close every 3rd, file %d (%s)", i, err);
	CHECK(connects - c0 == 3, "a new connection after each close (%d)", connects - c0);
	reset();
	sc.len = 5000;
	sc.http10 = true;
	c0 = connects;
	for (int i = 0; i < 3; i++)
		CHECK(fetch("release/f", out, sizeof out, 2048, err, sizeof err) == 5000,
			"HTTP/1.0, file %d (%s)", i, err);
	CHECK(connects - c0 >= 2, "a connection each (%d)", connects - c0);
	reset();
	sc.len = 5000;
	sc.hangup_idle = true;
	c0 = connects;
	for (int i = 0; i < 4; i++)
		CHECK(fetch("release/f", out, sizeof out, 2048, err, sizeof err) == 5000,
			"a server that hangs up between files, file %d (%s)", i, err);
	CHECK(connects - c0 >= 3, "reconnected (%d)", connects - c0);
	reset();
	sc.len = 70000;
	sc.no_length = true;
	long n = fetch("release/f", out, sizeof out, 2048, err, sizeof err);
	CHECK(n == 70000 && same(out, 70000), "no length: the body ends with the close (%ld %s)",
		n, err);
	reset();
	sc.len = 777;
	n = fetch("release/f", out, sizeof out, 2048, err, sizeof err);
	CHECK(n == 777, "and the next file on a new connection (%ld %s)", n, err);

	reset();
	sc.len = 6000;
	CHECK(fetch("release/f", out, sizeof out, 2048, err, sizeof err) == 6000, "a file (%s)", err);
	sc.ignore_requests = 1;
	c0 = connects;
	n = fetch("release/f", out, sizeof out, 2048, err, sizeof err);
	CHECK(n == 6000 && same(out, 6000) && connects - c0 == 1,
		"a server that closes as the request goes out: asked again on a new "
		"connection (%ld, %d, %s)", n, connects - c0, err);

	printf("answers that are not the file\n");
	reset();
	sc.status = 404;
	n = fetch("release/o/zz/none.lz", out, sizeof out, 2048, err, sizeof err);
	CHECK(n < 0 && strstr(err, "404"), "404: %s", err);
	reset();
	sc.status = 301;
	sc.location = "https://zeitlos.example.org/release/v0.0.7/manifest";
	n = fetch("release/v0.0.7/manifest", out, sizeof out, 2048, err, sizeof err);
	CHECK(n < 0 && strstr(err, "https") && strstr(err, "plain HTTP"),
		"a redirect to https: %s", err);
	reset();
	sc.status = 302;
	sc.location = "http://mirror.example.org/release/v0.0.7/manifest";
	n = fetch("release/v0.0.7/manifest", out, sizeof out, 2048, err, sizeof err);
	CHECK(n < 0 && strstr(err, "not followed") && strstr(err, "mirror.example.org"),
		"a redirect elsewhere, not followed: %s", err);
	reset();
	sc.len = 100;
	n = fetch("release/f", out, sizeof out, 2048, err, sizeof err);
	CHECK(n == 100, "and then a file again (%ld %s)", n, err);

	printf("failures\n");
	reset();
	sc.len = 50000;
	sc.drop_at = 20000;
	n = fetch("release/f", out, sizeof out, 2048, err, sizeof err);
	CHECK(n < 0 && strstr(err, "middle of the file"), "a hang-up mid-file: %s", err);
	reset();
	sc.len = 3000;
	n = fetch("release/f", out, sizeof out, 2048, err, sizeof err);
	CHECK(n == 3000 && same(out, 3000), "the retry, on a new connection (%ld %s)", n, err);

	printf("abandoned half way, then another\n");
	reset();
	sc.len = 200000;
	uint8_t buf[UPD_CHUNK_MAX];
	CHECK(upd_net_open("release/a", err, sizeof err) == UPD_OK, "open a");
	CHECK(upd_net_read(buf, 2048, err, sizeof err) > 0, "a little of a");
	int cc = closes_by_client;
	upd_net_close();
	net_run();
	CHECK(closes_by_client == cc + 1, "closing it half way closes the connection");
	sc.len = 4000;
	n = fetch("release/b", out, sizeof out, 2048, err, sizeof err);
	CHECK(n == 4000 && same(out, 4000), "b arrives whole on a new one (%ld %s)", n, err);

	// last: these leave no connection
	reset();
	upd_net_init_http(PID_NET, 0x5DB8D822, 8080, "zeitlos.example.org", other, 732);
	sc.refuse = true;
	n = fetch("release/f", out, sizeof out, 2048, err, sizeof err);
	CHECK(n < 0 && strstr(err, "refused"), "a connection refused: %s", err);
	reset();
	sc.silent = true;
	n = fetch("release/f", out, sizeof out, 2048, err, sizeof err);
	CHECK(n < 0 && strstr(err, "did not answer"), "a net that never answers: %s", err);

	free(pattern_buf);
	free(outq);
	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
