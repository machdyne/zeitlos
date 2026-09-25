/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * FromRadio in, ToRadio out. See mesh_proto.h and mesh_pb.h.
 */

#include <string.h>

#include "zpb.h"
#include "mesh_frame.h"
#include "mesh_proto.h"

// A known field with the wrong wire type fails the whole message.
#define WANT(f, t) do { if ((f)->wt != (t)) return false; } while (0)

// -- helpers --

int32_t mesh_f32_x10(uint32_t bits) {
	uint32_t e = (bits >> 23) & 0xff;
	uint64_t v;
	int shift;
	int32_t r;

	if (e == 0 || e == 0xff) return 0;		// zero, denormal, inf, NaN
	v = (uint64_t)((bits & 0x7fffff) | 0x800000) * 10u;
	shift = (int)e - 150;
	if (shift >= 0) {
		if (shift > 20) v = 0x7fffffff;
		else v <<= shift;
	} else if (-shift >= 40) {
		v = 0;
	} else {
		v = (v + ((uint64_t)1 << (-shift - 1))) >> -shift;
	}
	if (v > 0x7fffffff) v = 0x7fffffff;
	r = (int32_t)v;
	return (bits >> 31) ? -r : r;
}

// Length of a valid UTF-8 sequence at s (n bytes available), or 0.
static int utf8_len(const uint8_t *s, uint32_t n) {
	uint8_t c = s[0];
	int k, i;
	if (c < 0x80) return 1;
	if (c >= 0xc2 && c <= 0xdf) k = 2;
	else if (c >= 0xe0 && c <= 0xef) k = 3;
	else if (c >= 0xf0 && c <= 0xf4) k = 4;
	else return 0;
	if ((uint32_t)k > n) return 0;
	for (i = 1; i < k; i++) if ((s[i] & 0xc0) != 0x80) return 0;
	// overlongs, surrogates, past U+10FFFF
	if (c == 0xe0 && s[1] < 0xa0) return 0;
	if (c == 0xed && s[1] > 0x9f) return 0;
	if (c == 0xf0 && s[1] < 0x90) return 0;
	if (c == 0xf4 && s[1] > 0x8f) return 0;
	return k;
}

uint32_t mesh_str_clean(char *dst, uint32_t cap, const uint8_t *src,
	uint32_t len, int keep_nl) {
	uint32_t i = 0, o = 0;
	if (!cap) return 0;
	while (i < len) {
		int k = utf8_len(src + i, len - i);
		if (k == 1) {
			uint8_t c = src[i];
			if (o + 1 >= cap) break;
			if (c == '\n' && keep_nl) dst[o++] = '\n';
			else if (c < 0x20 || c == 0x7f) dst[o++] = ' ';
			else dst[o++] = (char)c;
			i++;
		} else if (k > 1) {
			if (o + (uint32_t)k >= cap) break;		// never split one
			memcpy(dst + o, src + i, (size_t)k);
			o += (uint32_t)k;
			i += (uint32_t)k;
		} else {
			if (o + 1 >= cap) break;
			dst[o++] = '?';
			i++;
		}
	}
	dst[o] = 0;
	return o;
}

static void put_str(char *dst, uint32_t cap, const zpb_field_t *f) {
	mesh_str_clean(dst, cap, f->ptr, f->len, 0);
}

// -- User, Position, DeviceMetrics: into a scratch node --

static bool parse_user(const zpb_field_t *uf, mesh_node_t *n) {
	zpb_rd_t r;
	zpb_field_t f;
	zpb_sub(&r, uf);
	while (zpb_next(&r, &f)) {
		switch (f.num) {
		case US_ID:			WANT(&f, ZPB_LEN); break;
		case US_LONG_NAME:	WANT(&f, ZPB_LEN); put_str(n->long_name, sizeof(n->long_name), &f); break;
		case US_SHORT_NAME:	WANT(&f, ZPB_LEN); put_str(n->short_name, sizeof(n->short_name), &f); break;
		case US_HW_MODEL:	WANT(&f, ZPB_VARINT); n->hw_model = (uint16_t)zpb_u32(&f); break;
		case US_ROLE:		WANT(&f, ZPB_VARINT); n->role = (uint8_t)zpb_u32(&f); break;
		default: break;
		}
	}
	if (r.err) return false;
	n->has_user = 1;
	return true;
}

