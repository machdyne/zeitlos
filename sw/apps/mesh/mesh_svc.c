/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * mesh0: the radio as a service. mesh_svc.h; docs/mesh_app.md, "mesh0".
 */
#include <string.h>
#include "zeitlos.h"
#include "zobj.h"
#include "zport.h"
#include "zsoc.h"
#include "mesh_pb.h"
#include "mesh_svc.h"

typedef struct {
	bool used;
	z_port_t port;
	uint16_t listen;					// the application port; 0 none yet
	uint32_t next_check;
} svc_client_t;

static svc_client_t cl[MESH_SVC_CLIENTS];
static bool started, was_live;
static uint32_t my_num;
static void (*say)(const char *s);

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// A record to a client; dropped if it is eight behind.
static void to_client(svc_client_t *c, const uint8_t *r, uint32_t n) {
	if (c->port.connected) z_port_send(&c->port, r, n);
}

static void tell_state(svc_client_t *c, bool live) {
	uint8_t r[5];
	if (live) { r[0] = 'U'; put32(r + 1, my_num); to_client(c, r, 5); }
	else { r[0] = 'D'; to_client(c, r, 1); }
}

static void refuse(svc_client_t *c, const char *why) {
	uint8_t r[80];
	uint32_t n = (uint32_t)strlen(why);
	if (n > sizeof(r) - 1) n = sizeof(r) - 1;
	r[0] = 'E';
	memcpy(r + 1, why, n);
	to_client(c, r, n + 1);
}

bool mesh_svc_start(void (*line)(const char *s)) {
	char name[24];
	say = line;
	started = z_pid_register("mesh", name, sizeof(name)) && !strcmp(name, "mesh0");
	if (!started && say) say("mesh: mesh0 is taken -- another mesh is running; no service here");
	return started;
}

static svc_client_t *by_msg(const z_msg_t *m) {
	for (int i = 0; i < MESH_SVC_CLIENTS; i++)
		if (cl[i].used && cl[i].port.peer_pid == m->from && cl[i].port.conn_id == m->tag) return &cl[i];
	return NULL;
}

static void record(svc_client_t *c, const uint8_t *d, uint32_t n, mesh_session_t *s) {
	if (!n) return;
	if (d[0] == 'L' && n == 3) {
		uint32_t p = get16(d + 1);
		if (p < PORT_PRIVATE_MIN) { refuse(c, "listen: ports below 256 are Meshtastic's own"); return; }
		c->listen = (uint16_t)p;
		return;
	}
	if (d[0] == 'S' && n >= 8) {
		uint32_t to = get32(d + 1), port = get16(d + 6);
		uint8_t ch = d[5];
		if (port < PORT_PRIVATE_MIN) { refuse(c, "send: ports below 256 are Meshtastic's own"); return; }
		if (n - 8 > MESH_PAYLOAD_MAX) { refuse(c, "send: more than a packet holds (233 bytes)"); return; }
		if (s->state != MESH_ST_LIVE) { refuse(c, "send: the radio is not up"); return; }
		if (!mesh_session_send_data(s, to, ch, port, d + 8, n - 8)) refuse(c, "send: the node would not take it");
		return;
	}
	refuse(c, "not a record mesh0 knows");
}

bool mesh_svc_msg(const z_msg_t *m, mesh_session_t *s) {
	svc_client_t *c;
	if (!started) return false;
	if (m->subject == Z_PORT_CONNECT) {
		for (int i = 0; i < MESH_SVC_CLIENTS; i++) {
			if (cl[i].used) continue;
			memset(&cl[i], 0, sizeof(cl[i]));
			cl[i].used = true;
			cl[i].next_check = z_uptime_ticks() + Z_TICK_HZ;
			z_port_accept(&cl[i].port, m, (uint32_t)(0x6d000000u + i));	// "m": ids apart from serial's
			tell_state(&cl[i], s->state == MESH_ST_LIVE);
			return true;
		}
		z_port_refuse(m, "mesh0: too many clients");
		return true;
	}
	c = by_msg(m);
	if (!c) return false;						// serial's, or nobody's: not ours
	switch (m->subject) {
	case Z_PORT_DATA:
		if (m->obj.type == Z_BLOB) record(c, z_blob_data(&m->obj), z_blob_len(&m->obj), s);
		z_port_send_ack(m);						// at once: a client never stalls mesh
		return true;
	case Z_PORT_DATA_ACK:
		z_port_handle_ack_closed(&c->port, m);
		return true;
	case Z_PORT_CLOSE:
		z_port_forget(&c->port);
		c->used = false;
		return true;
	}
	return true;
}

void mesh_svc_event(const mesh_ev_t *ev) {
	uint8_t r[16 + MESH_PAYLOAD_MAX];
	if (!started || ev->kind != MESH_EV_DATA || ev->data_len > MESH_PAYLOAD_MAX) return;
	r[0] = 'R';
	put32(r + 1, ev->from);
	put32(r + 5, ev->to);
	r[9] = ev->channel;
	put16(r + 10, ev->port);
	memcpy(r + 12, ev->data, ev->data_len);
	for (int i = 0; i < MESH_SVC_CLIENTS; i++)
		if (cl[i].used && cl[i].listen == ev->port) to_client(&cl[i], r, 12 + ev->data_len);
}

void mesh_svc_tick(mesh_session_t *s) {
	if (!started) return;
	bool live = s->state == MESH_ST_LIVE;
	if (live) my_num = s->m->my_num;
	if (live != was_live) {
		was_live = live;
		for (int i = 0; i < MESH_SVC_CLIENTS; i++) if (cl[i].used) tell_state(&cl[i], live);
	}
	uint32_t now = z_uptime_ticks();
	for (int i = 0; i < MESH_SVC_CLIENTS; i++) {
		if (!cl[i].used || (int32_t)(now - cl[i].next_check) < 0) continue;
		cl[i].next_check = now + Z_TICK_HZ;
		if (z_port_peer_gone(&cl[i].port)) { z_port_forget(&cl[i].port); cl[i].used = false; }
	}
}
