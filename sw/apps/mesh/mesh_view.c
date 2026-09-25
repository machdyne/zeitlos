/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * What the mesh window shows. See mesh_view.h.
 */

#include <string.h>

#include "zutf8.h"
#include "mesh_view.h"

// -- conversations --

void mesh_view_init(mesh_view_t *v) {
	memset(v, 0, sizeof(*v));
	v->cur_kind = CONV_CHAN;
	v->cur_chan = 0;
}

void mesh_view_conv_of(const mesh_model_t *m, const mesh_msg_t *g,
	uint8_t *kind, uint8_t *chan, uint32_t *peer) {
	if (g->dm) {
		*kind = CONV_DM;
		*chan = 0;
		*peer = (g->from == m->my_num) ? g->to : g->from;
	} else {
		*kind = CONV_CHAN;
		*chan = g->channel;
		*peer = 0;
	}
}

// The tracking slot for a DM peer, made if new (the least active one
// is reused when all are taken).
static int dm_slot(mesh_view_t *v, uint32_t num, bool make) {
	int i, free_i = -1, old_i = 0;
	for (i = 0; i < MESH_DM_TRACK; i++) {
		if (v->dm[i].num == num && num) return i;
		if (!v->dm[i].num && free_i < 0) free_i = i;
		if (v->dm[i].last < v->dm[old_i].last) old_i = i;
	}
	if (!make) return -1;
	i = free_i >= 0 ? free_i : old_i;
	v->dm[i].num = num;
	v->dm[i].unread = 0;
	v->dm[i].last = 0;
	return i;
}

bool mesh_view_in_cur(const mesh_view_t *v, const mesh_model_t *m,
	const mesh_msg_t *g) {
	uint8_t kind, chan;
	uint32_t peer;
	mesh_view_conv_of(m, g, &kind, &chan, &peer);
	if (kind != v->cur_kind) return false;
	return kind == CONV_CHAN ? chan == v->cur_chan : peer == v->cur_peer;
}

void mesh_view_note(mesh_view_t *v, const mesh_model_t *m, const mesh_msg_t *g) {
	uint8_t kind, chan;
	uint32_t peer;
	bool unread = g->status == MSG_RX && !mesh_view_in_cur(v, m, g);

	mesh_view_conv_of(m, g, &kind, &chan, &peer);
	if (kind == CONV_CHAN) {
		if (chan >= MESH_MAX_CHANNELS) return;
		v->chan_last[chan] = g->seq;
		if (unread && v->chan_unread[chan] < 999) v->chan_unread[chan]++;
	} else {
		int i = dm_slot(v, peer, true);
		v->dm[i].last = g->seq;
		if (unread && v->dm[i].unread < 999) v->dm[i].unread++;
	}
}

// Insertion sort of conv[from..to) by (last desc, heard desc). At most
// MESH_MAX_NODES entries, and only rebuilt on events that change it.
static void sort_nodes(mesh_view_t *v, int from, int to) {
	int i, j;
	for (i = from + 1; i < to; i++) {
		mesh_conv_t c = v->conv[i];
		for (j = i - 1; j >= from; j--) {
			bool before = c.last > v->conv[j].last ||
				(c.last == v->conv[j].last && c.heard > v->conv[j].heard);
			if (!before) break;
			v->conv[j + 1] = v->conv[j];
		}
		v->conv[j + 1] = c;
	}
}

void mesh_view_build(mesh_view_t *v, mesh_model_t *m) {
	int i, n = 0, k;

	for (i = 0; i < MESH_MAX_CHANNELS; i++) {
		if (m->chan[i].role == CH_ROLE_DISABLED) continue;
		memset(&v->conv[n], 0, sizeof(v->conv[n]));
		v->conv[n].kind = CONV_CHAN;
		v->conv[n].chan = (uint8_t)i;
		v->conv[n].unread = v->chan_unread[i];
		v->conv[n].last = v->chan_last[i];
		n++;
	}
	// Before the config dump there are no channels yet; the primary is
	// still where a message would go, so it is always offered.
	if (n == 0) {
		memset(&v->conv[0], 0, sizeof(v->conv[0]));
		v->conv[0].kind = CONV_CHAN;
		v->conv[0].unread = v->chan_unread[0];
		v->conv[0].last = v->chan_last[0];
		n = 1;
	}
	k = n;
	for (i = 0; i < MESH_MAX_NODES; i++) {
		mesh_node_t *nd = &m->node[i];
		int s;
		if (!nd->used || nd->num == m->my_num) continue;
		memset(&v->conv[n], 0, sizeof(v->conv[n]));
		v->conv[n].kind = CONV_DM;
		v->conv[n].peer = nd->num;
		v->conv[n].heard = nd->last_heard;
		s = dm_slot(v, nd->num, false);
		if (s >= 0) {
			v->conv[n].unread = v->dm[s].unread;
			v->conv[n].last = v->dm[s].last;
		}
		n++;
	}
	sort_nodes(v, k, n);
	v->nconv = n;
}

int mesh_view_cur(mesh_view_t *v) {
	int i;
	for (i = 0; i < v->nconv; i++) {
		mesh_conv_t *c = &v->conv[i];
		if (c->kind != v->cur_kind) continue;
		if (c->kind == CONV_CHAN ? c->chan == v->cur_chan : c->peer == v->cur_peer)
			return i;
	}
	if (!v->nconv) return -1;
	mesh_view_select(v, 0);
	return 0;
}

