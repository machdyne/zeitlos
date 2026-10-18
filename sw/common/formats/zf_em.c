/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The reader-side paragraph emitter. See zf_em.h.
 */

#include <string.h>

#include "zf_em.h"

void zf_em_init(zf_em_t *e, zf_doc_t *d, bool collapse) {
	memset(e, 0, sizeof(*e));
	e->d = d;
	e->collapse = collapse;
}

static int begin(zf_em_t *e) {
	if (e->begun || e->err) return e->err;
	e->begun = true;
	int r = e->d->begin(e->d, e->title[0] ? e->title : NULL);
	if (r < 0) e->err = r;
	return e->err;
}

static int flush(zf_em_t *e) {
	if (!e->n || e->err) { e->n = 0; return e->err; }
	int r = e->d->text(e->d, e->buf, e->n, e->style);
	e->n = 0;
	if (r < 0) e->err = r;
	return e->err;
}

static int open_now(zf_em_t *e) {
	if (e->open || e->err) return e->err;
	if (begin(e)) return e->err;
	int r = e->d->para(e->d, e->kind, e->level);
	if (r < 0) return e->err = r;
	e->open = true;
	e->pending = false;
	return 0;
}

bool zf_em_in_para(const zf_em_t *e) { return e->open || e->pending; }

int zf_em_end_para(zf_em_t *e) {
	if (e->open) {
		flush(e);
		if (!e->err) {
			int r = e->d->end_para(e->d);
			if (r < 0) e->err = r;
		}
	}
	e->open = e->pending = false;
	e->space = e->any = false;
	return e->err;
}

int zf_em_para(zf_em_t *e, int kind, int level, bool eager) {
	zf_em_end_para(e);
	e->kind = kind;
	e->level = level;
	e->pending = true;
	if (eager) open_now(e);
	return e->err;
}

void zf_em_kind(zf_em_t *e, int kind, int level) {
	if (e->open && e->any) return;
	if (e->open) {
		// opened eagerly, nothing in it: reopen as the new kind
		// costs an empty paragraph, so only the pending case changes
		return;
	}
	e->kind = kind;
	e->level = level;
}

static int add(zf_em_t *e, const char *s, uint32_t n, int style) {
	if (e->err) return e->err;
	if (!e->open) {
		if (!e->pending) { e->kind = ZF_P_BODY; e->level = 1; }
		if (open_now(e)) return e->err;
	}
	// a heading is bold already; a reader that says so again (a style
	// with bold in it) would put markup round every heading
	if (e->kind == ZF_P_HEAD) style &= ~ZF_S_BOLD;
	if (e->kind == ZF_P_PRE) style = 0;
	if (style != e->style) { flush(e); e->style = style; }
	for (uint32_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (e->n >= ZF_EM_BUF - 8 && (c & 0xC0) != 0x80) { if (flush(e)) return e->err; }
		if (e->n >= ZF_EM_BUF) { if (flush(e)) return e->err; }
		e->buf[e->n++] = (char)c;
	}
	e->any = true;
	return 0;
}

static int owed_space(zf_em_t *e, int style) {
	// the space between two runs belongs to what they have in common:
	// "plain **bold**", not "plain** bold**"
	if (e->space && e->any) { e->space = false; return add(e, " ", 1, style & e->style); }
	e->space = false;
	return 0;
}

int zf_em_raw(zf_em_t *e, const char *s, uint32_t n, int style) {
	if (!n) return e->err;
	owed_space(e, style);
	return add(e, s, n, style);
}

int zf_em_text(zf_em_t *e, const char *s, uint32_t n, int style) {
	if (!e->collapse) return n ? add(e, s, n, style) : e->err;
	uint32_t a = 0;
	for (uint32_t i = 0; i <= n; i++) {
		bool ws = i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f');
		if (i < n && !ws) continue;
		if (i > a) {
			owed_space(e, style);
			add(e, s + a, i - a, style);
		}
		if (ws) e->space = true;
		a = i + 1;
	}
	return e->err;
}

int zf_em_bytes(zf_em_t *e, const uint8_t *s, uint32_t n, int style) {
	char out[8];
	uint32_t a = 0;		// start of a pending ASCII/valid run
	for (uint32_t i = 0; i < n; i++) {
		uint8_t c = s[i];
		if (!e->u8need) {
			if (c < 0x80) continue;
			if (i > a) zf_em_text(e, (const char *)s + a, i - a, style);
			int need = (c >= 0xC2 && c <= 0xDF) ? 1 : (c >= 0xE0 && c <= 0xEF) ? 2 : (c >= 0xF0 && c <= 0xF4) ? 3 : 0;
			if (!need) {
				int k = zf_utf8_put(zf_cp1252(c), out);
				zf_em_raw(e, out, (uint32_t)k, style);
			} else {
				e->u8[0] = c;
				e->u8n = 1;
				e->u8need = need;
			}
			a = i + 1;
			continue;
		}
		if ((c & 0xC0) == 0x80) {
			e->u8[e->u8n++] = c;
			if (--e->u8need == 0) { zf_em_raw(e, (const char *)e->u8, (uint32_t)e->u8n, style); e->u8n = 0; }
			a = i + 1;
			continue;
		}
		// broken sequence: what was held, as Windows-1252
		for (int k = 0; k < e->u8n; k++) {
			int m = zf_utf8_put(zf_cp1252(e->u8[k]), out);
			zf_em_raw(e, out, (uint32_t)m, style);
		}
		e->u8n = e->u8need = 0;
		a = i;
		i--;		// look at this byte again, fresh
	}
	if (n > a && !e->u8need) zf_em_text(e, (const char *)s + a, n - a, style);
	return e->err;
}

int zf_em_brk(zf_em_t *e) {
	if (e->err) return e->err;
	// <p><br></p>: a paragraph that starts with a break is one blank line
	if (e->collapse && !e->open) { zf_em_para(e, e->pending ? e->kind : ZF_P_BODY, e->pending ? e->level : 1, true); return e->err; }
	if (!e->open && open_now(e)) return e->err;
	flush(e);
	e->space = false;
	if (!e->err) { int r = e->d->brk(e->d); if (r < 0) e->err = r; }
	e->any = false;		// no space owed at the start of the new line
	return e->err;
}

int zf_em_end(zf_em_t *e) {
	zf_em_end_para(e);
	begin(e);
	if (!e->err) { int r = e->d->end(e->d); if (r < 0) e->err = r; }
	return e->err;
}