static bool parse_position(const zpb_field_t *pf, mesh_node_t *n) {
	zpb_rd_t r;
	zpb_field_t f;
	int32_t lat = 0, lon = 0;
	zpb_sub(&r, pf);
	while (zpb_next(&r, &f)) {
		switch (f.num) {
		case PO_LAT_I:		WANT(&f, ZPB_I32); lat = zpb_i32(&f); break;
		case PO_LON_I:		WANT(&f, ZPB_I32); lon = zpb_i32(&f); break;
		case PO_ALTITUDE:	WANT(&f, ZPB_VARINT); break;
		case PO_TIME:		WANT(&f, ZPB_I32); break;
		default: break;
		}
	}
	if (r.err) return false;
	// 0,0 is what a node with no fix sends; it is not a place anyone is.
	if (lat || lon) {
		// Out of range is a corrupt frame, not a position.
		if (lat > 900000000 || lat < -900000000 ||
			lon > 1800000000 || lon < -1800000000) return false;
		n->lat_i = lat;
		n->lon_i = lon;
		n->has_pos = 1;
	}
	return true;
}

static bool parse_metrics(const zpb_field_t *mf, mesh_node_t *n) {
	zpb_rd_t r;
	zpb_field_t f;
	zpb_sub(&r, mf);
	while (zpb_next(&r, &f)) {
		switch (f.num) {
		case DM_BATTERY:
			WANT(&f, ZPB_VARINT);
			n->battery = zpb_u32(&f) > 101 ? 255 : (uint8_t)zpb_u32(&f);
			break;
		case DM_VOLTAGE:	WANT(&f, ZPB_I32); break;
		default: break;
		}
	}
	return !r.err;
}

// Copy what a scratch node learned into the real one.
static void node_merge(mesh_node_t *dst, const mesh_node_t *src) {
	if (src->has_user) {
		memcpy(dst->long_name, src->long_name, sizeof(dst->long_name));
		memcpy(dst->short_name, src->short_name, sizeof(dst->short_name));
		dst->hw_model = src->hw_model;
		dst->role = src->role;
		dst->has_user = 1;
	}
	if (src->has_pos) {
		dst->lat_i = src->lat_i;
		dst->lon_i = src->lon_i;
		dst->has_pos = 1;
	}
	if (src->battery != 255) dst->battery = src->battery;
	if (src->snr_x10 != MESH_UNKNOWN_SNR) dst->snr_x10 = src->snr_x10;
	if (src->hops_away >= 0) dst->hops_away = src->hops_away;
	if (src->last_heard) dst->last_heard = src->last_heard;
}

static void scratch_init(mesh_node_t *n) {
	memset(n, 0, sizeof(*n));
	n->hops_away = -1;
	n->battery = 255;
	n->snr_x10 = MESH_UNKNOWN_SNR;
}

// -- FromRadio payloads --

