/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A strict JSON parser: zfed's subset. docs/zjson.md; the rules in
 * zjson.h. Portable C99, no malloc, no recursion deeper than
 * ZJSON_MAX_DEPTH.
 *
 * One more limit than zjson.h's list: at most ZJSON_MAX_KEYS keys in one
 * object. Duplicate keys are found by comparing every pair, and without
 * a bound a 16 KB object of two thousand tiny keys would cost millions
 * of comparisons -- seconds, on the board, from one object anyone can
 * send.
 */
#include <string.h>
#include "zjson.h"

#define ZJSON_MAX_KEYS 256

typedef struct {
	const char *js;
	uint32_t len, pos;
	zjson_tok_t *tok;
	int ntok, n;
	int err;
	uint32_t where;
} ps_t;

static int fail(ps_t *p, int err) {
	if (!p->err) { p->err = err; p->where = p->pos; }
	return -1;
}

static void ws(ps_t *p) {
	while (p->pos < p->len) {
		char c = p->js[p->pos];
		if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
		p->pos++;
	}
}

static int new_tok(ps_t *p, int type) {
	if (p->n >= p->ntok) return fail(p, ZJ_E_TOKENS);
	zjson_tok_t *t = &p->tok[p->n];
	memset(t, 0, sizeof(*t));
	t->type = (uint8_t)type;
	t->start = p->pos;
	return p->n++;
}

// One UTF-8 character at p->pos (valid, shortest form, no surrogates,
// at most U+10FFFF); advances. False if it is not.
static bool utf8_char(ps_t *p) {
	const uint8_t *s = (const uint8_t *)p->js;
	uint8_t c = s[p->pos];
	uint32_t need, cp, min;
	if (c < 0x80) { p->pos++; return true; }
	if (c >= 0xC2 && c <= 0xDF) { need = 1; cp = c & 0x1F; min = 0x80; }
	else if (c >= 0xE0 && c <= 0xEF) { need = 2; cp = c & 0x0F; min = 0x800; }
	else if (c >= 0xF0 && c <= 0xF4) { need = 3; cp = c & 0x07; min = 0x10000; }
	else return false;
	for (uint32_t i = 1; i <= need; i++) {
		if (p->pos + i >= p->len) return false;
		uint8_t b = s[p->pos + i];
		if ((b & 0xC0) != 0x80) return false;
		cp = (cp << 6) | (b & 0x3F);
	}
	if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
	p->pos += need + 1;
	return true;
}

static int hex4(const char *s, uint32_t *v) {
	*v = 0;
	for (int i = 0; i < 4; i++) {
		char c = s[i];
		uint32_t d;
		if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
		else return 0;
		*v = (*v << 4) | d;
	}
	return 1;
}

static int parse_string(ps_t *p) {
	int t;
	p->pos++;						// the opening quote
	t = new_tok(p, ZJ_STRING);
	if (t < 0) return -1;
	while (1) {
		if (p->pos >= p->len) return fail(p, ZJ_E_SYNTAX);
		uint8_t c = (uint8_t)p->js[p->pos];
		if (c == '"') break;
		if (c < 0x20) return fail(p, ZJ_E_CONTROL);
		if (c == '\\') {
			if (p->pos + 1 >= p->len) return fail(p, ZJ_E_SYNTAX);
			char e = p->js[p->pos + 1];
			if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't') {
				p->pos += 2;
				continue;
			}
			if (e != 'u') { p->pos++; return fail(p, ZJ_E_ESCAPE); }
			uint32_t v, v2;
			if (p->pos + 6 > p->len || !hex4(p->js + p->pos + 2, &v)) return fail(p, ZJ_E_ESCAPE);
			if (v == 0) return fail(p, ZJ_E_NUL);
			if (v >= 0xDC00 && v <= 0xDFFF) return fail(p, ZJ_E_SURROGATE);
			if (v >= 0xD800 && v <= 0xDBFF) {
				// a high surrogate must be followed by a low one
				if (p->pos + 12 > p->len || p->js[p->pos + 6] != '\\' || p->js[p->pos + 7] != 'u' ||
						!hex4(p->js + p->pos + 8, &v2) || v2 < 0xDC00 || v2 > 0xDFFF)
					return fail(p, ZJ_E_SURROGATE);
				p->pos += 12;
				continue;
			}
			p->pos += 6;
			continue;
		}
		if (!utf8_char(p)) return fail(p, ZJ_E_UTF8);
	}
	p->tok[t].end = p->pos;
	p->pos++;						// the closing quote
	return t;
}

