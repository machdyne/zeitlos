/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bbs -- on Zeitlos: the port provider bbs0. docs/bbs.md.
 *
 *   peer --TCP-- net --port-- netserve --port-- bbs0     (a noauth listener)
 *                                   term --port-- bbs0     (`port bbs0`)
 *
 * The core (core/) is the BBS; this is its platform: a zport provider
 * with a connection per node, each one's output sent as the port can
 * take it. Written to the lessons in docs/ports.md: messages are matched
 * to a connection by sender AND tag, a stranger's DATA is rejected, the
 * sends of a closed connection are still counted off as their acks come,
 * and every peer is checked about once a second, because nobody says
 * when a client dies.
 *
 * Who is calling comes from the CONNECT (zport.h, "Who is connecting"):
 * netserve's identity map for a network caller, nothing for a local one.
 * The BBS logs everyone in itself -- `auth` is not consulted.
 *
 * With a federated forum (docs/bbs.md, "Federation") it is also a CLIENT,
 * of this node's fed: a port to bbs_cfg.fed (fed0), connected in the loop
 * -- a blocking z_port_connect() would drop callers' messages while it
 * waited -- retried every 5 seconds while fed is not there, and its
 * messages told from callers' by sender and connection id before
 * anything else looks at them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zpaths.h"
#include "../../common/zeitlos.h"
#include "../../common/zobj.h"
#include "../../common/zport.h"
#include "../../common/zcfg.h"
#include "../../common/zrng.h"
#include "core/bbs.h"

#define CONN_MAX   (BBS_NODES_MAX + 1)    // one more than the nodes, to say "busy"
#define SEND_CHUNK 512      // eight in flight: 4 KB of heap per caller at most
#define DRAIN_TICKS (5u * Z_TICK_HZ)

typedef struct {
	bool used;
	bool closing;            // CLOSE sent: waiting for the last acks
	z_port_t port;
	int node;                // -1: none (refused as busy)
	uint32_t deadline;
	uint32_t next_check;
} conn_t;

static conn_t conns[CONN_MAX];

// -- the link to this node's fed --

#define FED_RETRY_TICKS   (5u * Z_TICK_HZ)
static struct {
	z_port_t port;
	uint32_t pid;
	bool connecting;
	uint32_t since, next_try, next_check;
} fed = { { 0 }, 0, false, 0, 0, 0 };

static bool fed_said;					// "not running" said, until it connects

static void fed_down(bool peer_gone) {
	if (fed.port.connected) {
		if (peer_gone) z_port_forget(&fed.port);
		else z_port_close(&fed.port);
		bbs_fed_up(false);
	}
	memset(&fed.port, 0, sizeof(fed.port));
	fed.connecting = false;
	fed.next_try = z_uptime_ticks() + FED_RETRY_TICKS;
}

static void fed_try(uint32_t now) {
	const char *target = bbs_fed_target();
	if (!target[0] || fed.port.connected || fed.connecting || (int32_t)(now - fed.next_try) < 0) return;
	fed.next_try = now + FED_RETRY_TICKS;
	if (!z_pid_lookup(target, &fed.pid)) {			// not running: next time -- said once
		if (!fed_said) { bbs_logf("fed: %s is not running -- trying every 5 seconds", target); fed_said = true; }
		return;
	}
	fed.connecting = true;
	fed.since = now;
	z_msg_new_send(fed.pid, Z_PORT_CONNECT, 0, z_obj_none());
}

// A message from fed: true if it was one (and has been handled).
static bool fed_msg(const z_msg_t *m) {
	if (!fed.pid || m->from != fed.pid) return false;
	switch (m->subject) {
	case Z_PORT_CONNECTED:
		if (!fed.connecting) return true;
		fed.connecting = false;
		fed.port.peer_pid = m->from;
		fed.port.conn_id = m->obj.type == Z_UINT32 ? m->obj.val.uint32 : 0;
		fed.port.connected = true;
		fed.next_check = z_uptime_ticks() + Z_TICK_HZ;
		fed_said = false;					// said again if it goes away
		bbs_fed_up(true);
		return true;
	case Z_PORT_REFUSED:
		fed.connecting = false;
		return true;
	case Z_PORT_DATA:
		if (!fed.port.connected || m->tag != fed.port.conn_id || m->obj.type != Z_BLOB) return false;
		bbs_fed_input(z_blob_data(&m->obj), z_blob_len(&m->obj));
		z_port_send_ack(m);
		return true;
	case Z_PORT_DATA_ACK:
		if (m->tag != fed.port.conn_id) return false;
		z_port_handle_ack_closed(&fed.port, m);
		return true;
	case Z_PORT_CLOSE:
		if (m->tag != fed.port.conn_id) return false;
		fed.port.connected = false;
		bbs_fed_up(false);
		fed_down(false);
		return true;
	}
	return false;
}

static void fed_flush(uint32_t now) {
	const uint8_t *p;
	uint32_t n;
	if (fed.connecting && (int32_t)(now - fed.since) >= (int32_t)FED_RETRY_TICKS) fed.connecting = false;	// no answer
	if (!fed.port.connected) return;
	if ((int32_t)(now - fed.next_check) >= 0) {
		fed.next_check = now + Z_TICK_HZ;
		if (z_port_peer_gone(&fed.port)) { fed_down(true); return; }
	}
	// silent for a minute: fed may be gone -- restarted on the same pid,
	// which looks alive above -- so the link starts again (fedlink.c)
	if (!bbs_fed_tick(plat_ms())) { fed_down(true); return; }
	while ((n = bbs_fed_output(&p)) > 0) {
		if (n > SEND_CHUNK) n = SEND_CHUNK;
		if (z_port_send(&fed.port, p, n) != Z_OK) break;
		bbs_fed_consumed(n);
	}
}