static bool rx_node_info(mesh_model_t *m, const zpb_field_t *nf, mesh_ev_t *ev) {
	zpb_rd_t r;
	zpb_field_t f;
	mesh_node_t t;
	uint32_t num = 0;
	bool have_num = false, fav = false;

	scratch_init(&t);
	zpb_sub(&r, nf);
	while (zpb_next(&r, &f)) {
		switch (f.num) {
		case NI_NUM:	WANT(&f, ZPB_VARINT); num = zpb_u32(&f); have_num = true; break;
		case NI_USER:	WANT(&f, ZPB_LEN); if (!parse_user(&f, &t)) return false; break;
		case NI_POSITION: WANT(&f, ZPB_LEN); if (!parse_position(&f, &t)) return false; break;
		case NI_SNR:	WANT(&f, ZPB_I32); t.snr_x10 = (int16_t)mesh_f32_x10(zpb_u32(&f)); break;
		case NI_LAST_HEARD: WANT(&f, ZPB_I32); t.last_heard = zpb_u32(&f); break;
		case NI_DEVICE_METRICS: WANT(&f, ZPB_LEN); if (!parse_metrics(&f, &t)) return false; break;
		case NI_HOPS_AWAY: WANT(&f, ZPB_VARINT);
			t.hops_away = zpb_u32(&f) > 127 ? 127 : (int8_t)zpb_u32(&f); break;
		case NI_IS_FAVORITE: WANT(&f, ZPB_VARINT); fav = zpb_bool(&f); break;
		default: break;
		}
	}
	// A node with no number, or the broadcast address, is not a node.
	if (r.err || !have_num || num == 0 || num == MESH_BROADCAST) return false;
	ev->node = mesh_node_get(m, num);
	node_merge(ev->node, &t);
	ev->node->favorite = fav;
	ev->kind = MESH_EV_NODE;
	return true;
}

static bool rx_channel(mesh_model_t *m, const zpb_field_t *cf, mesh_ev_t *ev) {
	zpb_rd_t r, rs;
	zpb_field_t f, g;
	uint32_t idx = 0, role = 0;
	char name[MESH_CHAN_NAME_MAX];

	name[0] = 0;
	zpb_sub(&r, cf);
	while (zpb_next(&r, &f)) {
		switch (f.num) {
		case CH_INDEX:	WANT(&f, ZPB_VARINT); idx = zpb_u32(&f); break;
		case CH_ROLE:	WANT(&f, ZPB_VARINT); role = zpb_u32(&f); break;
		case CH_SETTINGS:
			WANT(&f, ZPB_LEN);
			zpb_sub(&rs, &f);
			while (zpb_next(&rs, &g))
				if (g.num == CS_NAME) {
					WANT(&g, ZPB_LEN);
					put_str(name, sizeof(name), &g);
				}
			if (rs.err) return false;
			break;
		default: break;
		}
	}
	if (r.err || idx >= MESH_MAX_CHANNELS || role > CH_ROLE_SECONDARY) return false;
	m->chan[idx].role = (uint8_t)role;
	memcpy(m->chan[idx].name, name, sizeof(name));
	ev->kind = MESH_EV_CHANNEL;
	return true;
}

static bool rx_config(mesh_model_t *m, const zpb_field_t *cf, mesh_ev_t *ev) {
	zpb_rd_t r, rl;
	zpb_field_t f, g;
	zpb_sub(&r, cf);
	ev->kind = MESH_EV_CONFIG;
	while (zpb_next(&r, &f)) {
		if (f.num != CF_LORA) continue;
		{
			bool use_preset = false;
			int preset = 0, region = 0, hops = 0;
			WANT(&f, ZPB_LEN);
			zpb_sub(&rl, &f);
			while (zpb_next(&rl, &g)) {
				switch (g.num) {
				case LC_USE_PRESET:	WANT(&g, ZPB_VARINT); use_preset = zpb_bool(&g); break;
				case LC_MODEM_PRESET: WANT(&g, ZPB_VARINT); preset = (int)zpb_u32(&g); break;
				case LC_REGION:		WANT(&g, ZPB_VARINT); region = (int)zpb_u32(&g); break;
				case LC_HOP_LIMIT:	WANT(&g, ZPB_VARINT); hops = (int)zpb_u32(&g); break;
				default: break;
				}
			}
			if (rl.err || preset > 64 || hops > 7) return false;
			m->modem_preset = use_preset ? preset : -2;
			m->region = region;
			m->hop_limit = (uint8_t)hops;
		}
	}
	return !r.err;
}