static int parse_number(ps_t *p) {
	int t = new_tok(p, ZJ_NUMBER);
	bool neg = false;
	uint64_t v = 0;
	if (t < 0) return -1;
	if (p->js[p->pos] == '-') { neg = true; p->pos++; }
	if (p->pos >= p->len || p->js[p->pos] < '0' || p->js[p->pos] > '9') return fail(p, ZJ_E_NUMBER);
	if (p->js[p->pos] == '0') {
		p->pos++;
		if (p->pos < p->len && p->js[p->pos] >= '0' && p->js[p->pos] <= '9') return fail(p, ZJ_E_NUMBER);
		if (neg) return fail(p, ZJ_E_NUMBER);	// -0
	} else {
		while (p->pos < p->len && p->js[p->pos] >= '0' && p->js[p->pos] <= '9') {
			v = v * 10 + (uint64_t)(p->js[p->pos] - '0');
			if (v > (uint64_t)ZJSON_MAX_INT) return fail(p, ZJ_E_RANGE);
			p->pos++;
		}
	}
	if (p->pos < p->len && (p->js[p->pos] == '.' || p->js[p->pos] == 'e' || p->js[p->pos] == 'E'))
		return fail(p, ZJ_E_NUMBER);
	p->tok[t].end = p->pos;
	p->tok[t].num = neg ? -(int64_t)v : (int64_t)v;
	return t;
}

static int literal(ps_t *p, const char *word, int type) {
	uint32_t n = (uint32_t)strlen(word);
	if (p->pos + n > p->len || memcmp(p->js + p->pos, word, n)) return fail(p, ZJ_E_SYNTAX);
	int t = new_tok(p, type);
	if (t < 0) return -1;
	p->pos += n;
	p->tok[t].end = p->pos;
	return t;
}

// The next byte of a string's unescaped form: escapes decoded to UTF-8.
// *i walks the input; q holds the rest of a decoded character.
typedef struct { const char *js; uint32_t i, end; uint8_t q[4]; int qn, qi; } unesc_t;

