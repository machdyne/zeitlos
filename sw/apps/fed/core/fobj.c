/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zfed objects. docs/fed.md, "Objects" and "Encoding, exactly"; the
 * layout in fobj.h.
 *
 * An object is checked as the bytes it is, never re-encoded: a node
 * stores and forwards exactly what it received. So the parser is strict
 * about every byte -- each header line is the one field allowed at that
 * point, in the one form allowed -- because anything it tolerated, some
 * other implementation would read differently, and a signature over
 * bytes two parsers disagree on is how forgeries get in.
 */
#include <stdio.h>
#include <string.h>
#include "fobj.h"
#include "../../../common/zsha256.h"
#include "../../../common/zjson.h"
#include "../../../common/z25519.h"
#include "../../../ext/monocypher/monocypher-ed25519.h"

static const char sig_prefix[12] = { 'z','f','e','d',' ','o','b','j','e','c','t', 0 };

// -- field rules, shared by parse and make --

static bool name_char(uint8_t c, bool slash) {
	return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || (slash && c == '/');
}

// 1..max bytes of a-z 0-9 . _ - (and / with `slash`); with `slash`, no
// empty segment: not first, not last, never two in a row.
static bool name_ok(const uint8_t *s, uint32_t n, uint32_t max, bool slash) {
	if (n < 1 || n > max) return false;
	for (uint32_t i = 0; i < n; i++) {
		if (!name_char(s[i], slash)) return false;
		if (s[i] == '/' && (i == 0 || i == n - 1 || s[i - 1] == '/')) return false;
	}
	return true;
}

// Decimal, no sign, no leading zero, at most 2^53.
static bool dec_ok(const uint8_t *s, uint32_t n, uint64_t *v) {
	*v = 0;
	if (n < 1 || n > 16 || (n > 1 && s[0] == '0')) return false;
	for (uint32_t i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9') return false;
		*v = *v * 10 + (uint64_t)(s[i] - '0');
	}
	return *v <= FOBJ_INT_MAX;
}

// Exactly 2n lowercase hex digits.
static bool hex_ok(const uint8_t *s, uint32_t len, uint8_t *out, uint32_t n) {
	if (len != 2 * n) return false;
	for (uint32_t i = 0; i < 2 * n; i++) {
		uint8_t c = s[i], d;
		if (c >= '0' && c <= '9') d = (uint8_t)(c - '0');
		else if (c >= 'a' && c <= 'f') d = (uint8_t)(c - 'a' + 10);
		else return false;
		if (i & 1) out[i / 2] |= d; else out[i / 2] = (uint8_t)(d << 4);
	}
	return true;
}

// -- parse --

typedef struct { const uint8_t *b; uint32_t n, pos, where; } cur_t;

// The next header line must be "name: value", the value 1+ bytes of
// printable ASCII with no spaces. -error if not.
static int field(cur_t *c, const char *name, const uint8_t **val, uint32_t *vlen) {
	uint32_t nl = c->pos, nlen = (uint32_t)strlen(name);
	c->where = c->pos;
	while (nl < c->n && c->b[nl] != '\n') {
		if (nl >= FOBJ_HEADER_MAX) return -FOBJ_E_HEADER;
		nl++;
	}
	if (nl >= c->n) return c->n >= FOBJ_HEADER_MAX ? -FOBJ_E_HEADER : -FOBJ_E_SHORT;
	if (nl - c->pos < nlen + 2 || memcmp(c->b + c->pos, name, nlen) || c->b[c->pos + nlen] != ':')
		return -FOBJ_E_FIELD;
	if (c->b[c->pos + nlen + 1] != ' ') return -FOBJ_E_SPACE;
	*val = c->b + c->pos + nlen + 2;
	*vlen = nl - (c->pos + nlen + 2);
	if (*vlen == 0) return -FOBJ_E_LINE;
	for (uint32_t i = 0; i < *vlen; i++)
		if ((*val)[i] < 0x21 || (*val)[i] > 0x7e) { c->where = (uint32_t)(*val - c->b) + i; return (*val)[i] == ' ' ? -FOBJ_E_SPACE : -FOBJ_E_LINE; }
	c->pos = nl + 1;
	return 0;
}

static bool is(const uint8_t *v, uint32_t n, const char *s) {
	return n == strlen(s) && !memcmp(v, s, n);
}

