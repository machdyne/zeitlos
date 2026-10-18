/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The XML pull tokenizer. See zf_xml.h.
 */

#include <string.h>

#include "zf_xml.h"

void zx_init(zx_t *x, zf_rd_t *rd) {
	memset(x, 0, sizeof(*x));
	x->rd = rd;
}

const char *zx_local(const char *name) {
	const char *c = strchr(name, ':');
	return c ? c + 1 : name;
}

const char *zx_attr(const zx_t *x, const char *name) {
	const char *p = x->attrs, *end = x->attrs + x->alen;
	while (p < end) {
		const char *v = p + strlen(p) + 1;
		if (!strcmp(p, name)) return v;
		p = v + strlen(v) + 1;
	}
	return NULL;
}

const char *zx_attr_local(const zx_t *x, const char *local) {
	const char *p = x->attrs, *end = x->attrs + x->alen;
	while (p < end) {
		const char *v = p + strlen(p) + 1;
		if (!strcmp(zx_local(p), local)) return v;
		p = v + strlen(v) + 1;
	}
	return NULL;
}

static bool is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool is_name(int c) {
	return c > 0x7F || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || c == ':' || c == '_' || c == '-' || c == '.';
}

// After '&': the entity, as UTF-8, into out (at least 5 bytes). An
// unknown or unterminated one is put back as written, so nothing is
// lost.
static int entity(zx_t *x, char *out, int cap) {
	char e[16];
	int n = 0;
	for (;;) {
		int c = zf_rd_peek(x->rd);
		if (c == ';') { zf_rd_getc(x->rd); break; }
		if (c < 0 || n >= (int)sizeof(e) - 1 || is_space(c) || c == '<' || c == '&') {
			// not an entity: '&' and what was read, literally
			int k = 0;
			out[k++] = '&';
			for (int i = 0; i < n && k < cap; i++) out[k++] = e[i];
			return k;
		}
		e[n++] = (char)zf_rd_getc(x->rd);
	}
	e[n] = 0;
	uint32_t cp = zf_entity(e);
	if (!cp) {
		int k = 0;
		out[k++] = '&';
		for (int i = 0; i < n && k < cap - 1; i++) out[k++] = e[i];
		out[k++] = ';';
		return k;
	}
	return zf_utf8_put(cp, out);
}

// Skips until `end` (a 2- or 3-character terminator) has been read.
static bool skip_until(zx_t *x, const char *end) {
	int n = (int)strlen(end), m = 0;
	for (;;) {
		int c = zf_rd_getc(x->rd);
		if (c < 0) return false;
		if (c == end[m]) { if (++m == n) return true; }
		else m = (c == end[0]) ? 1 : 0;
	}
}

// Text, up to the next '<' or a full buffer -- in CDATA, up to "]]>".
static zx_tok_t text(zx_t *x) {
	x->tlen = 0;
	for (;;) {
		int c = zf_rd_peek(x->rd);
		if (c < 0) break;
		if (x->tlen >= ZX_TEXT_MAX - 8 && (c & 0xC0) != 0x80) break;
		if (x->in_cdata) {
			if (c == ']') {
				zf_rd_getc(x->rd);
				if (zf_rd_peek(x->rd) == ']') {
					zf_rd_getc(x->rd);
					if (zf_rd_peek(x->rd) == '>') { zf_rd_getc(x->rd); x->in_cdata = false; break; }
					x->text[x->tlen++] = ']';
				}
				x->text[x->tlen++] = ']';
				continue;
			}
			x->text[x->tlen++] = (char)zf_rd_getc(x->rd);
			continue;
		}
		if (c == '<') break;
		zf_rd_getc(x->rd);
		if (c == '&') {
			x->tlen += (uint32_t)entity(x, &x->text[x->tlen], 6);
			continue;
		}
		if (c == '\r') {			// line ends are \n in XML
			if (zf_rd_peek(x->rd) == '\n') continue;
			c = '\n';
		}
		x->text[x->tlen++] = (char)c;
	}
	x->text[x->tlen] = 0;
	return x->tlen ? ZX_TEXT : ZX_EOF;
}

static bool read_name(zx_t *x, char *out, int cap) {
	int n = 0;
	while (is_name(zf_rd_peek(x->rd))) {
		int c = zf_rd_getc(x->rd);
		if (n < cap - 1) out[n++] = (char)c;
	}
	out[n] = 0;
	return n > 0;
}

