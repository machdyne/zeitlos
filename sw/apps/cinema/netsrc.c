/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netsrc -- see netsrc.h.
 */

#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zobj.h"
#include "../../common/zport.h"
#include "../../common/znet.h"
#include "../../common/zdns.h"

#include "httpc.h"
#include "netsrc.h"

/* The ring holds what has arrived of the stream and is not yet taken:
 * half a second of a 320x240 film. A request is made only when the
 * ring has room for all of it, so nothing that arrives ever waits for
 * room, and the acks that let `net` send more go back at once. */
#define NS_RING     32768
#define NS_MASK     (NS_RING - 1)
#define NS_REQ      16384
/* a request abandoned by a seek with less than this to come is read to
 * its end and thrown away; with more, the connection is closed instead */
#define NS_DRAIN    6144
#define NS_TIMEOUT  (10 * Z_TICK_HZ)
#define NS_RETRIES  3

enum { S_OFF, S_RESOLVE, S_CONNECT, S_READY, S_BUSY, S_DEAD };
enum { K_NONE, K_STREAM, K_READ };

static bool opened;
static uint8_t state = S_OFF;
static uint32_t net_pid, ip, dns_tag, since;
static uint16_t port;
static z_port_t sock;
static char host[64], hostport[72], path[192];
static const char *err = "";
static char errbuf[64];

static hc_t hc;

/* the request on the wire, or to go as soon as there is a connection */
static uint8_t rq_kind;
static bool rq_sent, rq_discard;
static uint32_t rq_off, rq_len, rq_got;
static uint8_t *rq_dst;
static uint32_t *rq_gotp;
static bool *rq_donep;
static uint8_t rq_retries;

/* a read wanted next (ns_read_later()) */
static bool want_on;
static uint32_t want_off, want_len, *want_gotp;
static uint8_t *want_dst;
static bool *want_donep;

/* the stream */
static uint8_t ring[NS_RING];
static uint32_t ring_r, ring_w;         /* counts; the index is & NS_MASK */
static uint32_t st_pos;                 /* the file offset of the next byte to arrive */
static bool st_on, st_end;
static uint32_t st_limit = 0xffffffffu;  /* the records end here: nothing of the stream is past it */

static ns_stats_t stats;

static void fail(const char *why) {
	uint32_t i = 0;
	while (why[i] && i < sizeof(errbuf) - 1) { errbuf[i] = why[i]; i++; }
	errbuf[i] = 0;
	err = errbuf;
	if (sock.connected) z_port_close(&sock);
	sock.connected = false;
	state = S_DEAD;
	st_end = true;
	if (rq_kind == K_READ && rq_donep) *rq_donep = true;
	rq_kind = K_NONE;
	if (want_on) { want_on = false; if (want_donep) *want_donep = true; }
}

static void hang_up(void) {
	if (sock.connected) z_port_close(&sock);
	sock.connected = false;
	if (state != S_DEAD) state = S_OFF;
}

static void connect_ip(void) {
	z_obj_t arg = z_obj_map(2);
	z_map_set(&arg, "ip", z_obj_uint32(ip));
	z_map_set(&arg, "port", z_obj_uint32(port));
	z_msg_new_send(net_pid, Z_PORT_CONNECT, 0, arg);
	state = S_CONNECT;
	since = z_uptime_ticks();
	stats.connects++;
}

static void dial(void) {
	if (state != S_OFF) return;
	if (ip) { connect_ip(); return; }
	if (z_parse_ipv4(host, &ip)) { connect_ip(); return; }
	dns_tag = (dns_tag + 1) | 0x20000u;
	z_msg_new_send(net_pid, Z_NET_DNS_RESOLVE, dns_tag, z_obj_str(host));
	state = S_RESOLVE;
	since = z_uptime_ticks();
}

static void send_request(void) {
	char req[384];
	uint32_t n = hc_request(&hc, req, sizeof(req), hostport, path, rq_off + rq_got, rq_len - rq_got);
	if (!n || z_port_send(&sock, req, n) != Z_OK) { hang_up(); return; }    /* dialled again by ns_poll() */
	rq_sent = true;
	state = S_BUSY;
	since = z_uptime_ticks();
	stats.requests++;
}

