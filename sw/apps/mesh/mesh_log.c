/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Message history: the record format. See mesh_log.h.
 */

#include <string.h>

#include "mesh_log.h"
#include "mesh_proto.h"		// mesh_str_clean()

// -- writing --

typedef struct {
	char *p;
	int n, cap;
	int err;
} sb_t;

static void put(sb_t *b, char c) {
	if (b->n + 1 >= b->cap) { b->err = 1; return; }
	b->p[b->n++] = c;
}

static void puts_(sb_t *b, const char *s) { while (*s) put(b, *s++); }

static void putdec(sb_t *b, uint32_t v) {
	char t[12];
	int k = 0;
	if (!v) t[k++] = '0';
	while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
	while (k) put(b, t[--k]);
}

static void puthex(sb_t *b, uint32_t v) {
	static const char hx[] = "0123456789abcdef";
	int i;
	for (i = 28; i >= 0; i -= 4) put(b, hx[(v >> i) & 15]);
}

int mesh_log_fmt_msg(const mesh_msg_t *g, char *out, int cap) {
	sb_t b = { out, 0, cap, 0 };
	const char *s;
	puts_(&b, "M\t");
	putdec(&b, g->rx_time); put(&b, '\t');
	puthex(&b, g->from); put(&b, '\t');
	puthex(&b, g->to); put(&b, '\t');
	putdec(&b, g->channel); put(&b, '\t');
	puthex(&b, g->id); put(&b, '\t');
	putdec(&b, g->status); put(&b, '\t');
	putdec(&b, g->err); put(&b, '\t');
	for (s = g->text; *s; s++) {
		if (*s == '\\') puts_(&b, "\\\\");
		else if (*s == '\t') puts_(&b, "\\t");
		else if (*s == '\n') puts_(&b, "\\n");
		else if (*s == '\r') puts_(&b, "\\r");
		else put(&b, *s);
	}
	put(&b, '\n');
	if (b.err) return 0;
	out[b.n] = 0;
	return b.n;
}

int mesh_log_fmt_status(const mesh_msg_t *g, char *out, int cap) {
	sb_t b = { out, 0, cap, 0 };
	puts_(&b, "S\t");
	puthex(&b, g->id); put(&b, '\t');
	putdec(&b, g->status); put(&b, '\t');
	putdec(&b, g->err);
	put(&b, '\n');
	if (b.err) return 0;
	out[b.n] = 0;
	return b.n;
}

// -- reading --

// The next tab-separated field: [*p, end of field). Advances past the
// tab. 0 if there is none.
static int field(const char **p, const char **start, int *len) {
	const char *s = *p, *e = s;
	if (!s) return 0;
	while (*e && *e != '\t') e++;
	*start = s;
	*len = (int)(e - s);
	*p = *e ? e + 1 : NULL;
	return 1;
}

static int dec(const char *s, int len, uint32_t *v) {
	uint32_t x = 0;
	int i;
	if (len < 1 || len > 10) return 0;
	for (i = 0; i < len; i++) {
		if (s[i] < '0' || s[i] > '9') return 0;
		if (x > 429496729u || (x == 429496729u && s[i] > '5')) return 0;
		x = x * 10 + (uint32_t)(s[i] - '0');
	}
	*v = x;
	return 1;
}

static int hex(const char *s, int len, uint32_t *v) {
	uint32_t x = 0;
	int i;
	if (len != 8) return 0;
	for (i = 0; i < 8; i++) {
		char c = s[i];
		int d = (c >= '0' && c <= '9') ? c - '0' :
			(c >= 'a' && c <= 'f') ? c - 'a' + 10 :
			(c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
		if (d < 0) return 0;
		x = x << 4 | (uint32_t)d;
	}
	*v = x;
	return 1;
}

int mesh_log_apply(mesh_model_t *m, const char *line) {
	const char *p = line, *f;
	int n;
	uint32_t t, from, to, ch, id, st, err;

	if (!field(&p, &f, &n) || n != 1) return 0;
	if (f[0] == 'S') {
		mesh_msg_t *g;
		if (!field(&p, &f, &n) || !hex(f, n, &id)) return 0;
		if (!field(&p, &f, &n) || !dec(f, n, &st) || st > MSG_UNHEARD) return 0;
		if (!field(&p, &f, &n) || !dec(f, n, &err) || err > 255) return 0;
		g = mesh_msg_find(m, id);
		if (!g || g->status == MSG_RX) return 0;
		g->status = (uint8_t)st;
		g->err = (uint8_t)err;
		return 1;
	}
	if (f[0] != 'M') return 0;
	if (!field(&p, &f, &n) || !dec(f, n, &t)) return 0;
	if (!field(&p, &f, &n) || !hex(f, n, &from)) return 0;
	if (!field(&p, &f, &n) || !hex(f, n, &to)) return 0;
	if (!field(&p, &f, &n) || !dec(f, n, &ch) || ch >= MESH_MAX_CHANNELS) return 0;
	if (!field(&p, &f, &n) || !hex(f, n, &id)) return 0;
	if (!field(&p, &f, &n) || !dec(f, n, &st) || st > MSG_UNHEARD) return 0;
	if (!field(&p, &f, &n) || !dec(f, n, &err) || err > 255) return 0;
	if (!p) return 0;		// the text field must be there, even empty
	{
		// Unescape into a scratch, then clean it as if it had come off
		// the air: a hand-edited log is untrusted text like any other.
		char raw[MESH_PAYLOAD_MAX + 1];
		int k = 0;
		const char *s = p;
		mesh_msg_t *g;
		while (*s && k < MESH_PAYLOAD_MAX) {
			char c = *s++;
			if (c == '\\' && *s) {
				char e = *s++;
				c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
			}
			raw[k++] = c;
		}
		g = mesh_msg_new(m);
		g->rx_time = t;
		g->from = from;
		g->to = to;
		g->channel = (uint8_t)ch;
		g->id = id;
		g->status = (uint8_t)st;
		g->err = (uint8_t)err;
		g->dm = to != MESH_BROADCAST;
		g->snr_x10 = MESH_UNKNOWN_SNR;
		// A message we were still sending when mesh stopped will never
		// hear its ack now.
		if (g->status == MSG_SENDING) g->status = MSG_UNHEARD;
		g->len = (uint16_t)mesh_str_clean(g->text, sizeof(g->text),
			(const uint8_t *)raw, (uint32_t)k, 1);
	}
	return 1;
}

void mesh_log_rd_init(mesh_log_rd_t *rd) {
	memset(rd, 0, sizeof(*rd));
}

static void line_done(mesh_log_rd_t *rd, mesh_model_t *m) {
	rd->line[rd->n] = 0;
	if (!rd->overlong && rd->n && rd->line[0] != '#') {
		if (mesh_log_apply(m, rd->line)) {
			if (rd->line[0] == 'M') rd->loaded++;
		} else {
			rd->bad++;
		}
	} else if (rd->overlong) {
		rd->bad++;
	}
	rd->n = 0;
	rd->overlong = 0;
}

void mesh_log_feed(mesh_log_rd_t *rd, mesh_model_t *m, const char *p, int len) {
	int i;
	for (i = 0; i < len; i++) {
		char c = p[i];
		if (c == '\n') { line_done(rd, m); continue; }
		if (c == '\r') continue;
		if (rd->n >= MESH_LOG_LINE_MAX) { rd->overlong = 1; continue; }
		rd->line[rd->n++] = c;
	}
}

void mesh_log_finish(mesh_log_rd_t *rd, mesh_model_t *m) {
	if (rd->n || rd->overlong) line_done(rd, m);
}