int fobj_parse(const uint8_t *b, uint32_t n, uint64_t now, fobj_t *o, uint32_t *where) {
	cur_t c = { b, n, 0, 0 };
	const uint8_t *v;
	uint32_t vl;
	uint64_t x;
	int r;
	memset(o, 0, sizeof(*o));
#define FAIL(e) do { if (where) *where = c.where; return -(e); } while (0)
#define FIELD(name) do { if ((r = field(&c, (name), &v, &vl)) < 0) { if (where) *where = c.where; return r; } } while (0)

	if (n < 6 || memcmp(b, FOBJ_MAGIC "\n", 6)) FAIL(FOBJ_E_MAGIC);
	c.pos = 6;

	FIELD("topic");
	if (!name_ok(v, vl, FOBJ_TOPIC_MAX, true)) FAIL(FOBJ_E_TOPIC);
	memcpy(o->topic, v, vl);
	FIELD("type");
	if (!name_ok(v, vl, FOBJ_TYPE_MAX, false)) FAIL(FOBJ_E_TYPE);
	memcpy(o->type, v, vl);
	FIELD("format");
	if (is(v, vl, "json")) o->format = FOBJ_JSON;
	else if (is(v, vl, "text")) o->format = FOBJ_TEXT;
	else if (is(v, vl, "bytes")) o->format = FOBJ_BYTES;
	else FAIL(FOBJ_E_FORMAT);
	FIELD("kind");
	if (is(v, vl, "log")) o->kind = FOBJ_LOG;
	else if (is(v, vl, "state")) o->kind = FOBJ_STATE;
	else FAIL(FOBJ_E_KIND);
	FIELD("origin");
	if (!hex_ok(v, vl, o->origin, 32)) FAIL(FOBJ_E_ORIGIN);
	FIELD("time");
	if (!dec_ok(v, vl, &o->time)) FAIL(FOBJ_E_NUMBER);
	if (now && o->time > now + FOBJ_FUTURE) FAIL(FOBJ_E_FUTURE);
	FIELD("seq");
	if (!dec_ok(v, vl, &o->seq)) FAIL(FOBJ_E_NUMBER);
	if (o->seq == 0) FAIL(FOBJ_E_SEQ);
	if (o->kind == FOBJ_STATE) {
		FIELD("key");
		if (!name_ok(v, vl, FOBJ_KEY_MAX, true)) FAIL(FOBJ_E_KEY);
		memcpy(o->key, v, vl);
	}
	FIELD("len");
	if (!dec_ok(v, vl, &x)) FAIL(FOBJ_E_NUMBER);
	if (x > FOBJ_PAYLOAD_MAX) FAIL(FOBJ_E_LEN);
	o->len = (uint32_t)x;

	// the blank line
	c.where = c.pos;
	if (c.pos >= n) FAIL(FOBJ_E_SHORT);
	if (b[c.pos] != '\n') FAIL(FOBJ_E_FIELD);			// a field after len: unknown, or out of order
	c.pos++;
	if (c.pos > FOBJ_HEADER_MAX) FAIL(FOBJ_E_HEADER);

	// the payload, then the signature line
	uint32_t hdr = c.pos;
	if ((uint64_t)hdr + o->len + FOBJ_SIGLINE > n) { c.where = n; FAIL(FOBJ_E_SHORT); }
	o->payload = b + hdr;
	c.pos = hdr + o->len;
	c.where = c.pos;
	if (memcmp(b + c.pos, "\nsig: ", 6) || b[c.pos + FOBJ_SIGLINE - 1] != '\n' ||
			!hex_ok(b + c.pos + 6, 128, o->sig, 64))
		FAIL(FOBJ_E_SIGLINE);
	o->size = c.pos + FOBJ_SIGLINE;

	if ((o->format == FOBJ_JSON || o->format == FOBJ_TEXT) && !zjson_utf8((const char *)o->payload, o->len)) {
		c.where = hdr;
		FAIL(FOBJ_E_PAYLOAD);
	}

	z_sha256(o->id, b, hdr + o->len);
	if (where) *where = 0;
	return 0;
#undef FAIL
#undef FIELD
}

int fobj_verify(const fobj_t *o) {
	uint8_t m[44];
	memcpy(m, sig_prefix, 12);
	memcpy(m + 12, o->id, 32);
	return z_ed25519_check(o->sig, o->origin, m, sizeof(m)) == 0 ? 0 : -FOBJ_E_SIG;
}

// -- make --