static void skip_space(zx_t *x) {
	while (is_space(zf_rd_peek(x->rd))) zf_rd_getc(x->rd);
}

static zx_tok_t start_tag(zx_t *x) {
	x->alen = 0;
	x->empty = false;
	if (!read_name(x, x->name, sizeof x->name)) return ZX_ERR;
	for (;;) {
		skip_space(x);
		int c = zf_rd_peek(x->rd);
		if (c < 0) return ZX_ERR;
		if (c == '>') { zf_rd_getc(x->rd); return ZX_START; }
		if (c == '/') {
			zf_rd_getc(x->rd);
			if (zf_rd_getc(x->rd) != '>') return ZX_ERR;
			x->empty = true;
			x->end_next = true;
			return ZX_START;
		}
		char an[ZX_NAME_MAX];
		if (!read_name(x, an, sizeof an)) { zf_rd_getc(x->rd); continue; }	// junk: skip it
		skip_space(x);
		if (zf_rd_peek(x->rd) != '=') continue;		// an attribute with no value
		zf_rd_getc(x->rd);
		skip_space(x);
		int q = zf_rd_getc(x->rd);
		if (q != '"' && q != '\'') return ZX_ERR;
		// name\0value\0, kept only if the whole of it fits
		uint32_t at = x->alen;
		uint32_t nl = (uint32_t)strlen(an) + 1;
		bool fits = at + nl < ZX_ATTR_MAX;
		if (fits) { memcpy(&x->attrs[at], an, nl); at += nl; }
		for (;;) {
			c = zf_rd_getc(x->rd);
			if (c < 0) return ZX_ERR;
			if (c == q) break;
			char u[8];
			int k = 1;
			if (c == '&') k = entity(x, u, 6);
			else u[0] = (char)c;
			if (fits && at + (uint32_t)k + 1 < ZX_ATTR_MAX) { memcpy(&x->attrs[at], u, (size_t)k); at += (uint32_t)k; }
			else fits = false;
		}
		if (fits) { x->attrs[at++] = 0; x->alen = at; }
	}
}

zx_tok_t zx_next(zx_t *x) {
	if (x->end_next) {
		x->end_next = false;
		x->empty = false;
		return ZX_END;		// name is still the element's
	}
	for (;;) {
		if (x->in_cdata) return text(x);
		int c = zf_rd_peek(x->rd);
		if (c < 0) return x->rd->err ? ZX_ERR : ZX_EOF;
		if (c != '<') return text(x);
		zf_rd_getc(x->rd);
		c = zf_rd_peek(x->rd);
		if (c == '/') {
			zf_rd_getc(x->rd);
			read_name(x, x->name, sizeof x->name);
			while ((c = zf_rd_getc(x->rd)) >= 0 && c != '>') ;
			return c < 0 ? ZX_ERR : ZX_END;
		}
		if (c == '?') { if (!skip_until(x, "?>")) return ZX_ERR; continue; }
		if (c == '!') {
			zf_rd_getc(x->rd);
			if (zf_rd_peek(x->rd) == '-') { if (!skip_until(x, "-->")) return ZX_ERR; continue; }
			if (zf_rd_peek(x->rd) == '[') {
				// <![CDATA[
				char k[8];
				int n = 0;
				while (n < 7) { int d = zf_rd_getc(x->rd); if (d < 0) return ZX_ERR; k[n++] = (char)d; }
				k[n] = 0;
				if (strcmp(k, "[CDATA[")) return ZX_ERR;
				x->in_cdata = true;
				continue;
			}
			// <!DOCTYPE ...>, with an internal subset in brackets
			int depth = 0;
			while ((c = zf_rd_getc(x->rd)) >= 0) {
				if (c == '[') depth++;
				else if (c == ']') depth--;
				else if (c == '>' && depth <= 0) break;
			}
			if (c < 0) return ZX_ERR;
			continue;
		}
		return start_tag(x);
	}
}

zx_tok_t zx_skip(zx_t *x) {
	int depth = 1;
	for (;;) {
		zx_tok_t t = zx_next(x);
		if (t == ZX_EOF || t == ZX_ERR) return t;
		if (t == ZX_START) depth++;
		else if (t == ZX_END && --depth == 0) return ZX_END;
	}
}