static void start(uint8_t kind, uint32_t off, uint32_t len, uint8_t *dst, uint32_t *gotp, bool *donep) {
	rq_kind = kind;
	rq_off = off;
	rq_len = len;
	rq_got = 0;
	rq_dst = dst;
	rq_gotp = gotp;
	rq_donep = donep;
	rq_sent = false;
	rq_discard = false;
	rq_retries = 0;
	if (gotp) *gotp = 0;
	if (donep) *donep = false;
	if (state == S_READY) send_request();
	else dial();
}

static void body(void *user, const uint8_t *p, uint32_t n) {
	(void)user;
	stats.bytes += n;
	if (rq_kind == K_STREAM) {
		if (!rq_discard) {
			uint32_t at = ring_w & NS_MASK, first = NS_RING - at;
			if (n > NS_RING - (ring_w - ring_r)) n = NS_RING - (ring_w - ring_r);   /* cannot be: asked for with room */
			if (first > n) first = n;
			memcpy(ring + at, p, first);
			memcpy(ring, p + first, n - first);
			ring_w += n;
			st_pos += n;
		}
	} else if (rq_kind == K_READ) {
		if (rq_got + n > rq_len) n = rq_len - rq_got;
		memcpy(rq_dst + rq_got, p, n);
		if (rq_gotp) *rq_gotp = rq_got + n;
	}
	rq_got += n;
	since = z_uptime_ticks();
}

/* the answer is whole, or the connection is gone: what now */
static void settle(void) {
	if (rq_kind == K_NONE) return;
	if (hc.state == HC_DONE) {
		if (rq_kind == K_STREAM && !rq_discard && (rq_got < rq_len || st_pos >= st_limit)) st_end = true;   /* short: the file's end */
		if (rq_kind == K_READ && rq_donep) *rq_donep = true;
		rq_kind = K_NONE;
		if (hc.to_close) hang_up();
		else if (state == S_BUSY) state = S_READY;
		return;
	}
	if (hc.state == HC_FAIL) {
		/* a connection that dropped is tried again from where it got
		 * to; anything the server SAID is final */
		if (hc.err == HC_E_SHORT && rq_retries < NS_RETRIES) {
			rq_retries++;
			stats.retries++;
			hang_up();
			rq_sent = false;
			dial();
			return;
		}
		fail(hc_error(&hc));
	}
}

static void on_data(const z_msg_t *msg) {
	uint32_t n = z_blob_len(&msg->obj);
	const uint8_t *d = z_blob_data(&msg->obj);
	bool ours = sock.connected && msg->tag == sock.conn_id && msg->from == sock.peer_pid;
	if (ours && d && n && rq_kind != K_NONE && rq_sent) {
		hc_feed(&hc, d, n, body, NULL);
		z_port_send_ack(msg);
		if (hc.state == HC_DONE || hc.state == HC_FAIL) settle();
		return;
	}
	z_port_send_ack(msg);
}

static void on_close(void) {
	sock.connected = false;
	if (state == S_DEAD) return;
	state = S_OFF;
	if (rq_kind != K_NONE && rq_sent) {
		hc_closed(&hc);
		settle();
	}
}

bool ns_msg(const z_msg_t *msg) {
	switch (msg->subject) {
	case Z_NET_DNS_RESOLVE_REPLY: {
		z_obj_t *ok, *a;
		if (msg->tag != dns_tag) return (msg->tag & 0x20000u) != 0;
		if (state != S_RESOLVE) return true;
		ok = z_map_find((z_obj_t *)&msg->obj, "ok");
		a = z_map_find((z_obj_t *)&msg->obj, "ip");
		if (ok && ok->type == Z_UINT32 && ok->val.uint32 && a && a->type == Z_UINT32) {
			ip = a->val.uint32;
			connect_ip();
		} else {
			fail("no such host");
		}
		return true;
	}
	case Z_PORT_CONNECTED:
		if (state == S_CONNECT && opened) {
			sock.peer_pid = msg->from;
			sock.conn_id = msg->obj.type == Z_UINT32 ? msg->obj.val.uint32 : 0;
			sock.connected = true;
			state = S_READY;
			if (rq_kind != K_NONE && !rq_sent) send_request();
		} else if (msg->obj.type == Z_UINT32) {
			z_msg_new_send(msg->from, Z_PORT_CLOSE, msg->obj.val.uint32, z_obj_none());     /* one given up on */
		}
		return true;
	case Z_PORT_REFUSED:
		if (state == S_CONNECT) {
			if (msg->obj.type == Z_STR && msg->obj.val.str) fail(msg->obj.val.str);
			else fail("could not connect");
		}
		return true;
	case Z_PORT_DATA:
		on_data(msg);
		return true;
	case Z_PORT_DATA_ACK:
		if (sock.connected && msg->tag == sock.conn_id) z_port_handle_ack(&sock, msg);
		return true;
	case Z_PORT_CLOSE:
		if (sock.connected && msg->tag == sock.conn_id) on_close();
		return true;
	default:
		return false;
	}
}