static bool rx_metadata(mesh_model_t *m, const zpb_field_t *mf, mesh_ev_t *ev) {
	zpb_rd_t r;
	zpb_field_t f;
	char fw[sizeof(m->firmware)];
	uint16_t hw = m->hw_model;
	fw[0] = 0;
	zpb_sub(&r, mf);
	while (zpb_next(&r, &f)) {
		switch (f.num) {
		case MD_FIRMWARE_VERSION: WANT(&f, ZPB_LEN); put_str(fw, sizeof(fw), &f); break;
		case MD_HW_MODEL: WANT(&f, ZPB_VARINT); hw = (uint16_t)zpb_u32(&f); break;
		default: break;
		}
	}
	if (r.err) return false;
	if (fw[0]) memcpy(m->firmware, fw, sizeof(fw));
	m->hw_model = hw;
	ev->kind = MESH_EV_CONFIG;
	return true;
}

// -- MeshPacket --

typedef struct {
	uint32_t from, to, id, rx_time, chan, hop_limit, hop_start;
	int32_t snr_x10, rssi;
	bool have_snr, have_decoded, encrypted, pki;
	uint32_t port;
	const uint8_t *payload;
	uint32_t payload_len;
	uint32_t request_id, emoji;
} pkt_t;

static bool parse_data(const zpb_field_t *df, pkt_t *p) {
	zpb_rd_t r;
	zpb_field_t f;
	zpb_sub(&r, df);
	while (zpb_next(&r, &f)) {
		switch (f.num) {
		case DA_PORTNUM:	WANT(&f, ZPB_VARINT); p->port = zpb_u32(&f); break;
		case DA_PAYLOAD:	WANT(&f, ZPB_LEN); p->payload = f.ptr; p->payload_len = f.len; break;
		case DA_REQUEST_ID:	WANT(&f, ZPB_I32); p->request_id = zpb_u32(&f); break;
		case DA_REPLY_ID:	WANT(&f, ZPB_I32); break;
		case DA_EMOJI:		WANT(&f, ZPB_I32); p->emoji = zpb_u32(&f); break;
		default: break;
		}
	}
	if (r.err || p->payload_len > MESH_PAYLOAD_MAX) return false;
	p->have_decoded = true;
	return true;
}

static bool parse_packet(const zpb_field_t *pf, pkt_t *p) {
	zpb_rd_t r;
	zpb_field_t f;
	memset(p, 0, sizeof(*p));
	zpb_sub(&r, pf);
	while (zpb_next(&r, &f)) {
		switch (f.num) {
		case MP_FROM:		WANT(&f, ZPB_I32); p->from = zpb_u32(&f); break;
		case MP_TO:			WANT(&f, ZPB_I32); p->to = zpb_u32(&f); break;
		case MP_CHANNEL:	WANT(&f, ZPB_VARINT); p->chan = zpb_u32(&f); break;
		case MP_DECODED:	WANT(&f, ZPB_LEN); if (!parse_data(&f, p)) return false; break;
		case MP_ENCRYPTED:	WANT(&f, ZPB_LEN); p->encrypted = true; break;
		case MP_ID:			WANT(&f, ZPB_I32); p->id = zpb_u32(&f); break;
		case MP_RX_TIME:	WANT(&f, ZPB_I32); p->rx_time = zpb_u32(&f); break;
		case MP_RX_SNR:		WANT(&f, ZPB_I32); p->snr_x10 = mesh_f32_x10(zpb_u32(&f)); p->have_snr = true; break;
		case MP_HOP_LIMIT:	WANT(&f, ZPB_VARINT); p->hop_limit = zpb_u32(&f); break;
		case MP_WANT_ACK:	WANT(&f, ZPB_VARINT); break;
		case MP_RX_RSSI:	WANT(&f, ZPB_VARINT); p->rssi = zpb_i32(&f); break;
		case MP_HOP_START:	WANT(&f, ZPB_VARINT); p->hop_start = zpb_u32(&f); break;
		case MP_PKI_ENCRYPTED: WANT(&f, ZPB_VARINT); p->pki = zpb_bool(&f); break;
		default: break;
		}
	}
	if (r.err) return false;
	// Sanity: hop counts are three bits, the channel an index into eight,
	// and RSSI a small negative number. A shifted frame fails one of
	// these far more often than not.
	if (p->hop_limit > 7 || p->hop_start > 7 || p->chan >= MESH_MAX_CHANNELS)
		return false;
	if (p->rssi > 0 || p->rssi < -200) return false;
	return true;
}

