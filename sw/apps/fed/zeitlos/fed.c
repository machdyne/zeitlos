/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * fed on Zeitlos: a zfed node. docs/fed.md; the Linux daemon is
 * linux/main.c, and both are platforms for the same core (core/).
 *
 *   peer --TCP-- net --port-- fed      sessions in (Z_NET_LISTEN) and out
 *                                      (a raw socket through net)
 *          bbs, tools --port-- fed0    the local interface: the same lines
 *                                      as fed.sock on Linux (docs/fed.md)
 *
 * One loop, never blocking: every connect and every DNS lookup is sent
 * and answered in the loop, because zport's and zdns's blocking helpers
 * DISCARD other messages while they wait (docs/netserve.md) -- another
 * session's data among them.
 *
 * Connection ids: net is on both ends here -- the client of our inbound
 * connections, the provider of our outbound one -- so ours for inbound
 * are 0x40000000 + a slot, well away from the small numbers net gives
 * its sockets, and a message is matched by sender AND tag.
 *
 * The node key is the flash key/value store's apps.fed.nodekey, made
 * from the TRNG on first start; without a seeded TRNG, fed does not
 * start (docs/fed.md, "Identity").
 *
 * The radio: with radio: in fed.cfg, fed is a client of mesh0 -- the
 * mesh app, which alone speaks Meshtastic (docs/mesh_app.md, "mesh0") --
 * and its packets feed the radio link (core/fradio.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/zeitlos.h"
#include "../../../common/zobj.h"
#include "../../../common/zport.h"
#include "../../../common/zcfg.h"
#include "../../../common/zrng.h"
#include "../../../common/zkv.h"
#include "../../../common/znet.h"
#include "../../../common/zplat.h"
#include "../core/fnode.h"
#include "../core/fobj.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"

#define NODEKEY_KEY  "apps.fed.nodekey"
#define SEND_CHUNK   512				// eight in flight: 4 KB of heap a connection at most
#define DRAIN_TICKS  (5u * Z_TICK_HZ)
#define DNS_TICKS    (10u * Z_TICK_HZ)
#define ID_IN        0x40000000u			// our ids for connections net hands us
#define ID_CLIENT    0x20000000u			// ... and for local clients

enum { K_IN = 1, K_OUT, K_CLIENT };

typedef struct {
	bool used, closing, connecting;
	int kind;
	z_port_t port;
	fsess_t *s;						// K_IN, K_OUT
	int client;						// K_CLIENT: fnode's client id
	uint32_t deadline, next_check;
} conn_t;

#define CONN_MAX (FNODE_SESSIONS + FNODE_CLIENTS)
static conn_t conns[CONN_MAX];
static uint32_t net_pid, next_net_look;
static bool listen_sent, listening;

// The one outbound attempt at a time: a DNS lookup, then a CONNECT.
static struct {
	int peer;					// -1: none
	bool dns;					// waiting for DNS
	uint32_t dns_tag, since;
	bool connect_sent;			// CONNECT sent, answer not yet in
	conn_t *c;					// the session it is for (NULL: it ended first)
} dial = { -1, false, 0, 0, false, NULL };

static uint32_t peer_ip[FNODE_PEERS];			// resolved, 0: not yet

// -- the radio: a client of mesh0 --

#define MESH_RETRY_TICKS  (5u * Z_TICK_HZ)
static struct {
	z_port_t port;
	uint32_t pid;
	bool connecting;
	uint32_t since, next_try, next_check;
} mesh;

static void le16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

// One packet on the air: an 'S' record, to everyone on the channel. False
// when mesh0 is eight behind -- the link tries again at its next pace.
static bool mesh_send(const uint8_t *p, uint32_t n, void *ctx) {
	uint8_t r[8 + 256];
	(void)ctx;
	if (!mesh.port.connected || n > 256) return false;
	r[0] = 'S';
	le32(r + 1, 0xFFFFFFFFu);
	r[5] = fnode_cfg()->radio_channel;
	le16(r + 6, fnode_cfg()->radio_port);
	memcpy(r + 8, p, n);
	return z_port_send(&mesh.port, r, 8 + n) == Z_OK;
}

static void mesh_down(bool peer_gone) {
	if (mesh.port.connected) {
		if (peer_gone) z_port_forget(&mesh.port); else z_port_close(&mesh.port);
	}
	memset(&mesh.port, 0, sizeof(mesh.port));
	mesh.connecting = false;
	mesh.next_try = z_uptime_ticks() + MESH_RETRY_TICKS;
	fnode_radio_up(false);
}

static void mesh_try(uint32_t now) {
	if (!fnode_cfg()->radio || mesh.port.connected || mesh.connecting || (int32_t)(now - mesh.next_try) < 0) return;
	mesh.next_try = now + MESH_RETRY_TICKS;
	if (!z_pid_lookup("mesh0", &mesh.pid)) return;		// mesh not running: next time
	mesh.connecting = true;
	mesh.since = now;
	z_msg_new_send(mesh.pid, Z_PORT_CONNECT, 0, z_obj_none());
}

static void mesh_record(const uint8_t *d, uint32_t n) {
	if (!n) return;
	if (d[0] == 'U') fnode_radio_up(true);
	else if (d[0] == 'D') fnode_radio_up(false);
	else if (d[0] == 'R' && n >= 12 && d[9] == fnode_cfg()->radio_channel && rd16(d + 10) == fnode_cfg()->radio_port)
		fnode_radio_packet(d + 12, n - 12, plat_ms());
	else if (d[0] == 'E') {
		char line[120];
		snprintf(line, sizeof(line), "fed: radio: mesh0 says: %.*s", (int)(n - 1 < 90 ? n - 1 : 90), (const char *)d + 1);
		plat_log(line);
	}
}

// A message from mesh0: true if it was one (and handled).
static bool mesh_msg(const z_msg_t *m) {
	if (!mesh.pid || m->from != mesh.pid) return false;
	switch (m->subject) {
	case Z_PORT_CONNECTED: {
		if (!mesh.connecting) return true;
		mesh.connecting = false;
		mesh.port.peer_pid = m->from;
		mesh.port.conn_id = m->obj.type == Z_UINT32 ? m->obj.val.uint32 : 0;
		mesh.port.connected = true;
		mesh.next_check = z_uptime_ticks() + Z_TICK_HZ;
		uint8_t r[3] = { 'L', 0, 0 };
		le16(r + 1, fnode_cfg()->radio_port);
		z_port_send(&mesh.port, r, 3);
		return true;
	}
	case Z_PORT_REFUSED:
		mesh.connecting = false;
		return true;
	case Z_PORT_DATA:
		if (!mesh.port.connected || m->tag != mesh.port.conn_id || m->obj.type != Z_BLOB) return false;
		mesh_record(z_blob_data(&m->obj), z_blob_len(&m->obj));
		z_port_send_ack(m);
		return true;
	case Z_PORT_DATA_ACK:
		if (m->tag != mesh.port.conn_id) return false;
		z_port_handle_ack_closed(&mesh.port, m);
		return true;
	case Z_PORT_CLOSE:
		if (m->tag != mesh.port.conn_id) return false;
		mesh.port.connected = false;
		mesh_down(false);
		return true;
	}
	return false;
}

static void mesh_step(uint32_t now) {
	if (mesh.connecting && (int32_t)(now - mesh.since) >= (int32_t)MESH_RETRY_TICKS) mesh.connecting = false;
	mesh_try(now);
	if (mesh.port.connected && (int32_t)(now - mesh.next_check) >= 0) {
		mesh.next_check = now + Z_TICK_HZ;
		if (z_port_peer_gone(&mesh.port)) mesh_down(true);
	}
}
static uint32_t dns_tag_next = 0x7ed00000u;

// -- helpers --

static void ip_str(uint32_t ip, char *out, int cap) {
	snprintf(out, (size_t)cap, "%lu.%lu.%lu.%lu", (unsigned long)(ip >> 24), (unsigned long)((ip >> 16) & 0xFF),
		(unsigned long)((ip >> 8) & 0xFF), (unsigned long)(ip & 0xFF));
}

// A literal dotted quad, or 0.
static uint32_t ip_parse(const char *s) {
	uint32_t ip = 0;
	for (int part = 0; part < 4; part++) {
		if (*s < '0' || *s > '9') return 0;
		uint32_t v = 0;
		int digits = 0;
		while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s++ - '0'); if (++digits > 3) return 0; }
		if (v > 255) return 0;
		ip = (ip << 8) | v;
		if (part < 3) { if (*s != '.') return 0; s++; }
	}
	return *s ? 0 : ip;
}

