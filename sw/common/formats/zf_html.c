/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Reading HTML (and the XHTML inside an EPUB) as a document.
 *
 * Not a browser: sw/apps/web is that. This reads a page for its text
 * and the structure a document keeps -- h1-h6, p, li (nested ul/ol),
 * pre, br, b/strong, i/em -- forgivingly: unclosed paragraphs end at
 * the next block, unknown tags are passed over, whitespace collapses
 * as a browser collapses it. Left out: script, style, head (except
 * the title), template, svg, math, nav, noscript, select, and the
 * text of form controls. A table's cells are separated by tabs, a
 * row per paragraph. Bytes that are not UTF-8 are read as
 * Windows-1252, which covers what the meta charset of an old page
 * usually says.
 */

#include <string.h>

#include "zf.h"
#include "zf_em.h"

typedef struct {
	zf_rd_t		rd;
	zf_em_t		em;
	int			bold, ital;		// nesting counts
	int			lists;
	int			pre;
	int			skip;			// inside script/style/...: the tag that ends it
	char		skip_tag[16];
	bool		in_title;
	int			tlen;
	bool		in_body;
	bool		body_only;
	bool		row_cell;		// a cell already in this table row
	bool		pre_nl;			// a newline straight after <pre> is not content
	uint32_t	ticks;
} html_t;

static html_t H;

static int style(void) { return (H.bold ? ZF_S_BOLD : 0) | (H.ital ? ZF_S_ITAL : 0); }

static void block(int kind, int level) {
	zf_em_para(&H.em, kind, level, false);
	H.em.collapse = (kind != ZF_P_PRE);
}

static void end_block(void) {
	zf_em_end_para(&H.em);
}

static bool is_heading(const char *t) {
	return (t[0] == 'h') && t[1] >= '1' && t[1] <= '6' && !t[2];
}

static bool in_set(const char *t, const char *const *set) {
	for (int i = 0; set[i]; i++) if (!strcmp(t, set[i])) return true;
	return false;
}

static const char *const blocks[] = {
	"p", "div", "section", "article", "header", "footer", "main", "aside", "blockquote",
	"address", "figure", "figcaption", "dl", "dt", "dd", "hr", "center", "form", "fieldset",
	"details", "summary", "caption", "body", "html", NULL };

static const char *const skips[] = {
	"script", "style", "head", "template", "svg", "math", "nav", "noscript", "select",
	"textarea", "button", "object", "iframe", "video", "audio", "canvas", NULL };

static void text_bytes(const uint8_t *s, uint32_t n) {
	if (H.in_title) {
		for (uint32_t i = 0; i < n && H.tlen < (int)sizeof(H.em.title) - 1; i++)
			H.em.title[H.tlen++] = (char)(s[i] == '\n' || s[i] == '\t' ? ' ' : s[i]);
		H.em.title[H.tlen] = 0;
		return;
	}
	if (H.skip) return;
	if (H.body_only && !H.in_body) return;
	if (H.pre_nl) {
		H.pre_nl = false;
		if (n && s[0] == '\n') { s++; n--; }
		if (!n) return;
	}
	zf_em_bytes(&H.em, s, n, style());
}