void ns_poll(void) {
	if (!opened || state == S_DEAD) return;
	if ((state == S_RESOLVE || state == S_CONNECT || state == S_BUSY) &&
	    z_uptime_ticks() - since >= NS_TIMEOUT) {
		if (state == S_BUSY && rq_retries < NS_RETRIES) {
			rq_retries++;
			stats.retries++;
			hang_up();
			rq_sent = false;
		} else {
			fail(state == S_RESOLVE ? "no answer from DNS" : state == S_CONNECT ? "no answer from the server" :
			     "the server stopped answering");
			return;
		}
	}
	if (rq_kind != K_NONE) {
		if (state == S_OFF) dial();     /* closed between requests, or being tried again */
		return;
	}
	if (want_on) {
		want_on = false;
		start(K_READ, want_off, want_len, want_dst, want_gotp, want_donep);
		return;
	}
	if (st_on && !st_end && NS_RING - (ring_w - ring_r) >= NS_REQ) {
		uint32_t n = st_limit > st_pos ? st_limit - st_pos : 0;
		if (n > NS_REQ) n = NS_REQ;
		if (!n) st_end = true;
		else start(K_STREAM, st_pos, n, NULL, NULL, NULL);
	}
}

/* the network's messages, and only those, off the mailbox */
static bool pump(void) {
	z_msg_t m;
	bool any = false, one;
	do {
		one = false;
		if (state == S_RESOLVE && z_msg_take_match(&m, Z_NET_DNS_RESOLVE_REPLY, dns_tag) == Z_OK) { ns_msg(&m); one = true; }
		if (state == S_CONNECT) {
			if (z_msg_take_match(&m, Z_PORT_CONNECTED, 0) == Z_OK) { ns_msg(&m); one = true; }
			else if (z_msg_take_match(&m, Z_PORT_REFUSED, 0) == Z_OK) { ns_msg(&m); one = true; }
		}
		if (sock.connected) {
			uint32_t id = sock.conn_id;
			if (z_msg_take_match(&m, Z_PORT_DATA_ACK, id) == Z_OK) { ns_msg(&m); one = true; }
			if (sock.connected && z_msg_take_match(&m, Z_PORT_DATA, id) == Z_OK) { ns_msg(&m); one = true; }
			else if (sock.connected && z_msg_take_match(&m, Z_PORT_CLOSE, id) == Z_OK) { ns_msg(&m); one = true; }
		}
		any |= one;
	} while (one);
	return any;
}

/* Wait a tick for the network, having done what could be done.
 *
 * Asleep, and for the one message that is due next: not z_proc_wait(),
 * which does not sleep while ANY message is queued -- and the keys
 * pressed during a seek are, for the app's loop to read afterwards. The
 * wait was then a loop of system calls that never gave the processor
 * up, and in the emulator the process serving the connection went
 * unscheduled for as long: ten seconds, until the timeout here. */
static void wait_step(void) {
	uint32_t t = z_uptime_ticks(), subject = 0, tag = 0;
	z_msg_t m;
	ns_poll();
	if (!pump()) {
		if (state == S_RESOLVE) { subject = Z_NET_DNS_RESOLVE_REPLY; tag = dns_tag; }
		else if (state == S_CONNECT) subject = Z_PORT_CONNECTED;
		else if (sock.connected) { subject = Z_PORT_DATA; tag = sock.conn_id; }
		if (!subject) subject = Z_PORT_CONNECTED;       /* (nothing is due: a tick's sleep) */
		if (z_msg_recv_match(&m, subject, tag, 1) == Z_OK) (void)ns_msg(&m);
	}
	stats.wait_ticks += z_uptime_ticks() - t;
}