// The address check's resolver on Zeitlos: literal addresses only. A
// host name would have to be looked up through net, and the loop cannot
// wait for it while the list loads -- so such a member's address is not
// checked (the log says so); its key still is.
static bool resolve_literal(const char *host, char *ip, int cap) {
	uint32_t v = ip_parse(host);
	if (!v) return false;
	ip_str(v, ip, cap);
	return true;
}

static conn_t *by_msg(const z_msg_t *m) {
	for (int i = 0; i < CONN_MAX; i++) {
		conn_t *c = &conns[i];
		if (c->used && c->port.peer_pid == m->from && c->port.conn_id == m->tag) return c;
	}
	return NULL;
}

static conn_t *free_conn(void) {
	for (int i = 0; i < CONN_MAX; i++) if (!conns[i].used) { memset(&conns[i], 0, sizeof(conns[i])); return &conns[i]; }
	return NULL;
}

// Ends a connection. The session (if any) is told and handed back to
// fnode; the port is closed, or forgotten if its peer is gone.
static void conn_end(conn_t *c, bool peer_gone) {
	if (c->s) {
		fsess_closed(c->s);					// a session not finished has failed
		fnode_session_end(c->s, plat_ms());
		c->s = NULL;
	}
	if (c->kind == K_CLIENT && c->client >= 0) { fnode_client_close(c->client); c->client = -1; }
	if (dial.c == c) dial.c = NULL;			// a late CONNECTED is closed when it comes
	c->deadline = z_uptime_ticks() + DRAIN_TICKS;
	if (peer_gone) {						// it will never ack: forget its sends
		z_port_forget(&c->port);
		c->used = false;
	} else if (!c->port.connected) {		// closed by the peer, or never connected
		c->closing = true;					// what we sent may still be acked
	} else {
		z_port_close(&c->port);
		c->closing = true;
	}
}

