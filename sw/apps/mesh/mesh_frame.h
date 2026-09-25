#ifndef MESH_FRAME_H
#define MESH_FRAME_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The Meshtastic client API's stream framing, both directions. See
 * docs/mesh_app.md, "The wire: framing".
 *
 * Every protobuf is preceded by four bytes: 0x94 0xc3, then its length,
 * high byte first. A length over 512 is a false start. Anything the
 * receiver sees outside a frame is the node's console text, which this
 * collects into lines.
 *
 * Byte at a time, no allocation, no knowledge of what a frame holds:
 * the caller gets whole frames and whole lines through two callbacks.
 * Pure C with no Zeitlos dependency, so the host tests run it as is.
 *
 * -- what it cannot do --
 *
 * The stream has no CRC. A byte lost INSIDE a frame is not detectable
 * here: the frame simply ends one byte later, having swallowed the
 * first byte of whatever came next. That is why every decoded frame is
 * checked for plausibility above this layer (mesh_proto.c), and why the
 * counters below exist -- a nonzero `resyncs` on a quiet link is the
 * visible symptom of a lossy one.
 */

#include <stdint.h>

#define MESH_START1		0x94
#define MESH_START2		0xc3
#define MESH_FRAME_MAX	512
#define MESH_HDR_LEN	4
#define MESH_LINE_MAX	160		// longer console lines are split

typedef struct {
	// One whole frame's protobuf bytes (not the header).
	void (*on_frame)(void *ctx, const uint8_t *pb, uint32_t len);
	// One line of console text, without its line ending; NUL-terminated.
	void (*on_line)(void *ctx, const char *line);
	void *ctx;

	// -- state; zero is the initial state (mesh_frame_init()) --
	uint8_t state;
	uint16_t want, got;
	uint8_t body[MESH_FRAME_MAX];
	char line[MESH_LINE_MAX + 1];
	uint16_t line_n;

	// -- counters, for the status bar and the tests --
	uint32_t frames;		// delivered
	uint32_t resyncs;		// headers with an impossible length
	uint32_t text_bytes;	// console bytes seen
} mesh_framer_t;

void mesh_frame_init(mesh_framer_t *fr,
	void (*on_frame)(void *, const uint8_t *, uint32_t),
	void (*on_line)(void *, const char *), void *ctx);

void mesh_frame_feed(mesh_framer_t *fr, const uint8_t *p, uint32_t n);

// Forget any partial frame or line -- after a reconnect, whatever was
// half-received belongs to the previous session.
void mesh_frame_reset(mesh_framer_t *fr);

// Header plus protobuf into out. Returns the bytes written, 0 if len is
// over MESH_FRAME_MAX or cap is too small.
uint32_t mesh_frame_wrap(uint8_t *out, uint32_t cap,
	const uint8_t *pb, uint32_t len);

#endif
