/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The radio link. fradio.h; docs/fed.md, "The radio link".
 */
#include <string.h>
#include "fradio.h"
#include "fstore.h"
#include "fsess.h"				// fsess_wanted()

static fradio_cfg_t g;
static bool g_up;
static uint8_t g_slot;			// the store's peer slot for "the radio"
static uint32_t g_ann_pos;		// announced up to here
static uint32_t g_t_ann, g_t_sent, g_now;
static fradio_stats_t g_st;

// A key for "the radio" as a peer of the store: objects that came from it
// are marked with its slot, and never announced back to it.
static const uint8_t RADIO_PEER[32] = "zfed radio link, not a node key";

static struct { uint16_t n; uint8_t p[FRADIO_PKT]; } g_q[FRADIO_QUEUE];
static int g_qh, g_qn;

typedef struct {
	bool used;
	uint8_t id[FRADIO_ID];
	uint32_t total;					// 0 until the first fragment says
	uint8_t count, have;
	uint8_t got[32];				// which fragments are here
	uint32_t last_ms;
	int tries;
	uint8_t buf[FRADIO_MAX];
} asm_t;
static asm_t g_asm[FRADIO_ASM];
static uint8_t g_obj[FRADIO_MAX];		// an object read to announce or serve -- none larger goes

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

// -- the queue to the air --

static bool queued(const uint8_t *p, uint32_t n) {
	for (int i = 0; i < g_qn; i++) {
		int k = (g_qh + i) % FRADIO_QUEUE;
		if (g_q[k].n == n && !memcmp(g_q[k].p, p, n)) return true;
	}
	return false;
}

static void queue(const uint8_t *p, uint32_t n) {
	if (n > FRADIO_PKT || g_qn >= FRADIO_QUEUE || queued(p, n)) return;
	int k = (g_qh + g_qn) % FRADIO_QUEUE;
	g_q[k].n = (uint16_t)n;
	memcpy(g_q[k].p, p, n);
	g_qn++;
}

// -- what is here --

// An object here by the store's 16-byte prefix of its id: its length, or 0.
static int held(const uint8_t id16[FRADIO_ID], uint8_t *buf) {
	uint8_t id[32];
	memset(id, 0, sizeof(id));
	memcpy(id, id16, FRADIO_ID);
	return fstore_get_id(id, buf, FRADIO_MAX);		// none if cancelled, or too big for the radio
}

static bool have(const uint8_t id16[FRADIO_ID]) {
	uint8_t id[32];
	memset(id, 0, sizeof(id));
	memcpy(id, id16, FRADIO_ID);
	return fstore_have(id);
}

// May it go over the radio? Small enough; not the radio's own.
static bool eligible(uint32_t pos, uint8_t id16[FRADIO_ID]) {
	int n = fstore_get(pos, g_obj, sizeof(g_obj));
	fobj_t o;
	if (n <= 0 || (uint32_t)n > g.max_object || fobj_parse(g_obj, (uint32_t)n, 0, &o, NULL)) return false;
	memcpy(id16, o.id, FRADIO_ID);
	return true;
}

// -- announcing --

static void announce(const uint8_t ids[][FRADIO_ID], int n) {
	uint8_t p[FRADIO_PKT];
	if (!n) return;
	p[0] = 'I';
	p[1] = (uint8_t)n;
	for (int i = 0; i < n; i++) memcpy(p + 2 + i * FRADIO_ID, ids[i], FRADIO_ID);
	queue(p, 2 + (uint32_t)n * FRADIO_ID);
}

// What arrived here since last time -- not from the radio.
static void announce_new(void) {
	uint8_t ids[FRADIO_INV_MAX][FRADIO_ID];
	int n = 0;
	uint32_t p;
	while (n < FRADIO_INV_MAX && (p = fstore_next_for(g_ann_pos, g_slot))) {
		g_ann_pos = p;
		if (eligible(p, ids[n])) n++;
	}
	announce(ids, n);
}

// The newest, again: for nodes that were out of range when they went.
static void announce_newest(void) {
	uint8_t ids[FRADIO_INV_MAX][FRADIO_ID], one[FRADIO_ID];
	int n = 0;
	uint32_t last = fstore_last(), from = last > 64 ? last - 64 : 0, p = from;
	while ((p = fstore_next_for(p, g_slot)) && p <= last) {
		if (!eligible(p, one)) continue;
		if (n == FRADIO_INV_MAX) { memmove(ids[0], ids[1], (FRADIO_INV_MAX - 1) * FRADIO_ID); n--; }
		memcpy(ids[n++], one, FRADIO_ID);
	}
	announce(ids, n);
}