void mesh_view_select(mesh_view_t *v, int i) {
	mesh_conv_t *c;
	if (i < 0 || i >= v->nconv) return;
	c = &v->conv[i];
	v->cur_kind = c->kind;
	v->cur_chan = c->chan;
	v->cur_peer = c->peer;
	c->unread = 0;
	if (c->kind == CONV_CHAN) {
		v->chan_unread[c->chan] = 0;
	} else {
		int s = dm_slot(v, c->peer, false);
		if (s >= 0) v->dm[s].unread = 0;
	}
}

void mesh_view_mark_read(mesh_view_t *v) {
	int i;
	for (i = 0; i < MESH_MAX_CHANNELS; i++) v->chan_unread[i] = 0;
	for (i = 0; i < MESH_DM_TRACK; i++) v->dm[i].unread = 0;
	for (i = 0; i < v->nconv; i++) v->conv[i].unread = 0;
}

uint32_t mesh_view_unread_total(const mesh_view_t *v) {
	uint32_t t = 0;
	int i;
	for (i = 0; i < MESH_MAX_CHANNELS; i++) t += v->chan_unread[i];
	for (i = 0; i < MESH_DM_TRACK; i++) t += v->dm[i].unread;
	return t;
}

// -- word wrap --

int mesh_wrap(const char *s, int cols, int indent,
	void (*emit)(void *ctx, const char *p, int len, int line), void *ctx) {
	const char *end = s + strlen(s);
	int line = 0;

	if (cols < 2) cols = 2;
	if (indent > cols - 1) indent = cols - 1;

	while (s < end || line == 0) {
		int room = line ? cols - indent : cols;
		const char *p = s, *brk = NULL, *next;
		int used = 0;

		while (p < end && *p != '\n') {
			const char *before = p;
			int w = z_cp_width(z_utf8_next(&p, end));
			if (used + w > room) { p = before; break; }
			used += w;
			if (*before == ' ') brk = p;		// break after the space
		}
		next = p;
		if (p < end && *p != '\n') {
			if (*p == ' ') next = p + 1;		// the line ends on a space
			else if (brk && brk > s) next = p = brk;
		}
		if (p == s && p < end && *p != '\n' && *p != ' ') {
			// Nothing fits (a wide character in one column): take one
			// character anyway, so the loop always advances.
			z_utf8_next(&p, end);
			next = p;
		}
		if (emit) {
			int len = (int)(p - s);
			// A line broken at a space does not show it.
			while (len > 0 && s[len - 1] == ' ' && next < end && *next != '\n') len--;
			emit(ctx, s, len, line);
		}
		line++;
		s = next;
		if (s < end && *s == '\n') {
			s++;
			if (s == end) {		// a trailing newline: an empty last line
				if (emit) emit(ctx, s, 0, line);
				line++;
			}
		}
	}
	return line;
}

// -- the input line --

void mesh_input_clear(mesh_input_t *in) {
	in->len = 0;
	in->cur = 0;
	in->buf[0] = 0;
}

bool mesh_input_insert(mesh_input_t *in, uint32_t cp) {
	char enc[4];
	int n;
	if (cp < 0x20 || cp == 0x7f || cp > 0x10ffff ||
		(cp >= 0xd800 && cp <= 0xdfff)) return false;
	n = z_utf8_put(cp, enc);
	if (n <= 0 || in->len + n > MESH_PAYLOAD_MAX) return false;
	memmove(in->buf + in->cur + n, in->buf + in->cur, (size_t)(in->len - in->cur));
	memcpy(in->buf + in->cur, enc, (size_t)n);
	in->len += n;
	in->cur += n;
	in->buf[in->len] = 0;
	return true;
}

static void cut(mesh_input_t *in, int from, int to) {
	if (to <= from) return;
	memmove(in->buf + from, in->buf + to, (size_t)(in->len - to));
	in->len -= to - from;
	in->buf[in->len] = 0;
	if (in->cur > to) in->cur -= to - from;
	else if (in->cur > from) in->cur = from;
}

void mesh_input_backspace(mesh_input_t *in) {
	cut(in, z_utf8_prev_off(in->buf, in->len, in->cur), in->cur);
}

void mesh_input_delete(mesh_input_t *in) {
	cut(in, in->cur, z_utf8_next_off(in->buf, in->len, in->cur));
}

void mesh_input_left(mesh_input_t *in) {
	in->cur = z_utf8_prev_off(in->buf, in->len, in->cur);
}

void mesh_input_right(mesh_input_t *in) {
	in->cur = z_utf8_next_off(in->buf, in->len, in->cur);
}

void mesh_input_home(mesh_input_t *in) { in->cur = 0; }
void mesh_input_end(mesh_input_t *in) { in->cur = in->len; }

void mesh_input_word(mesh_input_t *in) {
	int k = in->cur;
	while (k > 0 && in->buf[k - 1] == ' ') k--;
	while (k > 0 && in->buf[k - 1] != ' ') k = z_utf8_prev_off(in->buf, in->len, k);
	cut(in, k, in->cur);
}

int mesh_input_scroll(const mesh_input_t *in, int start, int cols) {
	if (cols < 2) cols = 2;
	if (start > in->cur) start = in->cur;
	if (start < 0) start = 0;
	// Keep a column free for the cursor itself.
	while (z_utf8_cols(in->buf + start, (size_t)(in->cur - start)) > cols - 1)
		start = z_utf8_next_off(in->buf, in->len, start);
	return start;
}