static conn_t *by_msg(const z_msg_t *m) {
	for (int i = 0; i < CONN_MAX; i++) {
		conn_t *c = &conns[i];
		if (c->used && c->port.peer_pid == m->from && c->port.conn_id == m->tag) return c;
	}
	return NULL;
}

static void conn_end(conn_t *c, bool peer_gone) {
	if (c->node >= 0) bbs_hangup(c->node);
	c->node = -1;
	if (peer_gone) {
		z_port_forget(&c->port);          // it will never ack
		c->used = false;
		return;
	}
	if (c->port.connected) z_port_close(&c->port);
	c->closing = true;
	c->deadline = z_uptime_ticks() + DRAIN_TICKS;
}

static void on_connect(const z_msg_t *m) {
	z_port_ident_t id;
	conn_t *c = NULL;
	for (int i = 0; i < CONN_MAX; i++) if (!conns[i].used) { c = &conns[i]; break; }
	if (!c) { z_port_refuse(m, "bbs: every line is busy"); return; }

	z_port_ident(m, &id);
	char peer[20] = "";
	if (id.remote)
		snprintf(peer, sizeof(peer), "%lu.%lu.%lu.%lu", (unsigned long)(id.peer >> 24),
			(unsigned long)((id.peer >> 16) & 0xFF), (unsigned long)((id.peer >> 8) & 0xFF),
			(unsigned long)(id.peer & 0xFF));
	bbs_conn_t who = { id.remote ? id.transport : "local", peer, id.user };

	memset(c, 0, sizeof(*c));
	c->used = true;
	z_port_accept(&c->port, m, (uint32_t)(c - conns) + 1);
	c->next_check = z_uptime_ticks() + Z_TICK_HZ;
	c->node = bbs_connect(&who);
	if (c->node < 0) {
		const char *t = bbs_busy_text();
		z_port_send(&c->port, t, (uint32_t)strlen(t));
		conn_end(c, false);
	}
}

static void on_msg(const z_msg_t *m) {
	conn_t *c;
	if (fed_msg(m)) return;			// fed's, before anything takes it for a caller's
	switch (m->subject) {
	case Z_PORT_CONNECT:
		on_connect(m);
		return;
	case Z_PORT_DATA:
		c = by_msg(m);
		if (!c || m->obj.type != Z_BLOB) { z_port_reject_stranger(m); return; }
		if (c->node >= 0 && !c->closing)
			bbs_input(c->node, z_blob_data(&m->obj), z_blob_len(&m->obj));
		z_port_send_ack(m);
		return;
	case Z_PORT_DATA_ACK:
		c = by_msg(m);
		if (c) z_port_handle_ack_closed(&c->port, m);
		return;
	case Z_PORT_CLOSE:
		c = by_msg(m);
		if (c && !c->closing) {
			c->port.connected = false;
			conn_end(c, false);
		}
		return;
	}
}

// Output: as much as the port takes; the rest stays in the core's ring
// until the acks come back (Z_PORT_MAX_PENDING_SENDS in flight).
static void flush(conn_t *c) {
	const uint8_t *p;
	uint32_t n;
	while (c->node >= 0 && (n = bbs_output(c->node, &p)) > 0) {
		if (n > SEND_CHUNK) n = SEND_CHUNK;
		if (z_port_send(&c->port, p, n) != Z_OK) break;
		bbs_consumed(c->node, n);
	}
	if (c->node >= 0 && bbs_wants_close(c->node)) conn_end(c, false);
}

// One pass of the loop: messages, the core's timers, output, closing,
// peers that died. True while anything is going on. (A function of its
// own for tests/test_zport.c, which runs it against a scripted kernel.)
static bool step(void) {
	z_msg_t m;
	uint32_t now = z_uptime_ticks();
	bool busy = false;

	while (z_msg_read(&m) == Z_OK) on_msg(&m);
	bbs_poll();
	fed_try(now);
	fed_flush(now);

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
			if (z_port_peer_gone(&c->port)) { conn_end(c, true); continue; }
		}
		flush(c);
	}
	return busy || bbs_busy();
}

int main(void) {
	char name[24], dir[Z_CFG_VAL_MAX];

	if (!z_pid_register("bbs", name, sizeof(name))) {
		printf("bbs: could not register -- already running?\n");
		return 1;
	}
	if (!z_rng_secure())
		printf("bbs: warning -- no seeded random source; password salts are weak (docs/trng.md)\n");
	if (!z_cfg_get("apps.bbs.dir", dir, sizeof(dir)) || !dir[0]) strcpy(dir, Z_DIR_BBS);
	if (!bbs_init(dir)) {
		printf("bbs: cannot use %s -- see docs/bbs.md\n", dir);
		return 1;
	}
	printf("bbs: running as %s\n", name);

	for (;;) z_proc_wait(step() ? 2 : Z_TICK_HZ / 5);
}