int fobj_make(fobj_t *o, const uint8_t *payload, const uint8_t secret_key[64], uint8_t *out, uint32_t cap) {
	static const char *fmts[] = { "", "json", "text", "bytes" };
	static const char *kinds[] = { "", "log", "state" };
	char origin[65], sig[129];
	int h;

	memcpy(o->origin, secret_key + 32, 32);		// Monocypher: seed, then the public key
	if (!name_ok((const uint8_t *)o->topic, (uint32_t)strlen(o->topic), FOBJ_TOPIC_MAX, true)) return -FOBJ_E_TOPIC;
	if (!name_ok((const uint8_t *)o->type, (uint32_t)strlen(o->type), FOBJ_TYPE_MAX, false)) return -FOBJ_E_TYPE;
	if (o->format < FOBJ_JSON || o->format > FOBJ_BYTES) return -FOBJ_E_FORMAT;
	if (o->kind != FOBJ_LOG && o->kind != FOBJ_STATE) return -FOBJ_E_KIND;
	if (o->time > FOBJ_INT_MAX || o->seq > FOBJ_INT_MAX) return -FOBJ_E_NUMBER;
	if (o->seq == 0) return -FOBJ_E_SEQ;
	if (o->kind == FOBJ_STATE && !name_ok((const uint8_t *)o->key, (uint32_t)strlen(o->key), FOBJ_KEY_MAX, true)) return -FOBJ_E_KEY;
	if (o->kind == FOBJ_LOG && o->key[0]) return -FOBJ_E_KEY;
	if (o->len > FOBJ_PAYLOAD_MAX) return -FOBJ_E_LEN;
	if ((o->format == FOBJ_JSON || o->format == FOBJ_TEXT) && !zjson_utf8((const char *)payload, o->len)) return -FOBJ_E_PAYLOAD;

	fobj_hex(o->origin, 32, origin);
	if (o->kind == FOBJ_STATE)
		h = snprintf((char *)out, cap, FOBJ_MAGIC "\ntopic: %s\ntype: %s\nformat: %s\nkind: %s\norigin: %s\n"
			"time: %llu\nseq: %llu\nkey: %s\nlen: %u\n\n", o->topic, o->type, fmts[o->format], kinds[o->kind],
			origin, (unsigned long long)o->time, (unsigned long long)o->seq, o->key, (unsigned)o->len);
	else
		h = snprintf((char *)out, cap, FOBJ_MAGIC "\ntopic: %s\ntype: %s\nformat: %s\nkind: %s\norigin: %s\n"
			"time: %llu\nseq: %llu\nlen: %u\n\n", o->topic, o->type, fmts[o->format], kinds[o->kind],
			origin, (unsigned long long)o->time, (unsigned long long)o->seq, (unsigned)o->len);
	if (h < 0 || h > FOBJ_HEADER_MAX) return -FOBJ_E_HEADER;
	if ((uint64_t)h + o->len + FOBJ_SIGLINE > cap) return -FOBJ_E_SHORT;
	memcpy(out + h, payload, o->len);
	z_sha256(o->id, out, (uint32_t)h + o->len);
	{
		uint8_t m[44];
		memcpy(m, sig_prefix, 12);
		memcpy(m + 12, o->id, 32);
		crypto_ed25519_sign(o->sig, secret_key, m, sizeof(m));
	}
	fobj_hex(o->sig, 64, sig);
	memcpy(out + h + o->len, "\nsig: ", 6);
	memcpy(out + h + o->len + 6, sig, 128);
	out[h + o->len + FOBJ_SIGLINE - 1] = '\n';
	o->payload = out + h;
	o->size = (uint32_t)h + o->len + FOBJ_SIGLINE;
	return (int)o->size;
}

// -- names --

void fobj_hex(const uint8_t *b, uint32_t n, char *out) {
	static const char d[] = "0123456789abcdef";
	for (uint32_t i = 0; i < n; i++) { out[2*i] = d[b[i] >> 4]; out[2*i+1] = d[b[i] & 15]; }
	out[2 * n] = 0;
}

void fobj_short_id(const uint8_t public_key[32], char out[17]) {
	uint8_t h[32];
	z_sha256(h, public_key, 32);
	fobj_hex(h, 8, out);
}

const char *fobj_strerror(int err) {
	static const char *names[] = { "ok", "not a zfed object (magic)", "bad header line", "missing, unknown or misordered field",
		"bad topic", "bad type", "bad format", "bad kind", "bad origin", "bad number", "time too far ahead",
		"seq is 0", "bad key", "payload too long", "header too long", "truncated", "bad signature line",
		"payload is not UTF-8", "signature does not check", "stray space" };
	if (err < 0) err = -err;
	return err < (int)(sizeof(names) / sizeof(names[0])) ? names[err] : "?";
}
