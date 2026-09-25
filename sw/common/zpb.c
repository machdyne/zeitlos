/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Protocol Buffers wire format. See zpb.h.
 */

#include <string.h>

#include "zpb.h"

// -- reading --

void zpb_rd_init(zpb_rd_t *r, const void *buf, uint32_t len) {
	r->p = (const uint8_t *)buf;
	r->end = r->p + len;
	r->err = false;
}

void zpb_sub(zpb_rd_t *sub, const zpb_field_t *f) {
	zpb_rd_init(sub, f->ptr, f->wt == ZPB_LEN ? f->len : 0);
}

// Up to ten bytes; a tenth byte with its continuation bit set is not a
// varint. Bits beyond 64 are dropped, as the spec allows.
static bool rd_varint(zpb_rd_t *r, uint64_t *out) {
	uint64_t v = 0;
	unsigned shift = 0;
	int i;
	for (i = 0; i < 10; i++) {
		uint8_t b;
		if (r->p >= r->end) return false;
		b = *r->p++;
		if (shift < 64) v |= (uint64_t)(b & 0x7f) << shift;
		shift += 7;
		if (!(b & 0x80)) {
			*out = v;
			return true;
		}
	}
	return false;
}

static uint32_t rd_le32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
		((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool zpb_next(zpb_rd_t *r, zpb_field_t *f) {
	uint64_t tag, n;

	if (r->err || r->p >= r->end) return false;
	memset(f, 0, sizeof(*f));

	if (!rd_varint(r, &tag) || tag >> 32) goto bad;
	f->num = (uint32_t)(tag >> 3);
	f->wt = (uint8_t)(tag & 7);
	if (f->num == 0) goto bad;

	switch (f->wt) {
	case ZPB_VARINT:
		if (!rd_varint(r, &f->v)) goto bad;
		return true;
	case ZPB_I64:
		if (r->end - r->p < 8) goto bad;
		f->v = (uint64_t)rd_le32(r->p) | ((uint64_t)rd_le32(r->p + 4) << 32);
		r->p += 8;
		return true;
	case ZPB_I32:
		if (r->end - r->p < 4) goto bad;
		f->v = rd_le32(r->p);
		r->p += 4;
		return true;
	case ZPB_LEN:
		if (!rd_varint(r, &n)) goto bad;
		if (n > (uint64_t)(r->end - r->p)) goto bad;
		f->ptr = r->p;
		f->len = (uint32_t)n;
		r->p += n;
		return true;
	default:		// groups (3, 4) and the unassigned 6, 7
		goto bad;
	}

bad:
	r->err = true;
	r->p = r->end;
	return false;
}

float zpb_float(const zpb_field_t *f) {
	uint32_t u = (uint32_t)f->v;
	float x;
	memcpy(&x, &u, sizeof(x));
	return x;
}

// -- writing --

void zpb_wr_init(zpb_wr_t *w, void *buf, uint32_t cap) {
	w->start = (uint8_t *)buf;
	w->p = w->start;
	w->end = w->start + cap;
	w->err = false;
}

static void wr_byte(zpb_wr_t *w, uint8_t b) {
	if (w->err) return;
	if (w->p >= w->end) {
		w->err = true;
		return;
	}
	*w->p++ = b;
}

static void wr_varint(zpb_wr_t *w, uint64_t v) {
	while (v >= 0x80) {
		wr_byte(w, (uint8_t)(v | 0x80));
		v >>= 7;
	}
	wr_byte(w, (uint8_t)v);
}

static void wr_tag(zpb_wr_t *w, uint32_t num, uint8_t wt) {
	wr_varint(w, ((uint64_t)num << 3) | wt);
}

static void wr_le32(zpb_wr_t *w, uint32_t v) {
	wr_byte(w, (uint8_t)v);
	wr_byte(w, (uint8_t)(v >> 8));
	wr_byte(w, (uint8_t)(v >> 16));
	wr_byte(w, (uint8_t)(v >> 24));
}

void zpb_put_varint(zpb_wr_t *w, uint32_t num, uint64_t v) {
	wr_tag(w, num, ZPB_VARINT);
	wr_varint(w, v);
}

void zpb_put_int32(zpb_wr_t *w, uint32_t num, int32_t v) {
	// Sign-extended to 64 bits: the spec's encoding for a negative
	// int32, and what every other implementation will expect to read.
	zpb_put_varint(w, num, (uint64_t)(int64_t)v);
}

void zpb_put_sint32(zpb_wr_t *w, uint32_t num, int32_t v) {
	uint32_t u = ((uint32_t)v << 1) ^ (uint32_t)(v >> 31);
	zpb_put_varint(w, num, u);
}

void zpb_put_fixed32(zpb_wr_t *w, uint32_t num, uint32_t v) {
	wr_tag(w, num, ZPB_I32);
	wr_le32(w, v);
}

void zpb_put_float(zpb_wr_t *w, uint32_t num, float v) {
	uint32_t u;
	memcpy(&u, &v, sizeof(u));
	zpb_put_fixed32(w, num, u);
}

void zpb_put_bytes(zpb_wr_t *w, uint32_t num, const void *data, uint32_t len) {
	wr_tag(w, num, ZPB_LEN);
	wr_varint(w, len);
	if (w->err) return;
	if ((uint32_t)(w->end - w->p) < len) {
		w->err = true;
		return;
	}
	if (len) memcpy(w->p, data, len);
	w->p += len;
}

void zpb_put_str(zpb_wr_t *w, uint32_t num, const char *s) {
	zpb_put_bytes(w, num, s, (uint32_t)strlen(s));
}

void zpb_put_msg(zpb_wr_t *w, uint32_t num, const zpb_wr_t *sub) {
	if (sub->err) w->err = true;
	zpb_put_bytes(w, num, sub->start, zpb_wr_len(sub));
}