static bool rx_routing(mesh_model_t *m, const pkt_t *p, mesh_ev_t *ev) {
	zpb_rd_t r;
	zpb_field_t f;
	uint32_t err = RT_ERR_NONE;
	mesh_msg_t *g;

	zpb_rd_init(&r, p->payload, p->payload_len);
	while (zpb_next(&r, &f))
		if (f.num == RT_ERROR) { WANT(&f, ZPB_VARINT); err = zpb_u32(&f); }
	if (r.err) return false;

	ev->kind = MESH_EV_PACKET;
	if (!p->request_id) return true;
	g = mesh_msg_find(m, p->request_id);
	if (!g || g->from != m->my_num || g->status == MSG_RX) return true;

	if (err != RT_ERR_NONE) {
		// A broadcast is transmitted whether or not anyone repeats it;
		// the firmware's want_ack for one means "listen for a neighbour
		// rebroadcasting it", and MAX_RETRANSMIT only says none was
		// heard. Calling that "failed" said the message did not go out,
		// which it did. Hardware showed exactly this on a quiet mesh.
		if (!g->dm && err == RT_ERR_MAX_RETRANSMIT) {
			if (g->status == MSG_SENDING) g->status = MSG_UNHEARD;
		} else {
			g->status = MSG_FAILED;
			g->err = (uint8_t)err;
		}
	} else if (g->dm && p->from == g->to) {
		g->status = MSG_DELIVERED;
	} else if (g->status == MSG_SENDING || g->status == MSG_UNHEARD) {
		// From our own node: it heard a neighbour rebroadcast ours (the
		// implicit ack). Or anyone acking a broadcast. Either way the
		// mesh has it; for a DM, delivery may still follow.
		g->status = MSG_SENT;
	}
	ev->kind = MESH_EV_STATUS;
	ev->msg = g;
	return true;
}

static bool rx_packet(mesh_model_t *m, const zpb_field_t *pf, mesh_ev_t *ev) {
	pkt_t p;
	mesh_node_t t, *n = NULL;
	zpb_field_t pl;

	if (!parse_packet(pf, &p)) return false;
	ev->kind = MESH_EV_PACKET;

	// Parse the payload BEFORE touching the model, so a bad payload
	// changes nothing.
	scratch_init(&t);
	memset(&pl, 0, sizeof(pl));
	pl.wt = ZPB_LEN;
	pl.ptr = p.payload;
	pl.len = p.payload_len;
	if (p.have_decoded) {
		if (p.port == PORT_NODEINFO && !parse_user(&pl, &t)) return false;
		if (p.port == PORT_POSITION && !parse_position(&pl, &t)) return false;
		if (p.port == PORT_TELEMETRY) {
			zpb_rd_t r;
			zpb_field_t f;
			zpb_rd_init(&r, p.payload, p.payload_len);
			while (zpb_next(&r, &f))
				if (f.num == TE_DEVICE_METRICS) {
					WANT(&f, ZPB_LEN);
					if (!parse_metrics(&f, &t)) return false;
				}
			if (r.err) return false;
		}
	}

	// Anything heard from a node says it is alive, and how well.
	if (p.from && p.from != MESH_BROADCAST) {
		n = mesh_node_get(m, p.from);
		if (p.rx_time) t.last_heard = p.rx_time;
		if (p.have_snr && p.rx_time) t.snr_x10 = (int16_t)p.snr_x10;
		if (p.hop_start && p.hop_start >= p.hop_limit)
			t.hops_away = (int8_t)(p.hop_start - p.hop_limit);
		node_merge(n, &t);
		ev->node = n;
	}
	if (!p.have_decoded) return true;

	switch (p.port) {
	case PORT_TEXT: {
		mesh_msg_t *g;
		// The node can hand the same packet over twice (queued while no
		// client was attached, then again). Same sender and id is the
		// same message.
		g = mesh_msg_find(m, p.id);
		if (g && p.id && g->from == p.from) return true;
		g = mesh_msg_new(m);
		g->id = p.id;
		g->from = p.from;
		g->to = p.to;
		g->channel = (uint8_t)p.chan;
		g->rx_time = p.rx_time;
		g->dm = p.to != MESH_BROADCAST;
		g->status = MSG_RX;
		g->snr_x10 = p.have_snr ? (int16_t)p.snr_x10 : MESH_UNKNOWN_SNR;
		g->rssi = (int16_t)p.rssi;
		g->len = (uint16_t)mesh_str_clean(g->text, sizeof(g->text),
			p.payload, p.payload_len, 1);
		ev->kind = MESH_EV_TEXT;
		ev->msg = g;
		return true;
	}
	case PORT_ROUTING:
		return rx_routing(m, &p, ev);
	case PORT_NODEINFO:
	case PORT_POSITION:
	case PORT_TELEMETRY:
		ev->kind = MESH_EV_NODE;
		return true;
	default:
		return true;
	}
}