// -- outbound: DNS, then CONNECT --

static void dial_fail(int peer) {
	// a session that never got a connection: made and ended, so fnode
	// backs off this peer as for any failure
	fsess_t *s = fnode_session(true, peer, "", plat_ms());
	if (s) fnode_session_end(s, plat_ms());
}

static void dial_connect(uint32_t ip) {
	const fcfg_t *cfg = fnode_cfg();
	int p = dial.peer;
	conn_t *c = free_conn();
	fsess_t *s = c ? fnode_session(true, p, cfg->peers[p].host, plat_ms()) : NULL;
	if (!s) { dial.peer = -1; return; }
	c->used = true;
	c->kind = K_OUT;
	c->connecting = true;
	c->s = s;
	c->client = -1;
	c->next_check = z_uptime_ticks() + Z_TICK_HZ;
	dial.c = c;
	dial.connect_sent = true;
	z_obj_t arg = z_obj_map(2);
	z_map_set(&arg, "ip", z_obj_uint32(ip));
	z_map_set(&arg, "port", z_obj_uint32(cfg->peers[p].port));
	z_msg_new_send(net_pid, Z_PORT_CONNECT, 0, arg);
	dial.peer = -1;
}

static void dial_start(int p) {
	const fcfg_t *cfg = fnode_cfg();
	uint32_t lit = ip_parse(cfg->peers[p].host);
	dial.peer = p;
	if (lit) peer_ip[p] = lit;
	if (peer_ip[p]) { dial_connect(peer_ip[p]); return; }
	dial.dns = true;
	dial.dns_tag = dns_tag_next++;
	dial.since = z_uptime_ticks();
	z_msg_new_send(net_pid, Z_NET_DNS_RESOLVE, dial.dns_tag, z_obj_str((char *)cfg->peers[p].host));
}

// -- messages --

static void on_net_connect(const z_msg_t *m) {
	z_net_accept_t info;
	char addr[20];
	if (m->obj.type != Z_BLOB || z_blob_len(&m->obj) < sizeof(info)) {
		z_msg_new_send(m->from, Z_PORT_REFUSED, m->tag, z_obj_str("fed: malformed"));
		return;
	}
	memcpy(&info, z_blob_data(&m->obj), sizeof(info));
	ip_str(info.ip, addr, sizeof(addr));
	conn_t *c = free_conn();
	// busy, or waiting out a limit: refused before a byte is read
	fsess_t *s = (c && fnode_admit(addr, plat_ms())) ? fnode_session(false, -1, addr, plat_ms()) : NULL;
	if (!s) { z_msg_new_send(m->from, Z_PORT_REFUSED, m->tag, z_obj_str("fed: busy")); return; }
	c->used = true;
	c->kind = K_IN;
	c->s = s;
	c->client = -1;
	c->port.peer_pid = m->from;
	c->port.conn_id = ID_IN | (uint32_t)(c - conns);
	c->port.connected = true;
	c->next_check = z_uptime_ticks() + Z_TICK_HZ;
	z_msg_new_send(m->from, Z_PORT_CONNECTED, m->tag, z_obj_uint32(c->port.conn_id));
}

