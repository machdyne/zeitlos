/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The Meshtastic stream framing. See mesh_frame.h.
 */

#include <string.h>

#include "mesh_frame.h"

#define ST_HUNT		0	// looking for START1; other bytes are console text
#define ST_START2	1	// had START1
#define ST_LEN_HI	2
#define ST_LEN_LO	3
#define ST_BODY		4

void mesh_frame_init(mesh_framer_t *fr,
	void (*on_frame)(void *, const uint8_t *, uint32_t),
	void (*on_line)(void *, const char *), void *ctx) {
	memset(fr, 0, sizeof(*fr));
	fr->on_frame = on_frame;
	fr->on_line = on_line;
	fr->ctx = ctx;
}

void mesh_frame_reset(mesh_framer_t *fr) {
	fr->state = ST_HUNT;
	fr->want = 0;
	fr->got = 0;
	fr->line_n = 0;
}

static void line_flush(mesh_framer_t *fr) {
	fr->line[fr->line_n] = 0;
	if (fr->on_line) fr->on_line(fr->ctx, fr->line);
	fr->line_n = 0;
}

static void text(mesh_framer_t *fr, uint8_t b) {
	fr->text_bytes++;
	if (b == '\r') return;
	if (b == '\n') {
		line_flush(fr);
		return;
	}
	fr->line[fr->line_n++] = (char)b;
	if (fr->line_n >= MESH_LINE_MAX) line_flush(fr);
}

static void step(mesh_framer_t *fr, uint8_t b) {
	switch (fr->state) {
	case ST_HUNT:
		if (b == MESH_START1) fr->state = ST_START2;
		else text(fr, b);
		break;
	case ST_START2:
		if (b == MESH_START2) {
			fr->state = ST_LEN_HI;
		} else {
			// 0x94 is also a UTF-8 continuation byte ("\xe2\x80\x94"
			// is an em dash), so a lone one is text -- and this byte
			// starts over, since it may itself be a START1.
			fr->state = ST_HUNT;
			text(fr, MESH_START1);
			step(fr, b);
		}
		break;
	case ST_LEN_HI:
		fr->want = (uint16_t)(b << 8);
		fr->state = ST_LEN_LO;
		break;
	case ST_LEN_LO:
		fr->want |= b;
		if (fr->want > MESH_FRAME_MAX) {
			// Not a frame. The two length bytes may themselves begin a
			// real header (a START1 lost from the header before), so
			// they go back through the hunt rather than being dropped.
			uint8_t hi = (uint8_t)(fr->want >> 8);
			fr->resyncs++;
			fr->state = ST_HUNT;
			step(fr, hi);
			step(fr, b);
		} else if (fr->want == 0) {
			// A valid, empty protobuf.
			fr->state = ST_HUNT;
			fr->frames++;
			if (fr->on_frame) fr->on_frame(fr->ctx, fr->body, 0);
		} else {
			fr->got = 0;
			fr->state = ST_BODY;
		}
		break;
	case ST_BODY:
		fr->body[fr->got++] = b;
		if (fr->got == fr->want) {
			fr->state = ST_HUNT;
			fr->frames++;
			if (fr->on_frame) fr->on_frame(fr->ctx, fr->body, fr->want);
		}
		break;
	}
}

void mesh_frame_feed(mesh_framer_t *fr, const uint8_t *p, uint32_t n) {
	uint32_t i;
	for (i = 0; i < n; i++) step(fr, p[i]);
}

uint32_t mesh_frame_wrap(uint8_t *out, uint32_t cap,
	const uint8_t *pb, uint32_t len) {
	if (len > MESH_FRAME_MAX || cap < len + MESH_HDR_LEN) return 0;
	out[0] = MESH_START1;
	out[1] = MESH_START2;
	out[2] = (uint8_t)(len >> 8);
	out[3] = (uint8_t)len;
	if (len) memcpy(out + MESH_HDR_LEN, pb, len);
	return len + MESH_HDR_LEN;
}