int mesh_proto_rx(mesh_model_t *m, const uint8_t *pb, uint32_t len, mesh_ev_t *ev) {
	static char note[128];
	zpb_rd_t r, rs;
	zpb_field_t f, g;
	bool ok = true;

	memset(ev, 0, sizeof(*ev));
	zpb_rd_init(&r, pb, len);
	while (ok && zpb_next(&r, &f)) {
		switch (f.num) {
		case FR_ID:
			ok = f.wt == ZPB_VARINT;
			break;
		case FR_PACKET:
			ok = f.wt == ZPB_LEN && rx_packet(m, &f, ev);
			break;
		case FR_MY_INFO:
			ok = f.wt == ZPB_LEN;
			if (!ok) break;
			zpb_sub(&rs, &f);
			while (zpb_next(&rs, &g))
				if (g.num == MI_MY_NODE_NUM) {
					if (g.wt != ZPB_VARINT) { ok = false; break; }
					if (zpb_u32(&g) && zpb_u32(&g) != MESH_BROADCAST) {
						m->my_num = zpb_u32(&g);
						mesh_node_get(m, m->my_num);
					}
				}
			if (rs.err) ok = false;
			ev->kind = MESH_EV_MY_INFO;
			break;
		case FR_NODE_INFO:
			ok = f.wt == ZPB_LEN && rx_node_info(m, &f, ev);
			break;
		case FR_CONFIG:
			ok = f.wt == ZPB_LEN && rx_config(m, &f, ev);
			break;
		case FR_MODULE_CONFIG:
		case FR_LOG_RECORD:
			ok = f.wt == ZPB_LEN;
			ev->kind = MESH_EV_CONFIG;
			break;
		case FR_CONFIG_COMPLETE:
			ok = f.wt == ZPB_VARINT;
			ev->kind = MESH_EV_CONFIG_DONE;
			ev->nonce = zpb_u32(&f);
			break;
		case FR_REBOOTED:
			ok = f.wt == ZPB_VARINT;
			ev->kind = MESH_EV_REBOOTED;
			break;
		case FR_CHANNEL:
			ok = f.wt == ZPB_LEN && rx_channel(m, &f, ev);
			break;
		case FR_QUEUE_STATUS:
			ok = f.wt == ZPB_LEN;
			if (!ok) break;
			zpb_sub(&rs, &f);
			while (zpb_next(&rs, &g)) {
				if (g.wt != ZPB_VARINT) continue;
				if (g.num == QS_FREE) m->queue_free = (uint8_t)zpb_u32(&g);
				if (g.num == QS_MAXLEN) m->queue_max = (uint8_t)zpb_u32(&g);
			}
			if (rs.err) ok = false;
			ev->kind = MESH_EV_QUEUE;
			break;
		case FR_METADATA:
			ok = f.wt == ZPB_LEN && rx_metadata(m, &f, ev);
			break;
		case FR_NOTIFICATION:
			ok = f.wt == ZPB_LEN;
			if (!ok) break;
			note[0] = 0;
			zpb_sub(&rs, &f);
			while (zpb_next(&rs, &g))
				if (g.num == CN_MESSAGE && g.wt == ZPB_LEN)
					put_str(note, sizeof(note), &g);
			if (rs.err) ok = false;
			ev->kind = MESH_EV_NOTIFY;
			ev->text = note;
			break;
		default:
			break;		// newer than us: skipped
		}
	}
	if (!ok || r.err) {
		m->rejected++;
		memset(ev, 0, sizeof(*ev));
		ev->kind = MESH_EV_REJECT;
	}
	return ev->kind;
}