static void on_client_connect(const z_msg_t *m) {
	conn_t *c = free_conn();
	int id = c ? fnode_client_open() : -1;
	if (id < 0) { z_port_refuse(m, "fed: too many clients"); return; }
	c->used = true;
	c->kind = K_CLIENT;
	c->client = id;
	c->next_check = z_uptime_ticks() + Z_TICK_HZ;
	z_port_accept(&c->port, m, ID_CLIENT | (uint32_t)(c - conns));
}

static void on_msg(const z_msg_t *m) {
	conn_t *c;
	if (mesh_msg(m)) return;			// mesh0's, before a session's or a client's
	if (net_pid && m->from == net_pid) {
		switch (m->subject) {
		case Z_NET_LISTEN_REPLY:
			listening = m->obj.type == Z_UINT32 && m->obj.val.uint32 == 0;
			printf(listening ? "fed: listening on port %lu\n" : "fed: net refused port %lu\n", (unsigned long)m->tag);
			return;
		case Z_NET_DNS_RESOLVE_REPLY:
			if (dial.dns && m->tag == dial.dns_tag) {
				z_obj_t *ok = z_map_find((z_obj_t *)&m->obj, "ok"), *ip = z_map_find((z_obj_t *)&m->obj, "ip");
				int p = dial.peer;
				dial.dns = false;
				if (ok && ok->type == Z_UINT32 && ok->val.uint32 && ip && ip->type == Z_UINT32) {
					peer_ip[p] = ip->val.uint32;
					dial_connect(peer_ip[p]);
				} else {
					printf("fed: cannot resolve %s\n", fnode_cfg()->peers[p].host);
					dial.peer = -1;
					dial_fail(p);
				}
			}
			return;
		case Z_PORT_CONNECTED:
			// the answer to our CONNECT (net answers in order; we have one at a time)
			if (!dial.connect_sent) return;
			dial.connect_sent = false;
			if (!dial.c) {					// its session ended first: close what we were given
				if (m->obj.type == Z_UINT32) z_msg_new_send(m->from, Z_PORT_CLOSE, m->obj.val.uint32, z_obj_none());
				return;
			}
			c = dial.c;
			dial.c = NULL;
			c->port.peer_pid = m->from;
			c->port.conn_id = m->obj.type == Z_UINT32 ? m->obj.val.uint32 : 0;
			c->port.connected = true;
			c->connecting = false;
			return;
		case Z_PORT_REFUSED:
			if (!dial.connect_sent) return;
			dial.connect_sent = false;
			if (dial.c) { c = dial.c; dial.c = NULL; c->connecting = false; conn_end(c, true); }
			return;
		case Z_PORT_CONNECT:
			on_net_connect(m);
			return;
		case Z_NET_EOF:
			for (int i = 0; i < CONN_MAX; i++)
				if (conns[i].used && conns[i].kind == K_IN && conns[i].port.conn_id == m->tag && conns[i].s)
					fsess_closed(conns[i].s);		// its side is finished: a session not done has failed
			return;
		}
	}
	switch (m->subject) {
	case Z_PORT_CONNECT:
		on_client_connect(m);
		return;
	case Z_PORT_DATA:
		c = by_msg(m);
		if (!c || m->obj.type != Z_BLOB) { z_port_reject_stranger(m); return; }
		if (!c->closing) {
			if (c->s) fsess_input(c->s, z_blob_data(&m->obj), z_blob_len(&m->obj));
			else if (c->kind == K_CLIENT && c->client >= 0) fnode_client_input(c->client, z_blob_data(&m->obj), z_blob_len(&m->obj));
		}
		z_port_send_ack(m);
		return;
	case Z_PORT_DATA_ACK:
		c = by_msg(m);
		if (c) z_port_handle_ack_closed(&c->port, m);
		return;
	case Z_PORT_CLOSE:
		c = by_msg(m);
		if (c && !c->closing) { c->port.connected = false; conn_end(c, false); }
		return;
	}
}

// Output: as much as the port takes, SEND_CHUNK at a time.
static void flush(conn_t *c) {
	const uint8_t *p;
	uint32_t n;
	if (!c->port.connected || c->connecting) return;
	for (;;) {
		n = c->s ? fsess_output(c->s, &p) : (c->client >= 0 ? fnode_client_output(c->client, &p) : 0);
		if (!n) break;
		if (n > SEND_CHUNK) n = SEND_CHUNK;
		if (z_port_send(&c->port, p, n) != Z_OK) break;
		if (c->s) fsess_consumed(c->s, n); else fnode_client_consumed(c->client, n);
	}
}