// -- asking --

static void want(asm_t *a) {
	uint8_t p[1 + FRADIO_ID + 32];
	uint32_t n = 1 + FRADIO_ID;
	p[0] = 'W';
	memcpy(p + 1, a->id, FRADIO_ID);
	if (a->total) {							// only what is missing
		uint32_t bytes = (a->count + 7u) / 8u;
		for (uint32_t i = 0; i < bytes; i++) p[n + i] = (uint8_t)~a->got[i];
		if (a->count % 8) p[n + bytes - 1] &= (uint8_t)((1u << (a->count % 8)) - 1u);
		n += bytes;
	}										// else: none yet -- all of it
	queue(p, n);
}

static asm_t *asm_of(const uint8_t id[FRADIO_ID]) {
	for (int i = 0; i < FRADIO_ASM; i++) if (g_asm[i].used && !memcmp(g_asm[i].id, id, FRADIO_ID)) return &g_asm[i];
	return NULL;
}

static asm_t *asm_new(const uint8_t id[FRADIO_ID]) {
	for (int i = 0; i < FRADIO_ASM; i++) {
		if (g_asm[i].used) continue;
		asm_t *a = &g_asm[i];
		a->used = true;
		memcpy(a->id, id, FRADIO_ID);
		a->total = 0; a->count = a->have = 0;
		memset(a->got, 0, sizeof(a->got));
		a->last_ms = g_now;
		a->tries = 0;
		return a;
	}
	return NULL;
}

// -- serving --

static void serve(const uint8_t id[FRADIO_ID], const uint8_t *bitmap, uint32_t blen) {
	int n = held(id, g_obj);
	if (n <= 0 || (uint32_t)n > g.max_object) return;
	uint32_t count = ((uint32_t)n + FRADIO_FRAG_DATA - 1) / FRADIO_FRAG_DATA;
	for (uint32_t i = 0; i < count; i++) {
		if (blen && (i / 8 >= blen || !(bitmap[i / 8] & (1u << (i % 8))))) continue;
		uint8_t p[FRADIO_PKT];
		uint32_t off = i * FRADIO_FRAG_DATA, len = (uint32_t)n - off;
		if (len > FRADIO_FRAG_DATA) len = FRADIO_FRAG_DATA;
		p[0] = 'F';
		memcpy(p + 1, id, FRADIO_ID);
		put16(p + 1 + FRADIO_ID, (uint32_t)n);
		p[3 + FRADIO_ID] = (uint8_t)i;
		p[4 + FRADIO_ID] = (uint8_t)count;
		memcpy(p + 5 + FRADIO_ID, g_obj + off, len);
		queue(p, 5 + FRADIO_ID + len);
	}
}

// -- putting back together --

static void complete(asm_t *a) {
	fobj_t o;
	uint32_t pos;
	a->used = false;
	uint32_t now_s = g.clock ? g.clock(g.ctx) : 0;
	if (fobj_parse(a->buf, a->total, now_s, &o, NULL) || o.size != a->total || memcmp(o.id, a->id, FRADIO_ID)) {
		// not the object announced: a fragment damaged on the way. Again.
		g_st.objects_rejected++;
		if (a->tries < g.retries) {
			asm_t *b = asm_new(a->id);
			if (b) { b->tries = a->tries + 1; want(b); }
		}
		return;
	}
	if ((g.wants && !fsess_wanted(g.wants, o.topic)) || (g.origin_ok && !g.origin_ok(o.origin, o.topic, g.ctx)) ||
			(g.admit && !g.admit(&o, g.ctx)) || fobj_verify(&o)) {
		g_st.objects_rejected++;
		return;
	}
	int r = fstore_put_from(a->buf, a->total, now_s, g_slot, &pos);
	if (r == FSTORE_NEW) {
		g_st.objects_in++;
		fstore_sync();
		if (g.stored) g.stored(&o, g.ctx);
	}
}

