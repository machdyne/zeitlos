#ifndef MESH_PROTO_H
#define MESH_PROTO_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * FromRadio in, ToRadio out. Decodes one frame's protobuf into the
 * model and says what changed; encodes the few messages mesh sends,
 * framed and ready to write. No I/O, no Zeitlos dependency.
 *
 * -- plausibility --
 *
 * The serial stream has no CRC, so a frame can arrive shifted by a lost
 * byte and still parse (docs/mesh_app.md, "Risks"). Every message is
 * decoded into temporaries first and applied to the model only if:
 *
 *   - zpb found it well formed, all the way down, and
 *   - every field we know arrived with the wire type it must have.
 *
 * Otherwise nothing is applied, the model's `rejected` count goes up,
 * and the event is MESH_EV_REJECT. Strings are made safe rather than
 * rejected: invalid UTF-8 and control characters become '?' and ' ',
 * because a real sender can produce odd text and losing a message is
 * worse than showing a question mark.
 */

#include <stdint.h>

#include "mesh_model.h"

#define MESH_EV_NONE		0	// nothing mesh shows (log records...)
#define MESH_EV_MY_INFO		1
#define MESH_EV_NODE		2	// node: which
#define MESH_EV_CHANNEL		3
#define MESH_EV_CONFIG		4	// config / metadata / module config
#define MESH_EV_CONFIG_DONE	5	// nonce: which want_config it answers
#define MESH_EV_REBOOTED	6
#define MESH_EV_TEXT		7	// msg: a new message
#define MESH_EV_STATUS		8	// msg: a sent message's status changed
#define MESH_EV_NOTIFY		9	// text: something to tell the user
#define MESH_EV_QUEUE		10
#define MESH_EV_REJECT		11	// failed the plausibility check
#define MESH_EV_PACKET		12	// some other packet (position, telemetry...)

typedef struct {
	int kind;
	uint32_t nonce;
	mesh_node_t *node;
	mesh_msg_t *msg;
	const char *text;
} mesh_ev_t;

// One frame's protobuf (a FromRadio). Fills *ev and returns ev->kind.
int mesh_proto_rx(mesh_model_t *m, const uint8_t *pb, uint32_t len, mesh_ev_t *ev);

// -- ToRadio, each framed (header included) into out; 0 if it does not
// fit. MESH_TX_MAX is always enough. --

#define MESH_TX_MAX			(4 + 300)

uint32_t mesh_tx_want_config(uint8_t *out, uint32_t cap, uint32_t nonce);
uint32_t mesh_tx_heartbeat(uint8_t *out, uint32_t cap, uint32_t nonce);
uint32_t mesh_tx_disconnect(uint8_t *out, uint32_t cap);

// A text message: to a node, or MESH_BROADCAST on `channel`. `id` is the
// packet id we choose (nonzero), which the node's ROUTING reply will
// carry as request_id. len is bytes of UTF-8, at most MESH_PAYLOAD_MAX.
uint32_t mesh_tx_text(uint8_t *out, uint32_t cap, uint32_t to, uint8_t channel,
	uint32_t id, uint8_t hop_limit, const char *text, uint32_t len);

// Record a message we are sending in the model, MSG_SENDING. Call when
// mesh_tx_text()'s bytes have been handed to the transport.
mesh_msg_t *mesh_proto_sent(mesh_model_t *m, uint32_t to, uint8_t channel,
	uint32_t id, const char *text, uint32_t len);

// A float's bits (fixed32 on the wire) as tenths, without floating
// point: RV32IM has no FPU and this is all mesh needs floats for.
int32_t mesh_f32_x10(uint32_t bits);

// Copy a wire string into dst (cap bytes incl. NUL), making it safe to
// draw: invalid UTF-8 -> '?', control characters -> ' ' (newline kept
// if keep_nl). Returns the length written.
uint32_t mesh_str_clean(char *dst, uint32_t cap, const uint8_t *src,
	uint32_t len, int keep_nl);

#endif
