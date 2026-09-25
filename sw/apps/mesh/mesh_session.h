#ifndef MESH_SESSION_H
#define MESH_SESSION_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * One client-API session with a Meshtastic node: the handshake, the
 * config dump, the live phase, heartbeats and recovery. See
 * docs/mesh_app.md, "Session".
 *
 * Transport-free and clock-free: the caller hands in bytes as they
 * arrive, says when the link comes and goes, and ticks it with the time
 * in milliseconds. Bytes to send go out through a callback. So the host
 * tests drive it with a simulated node, and the app drives it with the
 * `serial0` port -- and a TCP transport later would be one more caller.
 */

#include <stdint.h>

#include "mesh_frame.h"
#include "mesh_model.h"
#include "mesh_proto.h"

#define MESH_ST_DOWN	0	// no link
#define MESH_ST_CONFIG	1	// asked for the config, waiting for the end
#define MESH_ST_LIVE	2	// config complete; packets as they come

// Without a config_complete in this long, ask again with a new nonce.
#define MESH_CONFIG_TIMEOUT_MS	15000u
// A heartbeat this often while live, so the node keeps treating us as
// its client.
#define MESH_HEARTBEAT_MS		60000u

typedef struct mesh_session mesh_session_t;

struct mesh_session {
	// -- set by the caller before mesh_session_init() returns --
	// Write bytes to the node. 0 if taken, nonzero if not.
	int (*send)(void *ctx, const uint8_t *p, uint32_t n);
	// Something happened (mesh_proto.h's MESH_EV_*). May be NULL.
	void (*event)(void *ctx, const mesh_ev_t *ev);
	// A line of the node's console text. May be NULL.
	void (*line)(void *ctx, const char *s);
	// 32 random bits: nonces and packet ids.
	uint32_t (*rand32)(void *ctx);
	void *ctx;

	mesh_model_t *m;
	mesh_framer_t fr;
	int state;
	uint32_t nonce;			// of the want_config in flight
	uint32_t t_asked;		// when it was sent
	uint32_t t_beat;		// last heartbeat
	uint32_t now;			// last time we were told
	uint32_t asks;			// want_configs sent this link
	uint32_t configs;		// dumps completed, all time
	uint32_t send_fail;		// transport refused bytes
};

void mesh_session_init(mesh_session_t *s, mesh_model_t *m);

// The link is up: wake the node's API and ask for its configuration.
void mesh_session_up(mesh_session_t *s, uint32_t now_ms);
// The link is gone. Partial frames are dropped; the model keeps what it
// knows until the next config replaces it.
void mesh_session_down(mesh_session_t *s);

void mesh_session_rx(mesh_session_t *s, const uint8_t *p, uint32_t n);
void mesh_session_tick(mesh_session_t *s, uint32_t now_ms);

// Send a text message: to a node number, or MESH_BROADCAST on channel.
// The message is recorded in the model as MSG_SENDING and returned;
// NULL if it cannot go (not live, too long, empty, transport refused).
mesh_msg_t *mesh_session_send_text(mesh_session_t *s, uint32_t to,
	uint8_t channel, const char *text, uint32_t len);

// Tell the node we are going. Best effort.
void mesh_session_bye(mesh_session_t *s);

#endif