static void put_utf8(unesc_t *u, uint32_t cp) {
	u->qi = 0;
	if (cp < 0x80) { u->q[0] = (uint8_t)cp; u->qn = 1; }
	else if (cp < 0x800) { u->q[0] = (uint8_t)(0xC0 | (cp >> 6)); u->q[1] = (uint8_t)(0x80 | (cp & 0x3F)); u->qn = 2; }
	else if (cp < 0x10000) {
		u->q[0] = (uint8_t)(0xE0 | (cp >> 12)); u->q[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
		u->q[2] = (uint8_t)(0x80 | (cp & 0x3F)); u->qn = 3;
	} else {
		u->q[0] = (uint8_t)(0xF0 | (cp >> 18)); u->q[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
		u->q[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); u->q[3] = (uint8_t)(0x80 | (cp & 0x3F)); u->qn = 4;
	}
}

// -1 at the end. The string was already validated by parse_string().
static int unesc_next(unesc_t *u) {
	if (u->qi < u->qn) return u->q[u->qi++];
	if (u->i >= u->end) return -1;
	char c = u->js[u->i];
	if (c != '\\') { u->i++; return (uint8_t)c; }
	char e = u->js[u->i + 1];
	u->i += 2;
	switch (e) {
	case 'b': return '\b'; case 'f': return '\f'; case 'n': return '\n';
	case 'r': return '\r'; case 't': return '\t';
	case 'u': {
		uint32_t v, v2;
		hex4(u->js + u->i, &v);
		u->i += 4;
		if (v >= 0xD800 && v <= 0xDBFF) {
			hex4(u->js + u->i + 2, &v2);
			u->i += 6;
			v = 0x10000 + ((v - 0xD800) << 10) + (v2 - 0xDC00);
		}
		put_utf8(u, v);
		return u->q[u->qi++];
	}
	default: return (uint8_t)e;		// " \ /
	}
}

static bool keys_equal(const char *js, const zjson_tok_t *a, const zjson_tok_t *b) {
	unesc_t x = { js, a->start, a->end, {0}, 0, 0 }, y = { js, b->start, b->end, {0}, 0, 0 };
	while (1) {
		int c1 = unesc_next(&x), c2 = unesc_next(&y);
		if (c1 != c2) return false;
		if (c1 < 0) return true;
	}
}

static int parse_value(ps_t *p, int depth);

static int parse_container(ps_t *p, int depth, bool obj) {
	int t = new_tok(p, obj ? ZJ_OBJECT : ZJ_ARRAY);
	int keys[ZJSON_MAX_KEYS];
	if (t < 0) return -1;
	if (depth > ZJSON_MAX_DEPTH) return fail(p, ZJ_E_DEPTH);
	p->pos++;
	ws(p);
	if (p->pos < p->len && p->js[p->pos] == (obj ? '}' : ']')) {
		p->pos++;
		p->tok[t].end = p->pos;
		return t;
	}
	while (1) {
		if (obj) {
			ws(p);
			if (p->pos >= p->len || p->js[p->pos] != '"') return fail(p, ZJ_E_SYNTAX);
			if (p->tok[t].size >= ZJSON_MAX_KEYS) return fail(p, ZJ_E_TOKENS);
			int k = parse_string(p);
			if (k < 0) return -1;
			for (uint32_t i = 0; i < p->tok[t].size; i++)
				if (keys_equal(p->js, &p->tok[keys[i]], &p->tok[k])) { p->pos = p->tok[k].start; return fail(p, ZJ_E_DUPLICATE); }
			keys[p->tok[t].size] = k;
			ws(p);
			if (p->pos >= p->len || p->js[p->pos] != ':') return fail(p, ZJ_E_SYNTAX);
			p->pos++;
		}
		if (parse_value(p, depth) < 0) return -1;
		p->tok[t].size++;
		ws(p);
		if (p->pos >= p->len) return fail(p, ZJ_E_SYNTAX);
		char c = p->js[p->pos];
		if (c == ',') { p->pos++; continue; }
		if (c == (obj ? '}' : ']')) { p->pos++; break; }
		return fail(p, ZJ_E_SYNTAX);
	}
	p->tok[t].end = p->pos;
	return t;
}

static int parse_value(ps_t *p, int depth) {
	ws(p);
	if (p->pos >= p->len) return fail(p, ZJ_E_SYNTAX);
	switch (p->js[p->pos]) {
	case '{': return parse_container(p, depth + 1, true);
	case '[': return parse_container(p, depth + 1, false);
	case '"': return parse_string(p);
	case 't': return literal(p, "true", ZJ_TRUE);
	case 'f': return literal(p, "false", ZJ_FALSE);
	case 'n': return literal(p, "null", ZJ_NULL);
	default:
		if (p->js[p->pos] == '-' || (p->js[p->pos] >= '0' && p->js[p->pos] <= '9')) return parse_number(p);
		return fail(p, ZJ_E_SYNTAX);
	}
}

int zjson_parse(const char *js, uint32_t len, zjson_tok_t *tok, int ntok, uint32_t *where) {
	ps_t p = { js, len, 0, tok, ntok, 0, 0, 0 };
	if (len >= 3 && (uint8_t)js[0] == 0xEF && (uint8_t)js[1] == 0xBB && (uint8_t)js[2] == 0xBF) {
		if (where) *where = 0;
		return -ZJ_E_BOM;
	}
	ws(&p);
	if (p.pos >= len) { if (where) *where = p.pos; return -ZJ_E_EMPTY; }
	if (parse_value(&p, 0) < 0) { if (where) *where = p.where; return -p.err; }
	ws(&p);
	if (p.pos != len) { if (where) *where = p.pos; return -ZJ_E_TRAILING; }
	return p.n;
}

const char *zjson_strerror(int err) {
	static const char *names[] = { "ok", "empty", "byte-order mark", "something after the value",
		"too many tokens or keys", "nested too deep", "syntax", "not UTF-8", "unescaped control character",
		"bad escape", "unpaired surrogate", "\\u0000", "not an integer", "integer out of range",
		"duplicate key" };
	if (err < 0) err = -err;
	return (err >= 0 && err < (int)(sizeof(names) / sizeof(names[0]))) ? names[err] : "?";
}

int zjson_next(const zjson_tok_t *tok, int i) {
	int n = 1;							// values still to skip
	while (n > 0) {
		if (tok[i].type == ZJ_OBJECT) n += 2 * (int)tok[i].size;
		else if (tok[i].type == ZJ_ARRAY) n += (int)tok[i].size;
		n--;
		i++;
	}
	return i;
}

bool zjson_utf8(const char *s, uint32_t len) {
	ps_t p = { s, len, 0, NULL, 0, 0, 0, 0 };
	while (p.pos < len) {
		if (s[p.pos] == 0 || !utf8_char(&p)) return false;
	}
	return true;
}

bool zjson_eq(const char *js, const zjson_tok_t *t, const char *s) {
	if (t->type != ZJ_STRING) return false;
	unesc_t u = { js, t->start, t->end, {0}, 0, 0 };
	for (;; s++) {
		int c = unesc_next(&u);
		if (c < 0) return *s == 0;
		if (*s == 0 || (uint8_t)*s != (uint8_t)c) return false;
	}
}

int zjson_get(const char *js, const zjson_tok_t *tok, int obj, const char *key) {
	if (tok[obj].type != ZJ_OBJECT) return -1;
	int i = obj + 1;
	for (uint32_t k = 0; k < tok[obj].size; k++) {
		if (zjson_eq(js, &tok[i], key)) return i + 1;
		i = zjson_next(tok, i + 1);
	}
	return -1;
}

bool zjson_str(const char *js, const zjson_tok_t *t, char *out, uint32_t cap) {
	if (t->type != ZJ_STRING || cap == 0) return false;
	unesc_t u = { js, t->start, t->end, {0}, 0, 0 };
	uint32_t o = 0;
	int c;
	while ((c = unesc_next(&u)) >= 0) {
		if (o + 1 >= cap) { out[0] = 0; return false; }
		out[o++] = (char)c;
	}
	out[o] = 0;
	return true;
}
