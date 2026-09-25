/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * mesh's model. See mesh_model.h.
 */

#include <string.h>

#include "mesh_model.h"

void mesh_model_init(mesh_model_t *m) {
	memset(m, 0, sizeof(*m));
	m->modem_preset = -1;
	m->region = -1;
}

void mesh_model_forget_node(mesh_model_t *m) {
	m->my_num = 0;
	m->firmware[0] = 0;
	m->hw_model = 0;
	m->modem_preset = -1;
	m->region = -1;
	m->hop_limit = 0;
	memset(m->chan, 0, sizeof(m->chan));
	memset(m->node, 0, sizeof(m->node));
	m->clock = 0;
}

void mesh_model_forget_config(mesh_model_t *m) {
	m->firmware[0] = 0;
	m->modem_preset = -1;
	m->region = -1;
	m->hop_limit = 0;
	memset(m->chan, 0, sizeof(m->chan));
}

mesh_node_t *mesh_node_find(mesh_model_t *m, uint32_t num) {
	int i;
	for (i = 0; i < MESH_MAX_NODES; i++)
		if (m->node[i].used && m->node[i].num == num) return &m->node[i];
	return NULL;
}

mesh_node_t *mesh_node_get(mesh_model_t *m, uint32_t num) {
	mesh_node_t *n = mesh_node_find(m, num);
	int i, victim = -1;
	uint32_t oldest = 0xffffffffu;

	if (n) {
		n->seen = ++m->clock;
		return n;
	}
	for (i = 0; i < MESH_MAX_NODES; i++)
		if (!m->node[i].used) { victim = i; break; }
	if (victim < 0) {
		// Full: the least recently seen that is neither us nor a
		// favourite. There is always one, since both are few.
		for (i = 0; i < MESH_MAX_NODES; i++) {
			mesh_node_t *c = &m->node[i];
			if (c->num == m->my_num || c->favorite) continue;
			if (c->seen < oldest) { oldest = c->seen; victim = i; }
		}
		if (victim < 0) victim = 0;
		m->evicted++;
	}
	n = &m->node[victim];
	memset(n, 0, sizeof(*n));
	n->used = 1;
	n->num = num;
	n->hops_away = -1;
	n->battery = 255;
	n->snr_x10 = MESH_UNKNOWN_SNR;
	n->seen = ++m->clock;
	return n;
}

int mesh_node_count(const mesh_model_t *m) {
	int i, c = 0;
	for (i = 0; i < MESH_MAX_NODES; i++) if (m->node[i].used) c++;
	return c;
}

static char lower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool same_ci(const char *a, const char *b) {
	while (*a && *b && lower(*a) == lower(*b)) { a++; b++; }
	return *a == 0 && *b == 0;
}

