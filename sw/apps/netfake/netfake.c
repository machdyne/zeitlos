/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * netfake -- a stand-in for `net`, for the emulator. See the Makefile.
 *
 *   run netfake [rate=KB] [close] [drop=N] [stall=N] [noranges]
 *
 *   rate=KB    bytes a second it sends, in KB (default 120)
 *   close      "Connection: close" and a close after every answer
 *   drop=N     the connection dropped in the middle of every Nth answer
 *   stall=N    nothing sent for three seconds in every Nth answer
 *   noranges   answers 200 with the whole file, as python's http.server
 *   trace      a line for every connection, request and close
 *
 * One connection at a time. http://anything/NAME is /media/video/NAME.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../../common/zeitlos.h"
#include "../../common/zwm.h"
#include "../../common/zsoc.h"
#include "../../common/zport.h"
#include "../../common/znet.h"
#include "../../common/zfsapp.h"
#include "../../common/zpaths.h"

static char arg[160];
static void take_arg(void) {
	uint32_t wm;
	z_obj_t none;
	z_msg_t m;
	uint32_t i, waited = 0;
	if (!z_pid_lookup("wm0", &wm)) return;
	none.type = Z_NONE;
	z_msg_new_send(wm, Z_WM_GET_ARG, 0, none);
	for (;;) {
		if (z_msg_read(&m) != Z_OK) {
			if (++waited > 2 * Z_TICK_HZ) return;
			z_proc_wait(1);
			continue;
		}
		if (m.subject == Z_WM_ARG && m.tag == 0) break;
	}
	if (m.obj.type != Z_STR || !m.obj.val.str) return;
	for (i = 0; i < sizeof arg - 1 && m.obj.val.str[i]; i++) arg[i] = m.obj.val.str[i];
	arg[i] = 0;
}

static uint32_t opt(const char *name, uint32_t dflt) {
	const char *p = strstr(arg, name);
	if (!p) return dflt;
	p += strlen(name);
	if (*p != '=') return 1;
	return (uint32_t)atoi(p + 1);
}

static z_port_t port;
static uint32_t next_id = 100;
static char req[1024];
static uint32_t reqn;

/* the answer being sent */
static int fh = -1;
static uint32_t left, answers;
static bool closing, dropping;
static uint32_t stall_until;
static char head[256];
static uint32_t headn, heads;

static uint32_t rate_kb, opt_drop, opt_stall;
static bool opt_close, opt_noranges, opt_trace;
#define TR(...) do { if (opt_trace) printf(__VA_ARGS__); } while (0)
static uint32_t budget, last_tick;

static void hang_up(void) {
	if (fh >= 0) { fs_close_handle(fh); fh = -1; }
	if (port.connected) z_port_close(&port);
	memset(&port, 0, sizeof(port));     /* (what was unacked is forgotten: a test aid) */
	left = headn = heads = 0;
	reqn = 0;
}