static void got_frag(const uint8_t *p, uint32_t n) {
	const uint8_t *id = p + 1;
	uint32_t total = get16(p + 1 + FRADIO_ID), idx = p[3 + FRADIO_ID], count = p[4 + FRADIO_ID];
	uint32_t len = n - (5 + FRADIO_ID);
	if (!count || idx >= count || total > sizeof(g_asm[0].buf) || total > g.max_object ||
			count != (total + FRADIO_FRAG_DATA - 1) / FRADIO_FRAG_DATA) { g_st.bad_pkts++; return; }
	uint32_t want_len = idx + 1 < count ? FRADIO_FRAG_DATA : total - idx * FRADIO_FRAG_DATA;
	if (len != want_len) { g_st.bad_pkts++; return; }
	asm_t *a = asm_of(id);
	if (!a) {
		// one overheard, for something not here: collected all the same --
		// broadcasting pays only if every listener gains
		if (have(id) || !(a = asm_new(id))) return;
	}
	if (!a->total) { a->total = total; a->count = (uint8_t)count; }
	if (a->total != total || a->count != count) { g_st.bad_pkts++; return; }
	a->last_ms = g_now;
	if (a->got[idx / 8] & (1u << (idx % 8))) return;
	a->got[idx / 8] |= (uint8_t)(1u << (idx % 8));
	memcpy(a->buf + idx * FRADIO_FRAG_DATA, p + 5 + FRADIO_ID, len);
	if (++a->have == a->count) complete(a);
}

// -- the interface --

void fradio_init(const fradio_cfg_t *cfg, uint32_t now_ms) {
	g = *cfg;
	if (!g.max_object || g.max_object > FRADIO_MAX) g.max_object = FRADIO_MAX;
	if (!g.pace_ms) g.pace_ms = 2000;
	if (!g.announce_ms) g.announce_ms = 600000;
	if (!g.retry_ms) g.retry_ms = 30000;
	if (!g.retries) g.retries = 5;
	g_up = false;
	g_slot = fstore_peer_slot(RADIO_PEER, 1);
	g_ann_pos = fstore_last();
	g_t_ann = g_t_sent = g_now = now_ms;
	g_qh = g_qn = 0;
	memset(g_asm, 0, sizeof(g_asm));
	memset(&g_st, 0, sizeof(g_st));
}

void fradio_up(bool up) {
	if (up && !g_up) g_t_ann = g_now - g.announce_ms;		// say what is here, at once
	g_up = up;
}

bool fradio_is_up(void) { return g_up; }

void fradio_packet(const uint8_t *p, uint32_t n, uint32_t now_ms) {
	g_now = now_ms;
	if (!n || n > FRADIO_PKT) { g_st.bad_pkts++; return; }
	g_st.got_pkts++;
	switch (p[0]) {
	case 'I':
		if (n < 2 || n != 2u + (uint32_t)p[1] * FRADIO_ID || p[1] > FRADIO_INV_MAX) { g_st.bad_pkts++; return; }
		for (int i = 0; i < p[1]; i++) {
			const uint8_t *id = p + 2 + i * FRADIO_ID;
			asm_t *a;
			if (have(id) || asm_of(id) || !(a = asm_new(id))) continue;
			want(a);
		}
		return;
	case 'W':
		if (n < 1 + FRADIO_ID || n > 1 + FRADIO_ID + 32) { g_st.bad_pkts++; return; }
		serve(p + 1, p + 1 + FRADIO_ID, n - 1 - FRADIO_ID);
		return;
	case 'F':
		if (n < 6 + FRADIO_ID) { g_st.bad_pkts++; return; }
		got_frag(p, n);
		return;
	}
	g_st.bad_pkts++;
}

void fradio_poll(uint32_t now_ms) {
	g_now = now_ms;
	if (!g_up) return;
	announce_new();
	if ((int32_t)(now_ms - g_t_ann) >= (int32_t)g.announce_ms) { g_t_ann = now_ms; announce_newest(); }
	for (int i = 0; i < FRADIO_ASM; i++) {
		asm_t *a = &g_asm[i];
		if (!a->used || (int32_t)(now_ms - a->last_ms) < (int32_t)g.retry_ms) continue;
		if (++a->tries > g.retries) { a->used = false; g_st.given_up++; continue; }
		a->last_ms = now_ms;
		want(a);
	}
	if (g_qn && (int32_t)(now_ms - g_t_sent) >= (int32_t)g.pace_ms) {
		if (g.send(g_q[g_qh].p, g_q[g_qh].n, g.ctx)) {
			if (g_q[g_qh].p[0] == 'F' && g_q[g_qh].p[3 + FRADIO_ID] == 0) g_st.objects_out++;
			g_qh = (g_qh + 1) % FRADIO_QUEUE;
			g_qn--;
			g_st.sent_pkts++;
		}
		g_t_sent = now_ms;
	}
}

const fradio_stats_t *fradio_stats(void) { return &g_st; }
