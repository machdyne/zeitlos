/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * SSH wire encoding. See ssh_wire.h for the design and, in particular,
 * for why every read is bounds checked against a sticky flag rather
 * than trusted.
 */

#include <string.h>

#include "ssh_wire.h"

// -- reader --

void ssh_rd_init(ssh_rd *r, const void *buf, uint32_t len) {
	r->buf = (const uint8_t *)buf;
	r->len = len;
	r->pos = 0;
	r->bad = false;
}

// Every reader below funnels through this. Once `bad` is set it stays
// set and nothing further advances, so a caller can parse a whole
// message and check once at the end.
static bool take(ssh_rd *r, uint32_t n) {
	if (r->bad) return false;
	if (n > r->len - r->pos) { r->bad = true; return false; }
	return true;
}

uint8_t ssh_rd_u8(ssh_rd *r) {
	if (!take(r, 1)) return 0;
	return r->buf[r->pos++];
}

uint32_t ssh_rd_u32(ssh_rd *r) {
	uint32_t v;
	if (!take(r, 4)) return 0;
	v = ((uint32_t)r->buf[r->pos] << 24) |
		((uint32_t)r->buf[r->pos + 1] << 16) |
		((uint32_t)r->buf[r->pos + 2] << 8) |
		((uint32_t)r->buf[r->pos + 3]);
	r->pos += 4;
	return v;
}

bool ssh_rd_bool(ssh_rd *r) {
	// RFC 4251: any nonzero value is true, not just 1.
	return ssh_rd_u8(r) != 0;
}

const uint8_t *ssh_rd_string(ssh_rd *r, uint32_t *len) {

	uint32_t n;
	const uint8_t *p;

	*len = 0;

	n = ssh_rd_u32(r);
	if (r->bad) return NULL;

	// The length came off the wire, so it can be anything at all,
	// including a value that overflows when added to pos. Comparing
	// against remaining bytes (rather than computing pos + n and
	// comparing that) is what keeps this safe for n near 2^32.
	if (n > r->len - r->pos) { r->bad = true; return NULL; }

	p = r->buf + r->pos;
	r->pos += n;
	*len = n;
	return p;

}

void ssh_rd_skip_string(ssh_rd *r) {
	uint32_t n;
	(void)ssh_rd_string(r, &n);
}

void ssh_rd_skip(ssh_rd *r, uint32_t n) {
	if (!take(r, n)) return;
	r->pos += n;
}

bool ssh_rd_bytes(ssh_rd *r, void *out, uint32_t n) {
	if (!take(r, n)) return false;
	memcpy(out, r->buf + r->pos, n);
	r->pos += n;
	return true;
}

bool ssh_str_eq(const uint8_t *s, uint32_t len, const char *lit) {
	uint32_t n = (uint32_t)strlen(lit);
	if (!s || len != n) return false;
	return memcmp(s, lit, n) == 0;
}

bool ssh_namelist_has(const uint8_t *list, uint32_t len, const char *lit) {

	uint32_t n = (uint32_t)strlen(lit);
	uint32_t i = 0;
	uint32_t start = 0;

	if (!list) return false;

	// Walk comma-separated elements without copying. An empty list, a
	// trailing comma, and an element equal to the whole list all fall
	// out of this correctly.
	for (i = 0; i <= len; i++) {
		if (i == len || list[i] == ',') {
			if (i - start == n && memcmp(list + start, lit, n) == 0)
				return true;
			start = i + 1;
		}
	}

	return false;

}

// -- writer --

void ssh_wr_init(ssh_wr *w, void *buf, uint32_t cap) {
	w->buf = (uint8_t *)buf;
	w->cap = cap;
	w->pos = 0;
	w->bad = false;
}

static bool room(ssh_wr *w, uint32_t n) {
	if (w->bad) return false;
	if (n > w->cap - w->pos) { w->bad = true; return false; }
	return true;
}

void ssh_wr_u8(ssh_wr *w, uint8_t v) {
	if (!room(w, 1)) return;
	w->buf[w->pos++] = v;
}

void ssh_wr_u32(ssh_wr *w, uint32_t v) {
	if (!room(w, 4)) return;
	w->buf[w->pos++] = (uint8_t)(v >> 24);
	w->buf[w->pos++] = (uint8_t)(v >> 16);
	w->buf[w->pos++] = (uint8_t)(v >> 8);
	w->buf[w->pos++] = (uint8_t)v;
}

void ssh_wr_bool(ssh_wr *w, bool v) {
	ssh_wr_u8(w, v ? 1 : 0);
}

void ssh_wr_bytes(ssh_wr *w, const void *data, uint32_t len) {
	if (!room(w, len)) return;
	memcpy(w->buf + w->pos, data, len);
	w->pos += len;
}

void ssh_wr_string(ssh_wr *w, const void *data, uint32_t len) {
	ssh_wr_u32(w, len);
	ssh_wr_bytes(w, data, len);
}

void ssh_wr_cstr(ssh_wr *w, const char *s) {
	ssh_wr_string(w, s, (uint32_t)strlen(s));
}

void ssh_wr_mpint(ssh_wr *w, const uint8_t *be, uint32_t len) {

	uint32_t i = 0;
	bool pad;

	// Strip leading zero bytes. RFC 4251 requires the minimal
	// representation, and a server computing the same exchange hash
	// will have stripped them too -- an extra byte here is a hash
	// mismatch, not a cosmetic difference.
	while (i < len && be[i] == 0) i++;

	// Zero is the empty string, not a single 0x00 byte.
	if (i == len) {
		ssh_wr_u32(w, 0);
		return;
	}

	// A leading byte with its top bit set would read as negative in
	// two's complement, so a 0x00 goes in front. For a uniformly
	// random 32-byte X25519 shared secret this happens about half the
	// time, which is exactly why an implementation that omits it
	// appears to work and then fails every other connection.
	pad = (be[i] & 0x80) != 0;

	ssh_wr_u32(w, (len - i) + (pad ? 1 : 0));
	if (pad) ssh_wr_u8(w, 0);
	ssh_wr_bytes(w, be + i, len - i);

}