// One pass of the loop. True while anything is going on. (A function of
// its own for tests/test_zfed.c, which runs it against a scripted kernel
// and a real Linux fed.)
static bool step(void) {
	z_msg_t m;
	bool busy = false;

	while (z_msg_read(&m) == Z_OK) on_msg(&m);
	// the clock AFTER the messages: a session they made began just now
	uint32_t now = z_uptime_ticks(), ms = plat_ms();

	if (!net_pid && (int32_t)(now - next_net_look) >= 0) {
		next_net_look = now + Z_TICK_HZ;
		if (z_pid_lookup("net0", &net_pid)) listen_sent = false;
		else net_pid = 0;
	}
	if (net_pid && !listen_sent && fnode_cfg()->listen) {
		listen_sent = true;
		z_msg_new_send(net_pid, Z_NET_LISTEN, fnode_cfg()->listen, z_obj_uint32(fnode_cfg()->listen));
	}
	if (net_pid && dial.peer < 0 && !dial.dns && !dial.connect_sent) {
		int p = fnode_due(ms);
		if (p >= 0) dial_start(p);
	}
	if (dial.dns && (int32_t)(now - dial.since) >= (int32_t)DNS_TICKS) {
		int p = dial.peer;
		dial.dns = false;
		dial.peer = -1;
		printf("fed: no answer resolving %s\n", fnode_cfg()->peers[p].host);
		dial_fail(p);
	}

	for (int i = 0; i < CONN_MAX; i++) {
		conn_t *c = &conns[i];
		if (!c->used) continue;
		busy = true;
		if (c->closing) {
			if (!c->port.pending_count || (int32_t)(now - c->deadline) >= 0) c->used = false;
			continue;
		}
		if ((int32_t)(now - c->next_check) >= 0) {
			c->next_check = now + Z_TICK_HZ;
			if (c->port.connected && z_port_peer_gone(&c->port)) { conn_end(c, true); continue; }
		}
		flush(c);
		if (c->s) {
			fsess_poll(c->s, ms);
			// a failed session ends now; a finished one once its last bytes are out
			const uint8_t *p;
			if (c->s->state == FS_FAILED || (c->s->state == FS_DONE && !fsess_output(c->s, &p))) conn_end(c, false);
		}
	}
	mesh_step(now);
	fnode_tick(ms);
	return busy;
}

// The node key: apps.fed.nodekey, made from the TRNG on first start.
static bool node_seed(uint8_t seed[32]) {
	uint32_t len = 0;
	if (z_kv_get(NODEKEY_KEY, seed, 32, &len) == Z_KV_OK && len == 32) return true;
	if (!z_rng_secure()) {
		printf("fed: no seeded random source -- a node key cannot be made (docs/trng.md)\n");
		return false;
	}
	z_rng_bytes(seed, 32);
	int rc = z_kv_set(NODEKEY_KEY, seed, 32);
	if (rc != Z_KV_OK) {
		printf("fed: the node key cannot be stored (kv: %d) -- a new identity every start is no identity\n", rc);
		return false;
	}
	printf("fed: a node key made\n");
	return true;
}

// Starts the node: the key, the core, the name. False, having said why.
static bool fed_start(const char *dir) {
	uint8_t seed[32];
	char err[160], h[65], name[24];
	if (!node_seed(seed)) return false;
	fnode_set_resolver(resolve_literal);
	fnode_radio_attach(mesh_send, NULL);		// a radio: in fed.cfg uses it
	if (fnode_start(dir, seed, err, sizeof(err))) {
		memset(seed, 0, sizeof(seed));
		printf("fed: %s\n", err);
		return false;
	}
	memset(seed, 0, sizeof(seed));
	if (!z_pid_register("fed", name, sizeof(name))) {
		printf("fed: could not register -- already running?\n");
		return false;
	}
	for (int i = 0; i < FNODE_PEERS; i++) peer_ip[i] = 0;
	fobj_hex(fnode_public_key(), 32, h);
	printf("fed: running as %s, key %s\n", name, h);
	return true;
}

int main(void) {
	char dir[Z_CFG_VAL_MAX];
	if (!z_cfg_get("apps.fed.dir", dir, sizeof(dir)) || !dir[0]) strcpy(dir, "/fed");
	if (!fed_start(dir)) return 1;
	for (;;) z_proc_wait(step() ? 2 : Z_TICK_HZ / 5);
}