// -- ToRadio --

static uint32_t finish(uint8_t *out, uint32_t cap, const zpb_wr_t *w) {
	if (w->err) return 0;
	return mesh_frame_wrap(out, cap, w->start, zpb_wr_len(w));
}

uint32_t mesh_tx_want_config(uint8_t *out, uint32_t cap, uint32_t nonce) {
	uint8_t b[16];
	zpb_wr_t w;
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_varint(&w, TR_WANT_CONFIG, nonce);
	return finish(out, cap, &w);
}

uint32_t mesh_tx_heartbeat(uint8_t *out, uint32_t cap, uint32_t nonce) {
	uint8_t b[16], h[8];
	zpb_wr_t w, hw;
	zpb_wr_init(&hw, h, sizeof(h));
	zpb_put_varint(&hw, 1, nonce);			// Heartbeat.nonce
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_msg(&w, TR_HEARTBEAT, &hw);
	return finish(out, cap, &w);
}

uint32_t mesh_tx_disconnect(uint8_t *out, uint32_t cap) {
	uint8_t b[8];
	zpb_wr_t w;
	zpb_wr_init(&w, b, sizeof(b));
	zpb_put_varint(&w, TR_DISCONNECT, 1);
	return finish(out, cap, &w);
}

uint32_t mesh_tx_text(uint8_t *out, uint32_t cap, uint32_t to, uint8_t channel,
	uint32_t id, uint8_t hop_limit, const char *text, uint32_t len) {
	uint8_t db[MESH_PAYLOAD_MAX + 16], pb[MESH_PAYLOAD_MAX + 48], tb[MESH_PAYLOAD_MAX + 56];
	zpb_wr_t d, p, t;

	if (len > MESH_PAYLOAD_MAX || !id) return 0;
	zpb_wr_init(&d, db, sizeof(db));
	zpb_put_varint(&d, DA_PORTNUM, PORT_TEXT);
	zpb_put_bytes(&d, DA_PAYLOAD, text, len);

	zpb_wr_init(&p, pb, sizeof(pb));
	zpb_put_fixed32(&p, MP_TO, to);
	if (channel) zpb_put_varint(&p, MP_CHANNEL, channel);
	zpb_put_msg(&p, MP_DECODED, &d);
	zpb_put_fixed32(&p, MP_ID, id);
	if (hop_limit) zpb_put_varint(&p, MP_HOP_LIMIT, hop_limit);
	zpb_put_varint(&p, MP_WANT_ACK, 1);

	zpb_wr_init(&t, tb, sizeof(tb));
	zpb_put_msg(&t, TR_PACKET, &p);
	return finish(out, cap, &t);
}

mesh_msg_t *mesh_proto_sent(mesh_model_t *m, uint32_t to, uint8_t channel,
	uint32_t id, const char *text, uint32_t len) {
	mesh_msg_t *g = mesh_msg_new(m);
	g->id = id;
	g->from = m->my_num;
	g->to = to;
	g->channel = channel;
	g->dm = to != MESH_BROADCAST;
	g->status = MSG_SENDING;
	g->snr_x10 = MESH_UNKNOWN_SNR;
	g->len = (uint16_t)mesh_str_clean(g->text, sizeof(g->text),
		(const uint8_t *)text, len, 1);
	return g;
}