static void tag(const char *t, bool close, bool self) {
	if (H.skip) {
		if (close && !strcmp(t, H.skip_tag)) H.skip = 0;
		// the title is the one thing kept from <head>
		if (!strcmp(H.skip_tag, "head") && !strcmp(t, "title") && !H.body_only) H.in_title = !close;
		return;
	}
	if (!strcmp(t, "title")) { if (!H.body_only) H.in_title = !close; return; }
	if (!close && !self && in_set(t, skips)) {
		H.skip = 1;
		strcpy(H.skip_tag, t);
		return;
	}
	if (!strcmp(t, "body")) { H.in_body = !close; if (close) end_block(); return; }

	if (!strcmp(t, "b") || !strcmp(t, "strong")) { H.bold += close ? (H.bold ? -1 : 0) : 1; return; }
	if (!strcmp(t, "i") || !strcmp(t, "em") || !strcmp(t, "cite") || !strcmp(t, "var") || !strcmp(t, "dfn"))
		{ H.ital += close ? (H.ital ? -1 : 0) : 1; return; }
	if (!strcmp(t, "br")) { zf_em_brk(&H.em); return; }
	if (!strcmp(t, "img")) return;

	if (is_heading(t)) {
		if (close) end_block();
		else block(ZF_P_HEAD, t[1] - '0');
		return;
	}
	if (!strcmp(t, "ul") || !strcmp(t, "ol") || !strcmp(t, "menu")) {
		end_block();
		if (close) { if (H.lists) H.lists--; }
		else if (!self) H.lists++;
		return;
	}
	if (!strcmp(t, "li")) {
		if (close) end_block();
		else block(ZF_P_ITEM, H.lists ? H.lists : 1);
		return;
	}
	if (!strcmp(t, "pre") || !strcmp(t, "listing") || !strcmp(t, "xmp")) {
		end_block();
		if (close) { if (H.pre) H.pre--; H.em.collapse = true; }
		else { H.pre++; block(ZF_P_PRE, 1); H.pre_nl = true; }
		return;
	}
	if (!strcmp(t, "tr")) { end_block(); H.row_cell = false; return; }
	if (!strcmp(t, "td") || !strcmp(t, "th")) {
		if (!close) {
			if (H.row_cell) { H.em.space = false; zf_em_raw(&H.em, "\t", 1, style()); }
			H.row_cell = true;
		}
		return;
	}
	if (!strcmp(t, "table") || !strcmp(t, "tbody") || !strcmp(t, "thead")) { end_block(); return; }
	if (in_set(t, blocks)) {
		// <li><p>text</p></li>: the paragraph is the item's
		if (H.lists && zf_em_in_para(&H.em) && !H.em.any && H.em.kind == ZF_P_ITEM) return;
		if (close && H.lists && !strcmp(t, "p")) { end_block(); return; }
		end_block();
		if (!close && !self && !H.pre) block(ZF_P_BODY, 1);
		return;
	}
}

// After '<': a tag, a comment, a doctype or a stray '<'.
static void markup(void) {
	zf_rd_t *rd = &H.rd;
	int c = zf_rd_peek(rd);
	if (c == '!') {
		zf_rd_getc(rd);
		if (zf_rd_peek(rd) == '-') {
			zf_rd_getc(rd);
			if (zf_rd_peek(rd) == '-') {
				zf_rd_getc(rd);
				int m = 0;
				while ((c = zf_rd_getc(rd)) >= 0) {
					if (c == '-') m++;
					else if (c == '>' && m >= 2) return;
					else m = 0;
				}
				return;
			}
		}
		if (zf_rd_peek(rd) == '[') {
			// <![CDATA[ ... ]]> in XHTML: its text, as text
			char k[8];
			int n = 0;
			while (n < 7 && (c = zf_rd_getc(rd)) >= 0) k[n++] = (char)c;
			k[n] = 0;
			if (!strcmp(k, "[CDATA[")) {
				int m = 0;
				while ((c = zf_rd_getc(rd)) >= 0) {
					if (c == ']') { m++; continue; }
					if (c == '>' && m >= 2) return;
					while (m) { text_bytes((const uint8_t *)"]", 1); m--; }
					uint8_t b = (uint8_t)c;
					text_bytes(&b, 1);
				}
			}
			return;
		}
		while ((c = zf_rd_getc(rd)) >= 0 && c != '>') ;
		return;
	}
	if (c == '?') { while ((c = zf_rd_getc(rd)) >= 0 && c != '>') ; return; }
	bool close = false;
	if (c == '/') { close = true; zf_rd_getc(rd); c = zf_rd_peek(rd); }
	if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
		// not a tag: the '<' was text
		text_bytes((const uint8_t *)(close ? "</" : "<"), close ? 2 : 1);
		return;
	}
	char t[16];
	int n = 0;
	while ((c = zf_rd_peek(rd)) >= 0 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || c == ':' || c == '-')) {
		zf_rd_getc(rd);
		if (c >= 'A' && c <= 'Z') c += 32;
		if (n < 15) t[n++] = (char)c;
	}
	t[n] = 0;
	// a namespace prefix ("xhtml:p") is dropped
	char *colon = strchr(t, ':');
	const char *name = colon ? colon + 1 : t;
	// attributes, skipped -- minding quotes, which may hold '>'
	int q = 0;
	bool self = false;
	int prev = 0;
	while ((c = zf_rd_getc(rd)) >= 0) {
		if (q) { if (c == q) q = 0; continue; }
		if (c == '"' || c == '\'') { q = c; continue; }
		if (c == '>') { self = (prev == '/'); break; }
		prev = c;
	}
	tag(name, close, self);
}

