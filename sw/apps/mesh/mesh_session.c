/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * One client-API session. See mesh_session.h.
 */

#include <string.h>

#include "mesh_session.h"

static void emit(mesh_session_t *s, const mesh_ev_t *ev) {
	if (s->event) s->event(s->ctx, ev);
}

static int put(mesh_session_t *s, const uint8_t *p, uint32_t n) {
	if (!n || !s->send || s->send(s->ctx, p, n)) {
		s->send_fail++;
		return -1;
	}
	return 0;
}

static void on_frame(void *ctx, const uint8_t *pb, uint32_t len) {
	mesh_session_t *s = (mesh_session_t *)ctx;
	mesh_ev_t ev;

	if (s->state == MESH_ST_DOWN) return;
	mesh_proto_rx(s->m, pb, len, &ev);

	if (ev.kind == MESH_EV_CONFIG_DONE) {
		// An old nonce is a previous request's answer: not the end of
		// this one.
		if (s->state != MESH_ST_CONFIG || ev.nonce != s->nonce) return;
		s->state = MESH_ST_LIVE;
		s->configs++;
		s->t_beat = s->now;
	} else if (ev.kind == MESH_EV_REBOOTED) {
		// Its channels and config may be different now.
		emit(s, &ev);
		mesh_session_up(s, s->now);
		return;
	}
	emit(s, &ev);
}

static void on_line(void *ctx, const char *line) {
	mesh_session_t *s = (mesh_session_t *)ctx;
	if (s->line) s->line(s->ctx, line);
}

void mesh_session_init(mesh_session_t *s, mesh_model_t *m) {
	s->m = m;
	s->state = MESH_ST_DOWN;
	s->nonce = 0;
	s->asks = 0;
	s->configs = 0;
	s->send_fail = 0;
	mesh_frame_init(&s->fr, on_frame, on_line, s);
}

static void ask(mesh_session_t *s) {
	uint8_t out[MESH_TX_MAX];
	uint8_t wake[32];

	// A run of START2 first: if the node's receiver is part-way through
	// hunting for a header -- a previous client left mid-frame, or this
	// is our own retry -- this gives it bytes to resynchronise on
	// before the request that matters.
	memset(wake, MESH_START2, sizeof(wake));
	put(s, wake, sizeof(wake));

	// Never 0, and never the previous one: an answer to that must not
	// end this request.
	do {
		s->nonce = s->rand32 ? s->rand32(s->ctx) : s->nonce + 1;
		s->nonce &= 0x7fffffffu;
	} while (s->nonce == 0);
	put(s, out, mesh_tx_want_config(out, sizeof(out), s->nonce));
	s->t_asked = s->now;
	s->asks++;
}

void mesh_session_up(mesh_session_t *s, uint32_t now_ms) {
	s->now = now_ms;
	mesh_frame_reset(&s->fr);
	mesh_model_forget_config(s->m);
	s->state = MESH_ST_CONFIG;
	ask(s);
}

void mesh_session_down(mesh_session_t *s) {
	s->state = MESH_ST_DOWN;
	mesh_frame_reset(&s->fr);
}

void mesh_session_rx(mesh_session_t *s, const uint8_t *p, uint32_t n) {
	if (s->state == MESH_ST_DOWN) return;
	mesh_frame_feed(&s->fr, p, n);
}

void mesh_session_tick(mesh_session_t *s, uint32_t now_ms) {
	s->now = now_ms;
	if (s->state == MESH_ST_CONFIG &&
		now_ms - s->t_asked >= MESH_CONFIG_TIMEOUT_MS) {
		ask(s);
	} else if (s->state == MESH_ST_LIVE &&
		now_ms - s->t_beat >= MESH_HEARTBEAT_MS) {
		uint8_t out[MESH_TX_MAX];
		put(s, out, mesh_tx_heartbeat(out, sizeof(out),
			s->rand32 ? s->rand32(s->ctx) : 0));
		s->t_beat = now_ms;
	}
}

mesh_msg_t *mesh_session_send_text(mesh_session_t *s, uint32_t to,
	uint8_t channel, const char *text, uint32_t len) {
	uint8_t out[MESH_TX_MAX];
	uint32_t id, n;

	if (s->state != MESH_ST_LIVE || !len || len > MESH_PAYLOAD_MAX ||
		channel >= MESH_MAX_CHANNELS)
		return NULL;
	do {
		id = s->rand32 ? s->rand32(s->ctx) : 0x10000u + s->m->msg_seq;
	} while (id == 0);
	n = mesh_tx_text(out, sizeof(out), to, channel, id,
		s->m->hop_limit ? s->m->hop_limit : 3, text, len);
	if (put(s, out, n)) return NULL;
	return mesh_proto_sent(s->m, to, channel, id, text, len);
}

void mesh_session_bye(mesh_session_t *s) {
	uint8_t out[16];
	if (s->state == MESH_ST_DOWN) return;
	put(s, out, mesh_tx_disconnect(out, sizeof(out)));
	mesh_session_down(s);
}