bool ns_read_later(uint32_t off, void *buf, uint32_t n, uint32_t *got, bool *done) {
	if (!opened || state == S_DEAD || want_on || !n) return false;
	if (rq_kind == K_READ && !rq_discard) return false;
	want_on = true;
	want_off = off;
	want_len = n;
	want_dst = buf;
	want_gotp = got;
	want_donep = done;
	if (got) *got = 0;
	if (done) *done = false;
	return true;
}

bool ns_read_pending(bool wait) {
	while (opened && state != S_DEAD && (want_on || rq_kind == K_READ)) {
		if (!wait) return true;
		wait_step();
	}
	return false;
}

int ns_read_at(uint32_t off, void *buf, uint32_t n) {
	uint32_t got = 0;
	bool done = false;
	if (!opened || state == S_DEAD) return -1;
	(void)ns_read_pending(true);
	if (!ns_read_later(off, buf, n, &got, &done)) return -1;
	while (!done && state != S_DEAD) wait_step();
	/* (the pointers are to this frame: nothing may keep them) */
	if (rq_kind == K_READ && rq_gotp == &got) { rq_gotp = NULL; rq_donep = NULL; }
	if (state == S_DEAD) return -1;
	return (int)got;
}

void ns_stream_seek(uint32_t off, uint32_t limit) {
	ring_r = ring_w = 0;
	st_pos = off;
	st_limit = limit ? limit : 0xffffffffu;
	st_on = true;
	if (state != S_DEAD) st_end = false;
	if (rq_kind == K_STREAM) {
		if (!rq_sent) {
			/* not asked yet -- a connection is being made for it: that
			 * one serves the next request as well. (Hung up here, its
			 * CONNECTED would still come, and be taken for the answer to
			 * the next dial's.) */
			rq_kind = K_NONE;
		} else if (hc.state == HC_BODY && rq_len - rq_got <= NS_DRAIN) {
			rq_discard = true;          /* a little is left: let it come */
		} else {
			hang_up();
			rq_kind = K_NONE;
		}
	}
}

uint32_t ns_stream_read(uint8_t *dst, uint32_t max, bool wait) {
	uint32_t have, at, first;
	if (!opened) return 0;
	while (wait && st_on && ring_w == ring_r && !st_end && state != S_DEAD) wait_step();
	have = ring_w - ring_r;
	if (have > max) have = max;
	at = ring_r & NS_MASK;
	first = NS_RING - at;
	if (first > have) first = have;
	memcpy(dst, ring + at, first);
	memcpy(dst + first, ring, have - first);
	ring_r += have;
	return have;
}

bool ns_stream_ended(void) { return !opened || (st_end && ring_w == ring_r); }
uint32_t ns_stream_buffered(void) { return ring_w - ring_r; }
const char *ns_error(void) { return err; }
bool ns_is_open(void) { return opened; }

void ns_stats(ns_stats_t *out, bool reset) {
	*out = stats;
	if (reset) memset(&stats, 0, sizeof(stats));
}

bool ns_open(const char *url) {
	uint32_t i = 0, n;
	ns_close();
	err = "";
	if (!hc_url(url, host, sizeof(host), &port, path, sizeof(path))) {
		err = "not an http:// address";
		return false;
	}
	if (!z_pid_lookup("net0", &net_pid)) {
		err = "the network is not running";
		return false;
	}
	/* the Host header: with the port when it is not HTTP's own */
	while (host[i]) { hostport[i] = host[i]; i++; }
	if (port != 80) {
		char d[6];
		uint32_t v = port;
		n = 0;
		do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
		hostport[i++] = ':';
		while (n) hostport[i++] = d[--n];
	}
	hostport[i] = 0;
	memset(&hc, 0, sizeof(hc));
	memset(&sock, 0, sizeof(sock));
	ip = 0;
	state = S_OFF;
	rq_kind = K_NONE;
	want_on = false;
	ring_r = ring_w = 0;
	st_on = st_end = false;
	st_pos = 0;
	opened = true;
	return true;
}

void ns_close(void) {
	if (!opened) return;
	if (sock.connected) z_port_close(&sock);
	sock.connected = false;
	state = S_OFF;
	rq_kind = K_NONE;
	want_on = false;
	st_on = false;
	opened = false;
}