static void answer(void) {
	char path[128], *p, *e;
	uint32_t a = 0, b = 0xffffffffu, size, n;
	bool ranged = false;
	int sz;
	req[reqn] = 0;
	reqn = 0;
	if (strncmp(req, "GET ", 4)) { TR("netfake: not a request\n"); hang_up(); return; }
	p = req + 4;
	e = strchr(p, ' ');
	if (!e) { hang_up(); return; }
	*e = 0;
	snprintf(path, sizeof(path), "%s%s", Z_DIR_MEDIA_VIDEO, p);
	p = strstr(e + 1, "Range: bytes=");
	if (p && !opt_noranges) {
		p += 13;
		a = (uint32_t)strtoul(p, &p, 10);
		if (*p == '-' && p[1] >= '0' && p[1] <= '9') b = (uint32_t)strtoul(p + 1, NULL, 10);
		ranged = true;
	}
	answers++;
	TR("netfake: t %lu: bytes %lu-%lu on connection %lu\n", (unsigned long)z_uptime_ticks(), (unsigned long)a, (unsigned long)b, (unsigned long)port.conn_id);
	closing = opt_close || opt_noranges;
	sz = fs_size(path);
	if (sz <= 0) {
		headn = (uint32_t)snprintf(head, sizeof(head), "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
		printf("netfake: 404 %s\n", path);
		return;
	}
	size = (uint32_t)sz;
	if (ranged && a >= size) {
		headn = (uint32_t)snprintf(head, sizeof(head),
			"HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%lu\r\nContent-Length: 0\r\n\r\n", (unsigned long)size);
		return;
	}
	if (b >= size) b = size - 1;
	n = ranged ? b - a + 1 : size;
	fh = fs_open_read(path);
	if (fh < 0) { hang_up(); return; }
	if (ranged && a) fs_seek(fh, a);
	left = n;
	if (ranged)
		headn = (uint32_t)snprintf(head, sizeof(head),
			"HTTP/1.1 206 Partial Content\r\nContent-Length: %lu\r\nContent-Range: bytes %lu-%lu/%lu\r\n%s\r\n",
			(unsigned long)n, (unsigned long)a, (unsigned long)b, (unsigned long)size,
			closing ? "Connection: close\r\n" : "");
	else
		headn = (uint32_t)snprintf(head, sizeof(head), "HTTP/1.0 200 OK\r\nContent-Length: %lu\r\n\r\n", (unsigned long)n);
	dropping = opt_drop && answers % opt_drop == 0;
	if (opt_stall && answers % opt_stall == 0) stall_until = z_uptime_ticks() + 3 * Z_TICK_HZ;
}

static void pump(void) {
	static uint8_t buf[4096];
	uint32_t now = z_uptime_ticks();
	if (now != last_tick) {
		budget += (now - last_tick) * rate_kb * 1024u / Z_TICK_HZ;
		if (budget > rate_kb * 1024u / 4) budget = rate_kb * 1024u / 4;
		last_tick = now;
	}
	if (!port.connected) return;
	if (stall_until) {
		if ((int32_t)(now - stall_until) < 0) return;
		stall_until = 0;
	}
	while (port.pending_count < 2) {
		if (heads < headn) {
			if (z_port_send(&port, head + heads, headn - heads) != Z_OK) return;
			heads = headn;
			continue;
		}
		if (left) {
			uint32_t n = left > sizeof(buf) ? sizeof(buf) : left;
			int r;
			if (n > 1460) n = n > budget ? (budget / 1460) * 1460 : n;      /* whole segments, as TCP delivers */
			if (!n || n > budget) return;
			if (dropping && left < 9000) {
				/* (once what was sent has been read: a payload is this
				 * side's memory until its ack) */
				if (port.pending_count) return;
				printf("netfake: dropping the connection\n");
				hang_up();
				return;
			}
			r = fs_read_chunk(fh, buf, (int)n);
			if (r <= 0) { hang_up(); return; }
			if (z_port_send(&port, buf, (uint32_t)r) != Z_OK) return;
			left -= (uint32_t)r;
			budget -= (uint32_t)r;
			continue;
		}
		if (headn) {
			/* the answer is out */
			headn = heads = 0;
			if (fh >= 0) { fs_close_handle(fh); fh = -1; }
			if (closing && port.pending_count == 0) hang_up();
			else if (closing) { headn = heads = 1; return; }    /* until the last is acked */
		}
		return;
	}
}

int main(void) {
	char name[16];
	take_arg();
	rate_kb = opt("rate", 120);
	opt_close = strstr(arg, "close") != NULL;
	opt_noranges = strstr(arg, "noranges") != NULL;
	opt_trace = strstr(arg, "trace") != NULL;
	opt_drop = strstr(arg, "drop") ? opt("drop", 0) : 0;
	opt_stall = strstr(arg, "stall") ? opt("stall", 0) : 0;
	if (!z_pid_register("net", name, sizeof(name)) || strcmp(name, "net0")) {
		printf("netfake: the name net0 is taken (is net running?)\n");
		return 1;
	}
	printf("netfake: net0, %lu KB/s%s%s\n", (unsigned long)rate_kb, opt_close ? ", closing after each answer" : "",
		opt_noranges ? ", no ranges" : "");
	last_tick = z_uptime_ticks();
	for (;;) {
		z_msg_t m;
		while (z_msg_recv(&m, 1) == Z_OK) {
			switch (m.subject) {
			case Z_PORT_CONNECT:
				TR("netfake: t %lu: connection %lu%s\n", (unsigned long)z_uptime_ticks(), (unsigned long)next_id, port.connected ? " (the last one was still open)" : "");
				hang_up();
				z_port_accept(&port, &m, next_id++);
				break;
			case Z_NET_DNS_RESOLVE: {
				z_obj_t r = z_obj_map(2);
				z_map_set(&r, "ok", z_obj_uint32(1));
				z_map_set(&r, "ip", z_obj_uint32(0x0a000002u));
				z_msg_new_send(m.from, Z_NET_DNS_RESOLVE_REPLY, m.tag, r);
				break;
			}
			case Z_PORT_DATA: {
				uint32_t n = z_blob_len(&m.obj);
				const char *d = z_blob_data(&m.obj);
				bool ours = port.connected && m.tag == port.conn_id;
				if (!ours) TR("netfake: t %lu: %lu bytes for connection %lu, which is not open\n", (unsigned long)z_uptime_ticks(), (unsigned long)n, (unsigned long)m.tag);
				if (ours && d && reqn + n < sizeof(req)) { memcpy(req + reqn, d, n); reqn += n; }
				z_port_send_ack(&m);
				if (ours && reqn >= 4 && !memcmp(req + reqn - 4, "\r\n\r\n", 4)) answer();
				break;
			}
			case Z_PORT_DATA_ACK:
				if (port.connected && m.tag == port.conn_id) z_port_handle_ack(&port, &m);
				break;
			case Z_PORT_CLOSE:
				TR("netfake: t %lu: closed by the client: %lu%s, %lu bytes unsent\n", (unsigned long)z_uptime_ticks(), (unsigned long)m.tag,
					port.connected && m.tag == port.conn_id ? "" : " (not the open one)", (unsigned long)left);
				if (port.connected && m.tag == port.conn_id) { port.connected = false; hang_up(); }
				break;
			default:
				break;
			}
			pump();
		}
		pump();
	}
}