static int hexval(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	c = lower(c);
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

mesh_node_t *mesh_node_lookup(mesh_model_t *m, const char *s) {
	int i;
	if (s[0] == '!') {
		uint32_t v = 0;
		for (i = 1; s[i]; i++) {
			int h = hexval(s[i]);
			if (h < 0 || i > 8) return NULL;
			v = v << 4 | (uint32_t)h;
		}
		if (i == 1) return NULL;
		return mesh_node_find(m, v);
	}
	for (i = 0; i < MESH_MAX_NODES; i++)
		if (m->node[i].used && m->node[i].has_user &&
			same_ci(m->node[i].short_name, s)) return &m->node[i];
	for (i = 0; i < MESH_MAX_NODES; i++)
		if (m->node[i].used && m->node[i].has_user &&
			same_ci(m->node[i].long_name, s)) return &m->node[i];
	return NULL;
}

void mesh_node_id(uint32_t num, char *buf) {
	static const char hx[] = "0123456789abcdef";
	int i;
	buf[0] = '!';
	for (i = 0; i < 8; i++) buf[1 + i] = hx[(num >> (28 - 4 * i)) & 15];
	buf[9] = 0;
}

const char *mesh_name(mesh_model_t *m, uint32_t num, char *buf, int cap) {
	mesh_node_t *n;
	if (cap < 10) { if (cap > 0) buf[0] = 0; return buf; }
	if (num == MESH_BROADCAST) {
		strcpy(buf, "^all");
		return buf;
	}
	n = mesh_node_find(m, num);
	if (n && n->has_user && n->short_name[0]) {
		strncpy(buf, n->short_name, (size_t)cap - 1);
		buf[cap - 1] = 0;
	} else {
		mesh_node_id(num, buf);
	}
	return buf;
}

// The firmware's own display names for its modem presets, which is what
// people see on their phones for an unnamed primary channel.
static const char *const preset_names[] = {
	"LongFast", "LongSlow", "VLongSlow", "MediumSlow", "MediumFast",
	"ShortSlow", "ShortFast", "LongMod", "ShortTurbo", "LongTurbo",
	"LiteFast", "LiteSlow", "NarrowFast", "NarrowSlow", "TinyFast",
	"TinySlow", "MediumTurbo",
};

const char *mesh_chan_name(const mesh_model_t *m, int idx) {
	if (idx < 0 || idx >= MESH_MAX_CHANNELS) return "?";
	if (m->chan[idx].name[0]) return m->chan[idx].name;
	// Channel 0 is the primary; before the dump says so, it still is.
	if (m->chan[idx].role == CH_ROLE_PRIMARY || idx == 0) {
		if (m->modem_preset >= 0 &&
			m->modem_preset < (int)(sizeof(preset_names) / sizeof(preset_names[0])))
			return preset_names[m->modem_preset];
		if (m->modem_preset == -2) return "Custom";
		return "Primary";
	}
	return "Channel";
}

mesh_msg_t *mesh_msg_new(mesh_model_t *m) {
	mesh_msg_t *g = &m->msg[m->msg_head];
	memset(g, 0, sizeof(*g));
	g->seq = ++m->msg_seq;
	m->msg_head = (m->msg_head + 1) % MESH_MAX_MSGS;
	if (m->msg_count < MESH_MAX_MSGS) m->msg_count++;
	return g;
}

mesh_msg_t *mesh_msg_at(mesh_model_t *m, uint32_t i) {
	uint32_t first = (m->msg_head + MESH_MAX_MSGS - m->msg_count) % MESH_MAX_MSGS;
	if (i >= m->msg_count) return NULL;
	return &m->msg[(first + i) % MESH_MAX_MSGS];
}

mesh_msg_t *mesh_msg_find(mesh_model_t *m, uint32_t id) {
	uint32_t i;
	for (i = m->msg_count; i > 0; i--) {
		mesh_msg_t *g = mesh_msg_at(m, i - 1);
		if (g->id == id) return g;
	}
	return NULL;
}

const char *mesh_status_name(const mesh_msg_t *g) {
	switch (g->status) {
	case MSG_RX:		return "";
	case MSG_SENDING:	return "sending";
	case MSG_SENT:		return "sent";
	case MSG_DELIVERED:	return "delivered";
	case MSG_UNHEARD:	return "sent, no relay heard";
	default: break;
	}
	switch (g->err) {
	case RT_ERR_NO_ROUTE:		return "no route";
	case RT_ERR_GOT_NAK:		return "refused";
	case RT_ERR_TIMEOUT:		return "timed out";
	case RT_ERR_NO_INTERFACE:	return "no radio";
	case RT_ERR_MAX_RETRANSMIT:	return "no ack";
	case RT_ERR_NO_CHANNEL:		return "no channel";
	case RT_ERR_TOO_LARGE:		return "too long";
	case RT_ERR_NO_RESPONSE:	return "no response";
	case RT_ERR_DUTY_CYCLE:		return "duty cycle limit";
	case RT_ERR_PKI_FAILED:		return "encryption failed";
	case RT_ERR_PKI_UNKNOWN_KEY: return "no key for node";
	default:					return "failed";
	}
}
