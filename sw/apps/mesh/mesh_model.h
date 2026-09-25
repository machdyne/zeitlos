#ifndef MESH_MODEL_H
#define MESH_MODEL_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * What mesh knows: our node, the channels, the node list and the
 * messages. Fixed-size, allocated once (static), no Zeitlos dependency
 * -- the host tests use it as is. See docs/mesh_app.md, "Model".
 *
 * Filled by mesh_proto.c from what the node sends; read by whatever
 * shows it (the console in phase 3, the window in phase 4).
 */

#include <stdint.h>
#include <stdbool.h>

#include "mesh_pb.h"

#define MESH_MAX_NODES		128
#define MESH_MAX_CHANNELS	8
#define MESH_MAX_MSGS		96

#define MESH_LONG_MAX		40		// bytes incl. NUL
#define MESH_SHORT_MAX		8
#define MESH_CHAN_NAME_MAX	16

#define MESH_UNKNOWN_SNR	(-32768)

typedef struct {
	uint32_t num;
	char long_name[MESH_LONG_MAX];
	char short_name[MESH_SHORT_MAX];
	uint16_t hw_model;
	uint8_t role;
	uint8_t used;
	uint8_t has_user;
	uint8_t has_pos;
	uint8_t favorite;
	int8_t hops_away;		// -1 unknown
	uint8_t battery;		// 0-100, 101 powered, 255 unknown
	int16_t snr_x10;		// MESH_UNKNOWN_SNR if unknown
	uint32_t last_heard;	// the node's epoch seconds, 0 never
	uint32_t seen;			// model's own clock: for eviction
	int32_t lat_i, lon_i;	// degrees * 1e7
} mesh_node_t;

typedef struct {
	uint8_t role;			// CH_ROLE_*
	char name[MESH_CHAN_NAME_MAX];
} mesh_chan_t;

// Message status.
#define MSG_RX			0	// received
#define MSG_SENDING		1	// handed to the node, nothing back yet
#define MSG_SENT		2	// the mesh heard it (implicit ack, or ack
							//   of a broadcast)
#define MSG_DELIVERED	3	// the destination acknowledged it
#define MSG_FAILED		4	// err says why
#define MSG_UNHEARD		5	// a broadcast that went out, but no node was
							//   heard repeating it (the firmware's
							//   MAX_RETRANSMIT for a broadcast): not a
							//   failure, and nobody confirmed it either

typedef struct {
	uint32_t id;			// packet id
	uint32_t from, to;
	uint32_t rx_time;		// node epoch seconds; 0 if not known
	uint32_t seq;			// arrival order, monotonic
	uint8_t channel;
	uint8_t status;			// MSG_*
	uint8_t err;			// RT_ERR_* when MSG_FAILED
	uint8_t dm;				// to a node (not broadcast)
	int16_t snr_x10;
	int16_t rssi;			// 0 unknown
	uint16_t len;
	char text[MESH_PAYLOAD_MAX + 1];
} mesh_msg_t;

typedef struct {
	uint32_t my_num;		// 0 until my_info arrives
	char firmware[24];
	uint16_t hw_model;
	int modem_preset;		// -1 unknown, -2 custom (not a preset)
	int region;				// -1 unknown, 0 unset (will not transmit)
	uint8_t hop_limit;		// 0 unknown
	uint8_t queue_free, queue_max;

	mesh_chan_t chan[MESH_MAX_CHANNELS];

	mesh_node_t node[MESH_MAX_NODES];
	uint32_t clock;			// bumps on every node update

	mesh_msg_t msg[MESH_MAX_MSGS];
	uint32_t msg_head;		// next slot to write
	uint32_t msg_count;		// valid, up to MESH_MAX_MSGS
	uint32_t msg_seq;

	// What went wrong, for the status line.
	uint32_t rejected;		// messages failing the plausibility check
	uint32_t evicted;		// nodes dropped for room
} mesh_model_t;

void mesh_model_init(mesh_model_t *m);

// Everything the node told us, not our own history. Messages are kept.
void mesh_model_forget_node(mesh_model_t *m);

// Only its configuration -- channels, firmware, LoRa settings -- before
// a new config dump. The node list is kept: the dump updates every
// entry in it, and forgetting it would turn every name in the message
// history into a bare !id until the dump arrived (or for good, for a
// node the new dump does not mention).
void mesh_model_forget_config(mesh_model_t *m);

// The node with this number, created if new (evicting the least
// recently seen that is not us and not a favourite). Never NULL.
mesh_node_t *mesh_node_get(mesh_model_t *m, uint32_t num);
// Only if known; NULL otherwise.
mesh_node_t *mesh_node_find(mesh_model_t *m, uint32_t num);
// By short name (case-insensitive), long name, or "!a1b2c3d4".
mesh_node_t *mesh_node_lookup(mesh_model_t *m, const char *s);
int mesh_node_count(const mesh_model_t *m);

// A printable name for a node number: short name if known, else
// "!a1b2c3d4". "^all" for broadcast. Into buf, which it returns.
const char *mesh_name(mesh_model_t *m, uint32_t num, char *buf, int cap);

// "!a1b2c3d4" into buf (at least 10 bytes).
void mesh_node_id(uint32_t num, char *buf);

// A printable channel name: its own, or for an unnamed primary the
// modem preset's ("LongFast").
const char *mesh_chan_name(const mesh_model_t *m, int idx);

// A new message slot, oldest overwritten. Zeroed, seq assigned.
mesh_msg_t *mesh_msg_new(mesh_model_t *m);
// By packet id, most recent first; NULL if not held.
mesh_msg_t *mesh_msg_find(mesh_model_t *m, uint32_t id);
// The i-th oldest held message, i < msg_count.
mesh_msg_t *mesh_msg_at(mesh_model_t *m, uint32_t i);

// Short text for a status / error: "sending", "delivered", "no route"...
const char *mesh_status_name(const mesh_msg_t *g);

#endif