static void entity(void) {
	char e[16];
	int n = 0;
	for (;;) {
		int c = zf_rd_peek(&H.rd);
		if (c == ';') { zf_rd_getc(&H.rd); break; }
		if (c < 0 || n >= 15 || !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '#')) {
			// not terminated: "&amp" without ';' is still common
			e[n] = 0;
			uint32_t cp = n ? zf_entity(e) : 0;
			if (cp && n >= 2 && e[0] != '#') {
				char b[4];
				int k = zf_utf8_put(cp, b);
				text_bytes((const uint8_t *)b, (uint32_t)k);
				return;
			}
			text_bytes((const uint8_t *)"&", 1);
			text_bytes((const uint8_t *)e, (uint32_t)n);
			return;
		}
		e[n++] = (char)zf_rd_getc(&H.rd);
	}
	e[n] = 0;
	uint32_t cp = zf_entity(e);
	if (!cp) {
		text_bytes((const uint8_t *)"&", 1);
		text_bytes((const uint8_t *)e, (uint32_t)n);
		text_bytes((const uint8_t *)";", 1);
		return;
	}
	char b[4];
	int k = zf_utf8_put(cp, b);
	if (cp == 0xA0) { zf_em_raw(&H.em, " ", 1, style()); return; }
	text_bytes((const uint8_t *)b, (uint32_t)k);
}

int zf_html_src(zf_src_t *src, zf_doc_t *d, bool body_only) {
	// an epub's chapters each run through here into one document
	bool keep = body_only && H.em.d == d;
	if (!keep) {
		memset(&H, 0, sizeof H);
		zf_em_init(&H.em, d, true);
	} else {
		H.bold = H.ital = H.lists = H.pre = H.skip = 0;
		H.in_body = false;
		H.in_title = false;
	}
	H.body_only = body_only;
	zf_rd_init(&H.rd, src);
	uint8_t run[256];
	uint32_t rn = 0;
	for (;;) {
		if (!(++H.ticks & 1023) && zf_cancel_poll && zf_cancel_poll()) return ZF_E_CANCEL;
		int c = zf_rd_getc(&H.rd);
		if (c < 0 || c == '<' || c == '&' || rn == sizeof run || (c == '\r')) {
			if (rn) { text_bytes(run, rn); rn = 0; }
		}
		if (c < 0) break;
		if (c == '<') markup();
		else if (c == '&') entity();
		else if (c == '\r') { if (zf_rd_peek(&H.rd) != '\n') { uint8_t nl = '\n'; text_bytes(&nl, 1); } }
		else run[rn++] = (uint8_t)c;
		if (H.em.err) return H.em.err;
	}
	if (H.rd.err) return H.rd.err;
	end_block();
	if (body_only) return H.em.err;
	return zf_em_end(&H.em);
}

// Finishes a document built from body_only calls.
int zf_html_finish(zf_doc_t *d) {
	if (H.em.d != d) return ZF_OK;
	return zf_em_end(&H.em);
}

void zf_html_title(const char *t) {
	int i = 0;
	for (; t[i] && i < (int)sizeof(H.em.title) - 1; i++) H.em.title[i] = t[i];
	H.em.title[i] = 0;
}

void zf_html_reset(zf_doc_t *d) {
	memset(&H, 0, sizeof H);
	zf_em_init(&H.em, d, true);
}

int zf_read_html(zf_io_t *in, zf_doc_t *d) {
	static zf_src_t src;
	zf_src_io(&src, in);
	H.em.d = NULL;
	return zf_html_src(&src, d, false);
}
