/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See html.h.
 *
 * -- structure --
 *
 * Three layers, in order down the file:
 *
 *   1. The tag table and the element stack. Which names are known,
 *      what kind of thing each is, and what nesting is currently open.
 *   2. The tokenizer. A flat byte state machine -- no lookahead, no
 *      pushback -- because a resumable parser cannot have either.
 *   3. Block assembly. Turning a stream of characters and tags into
 *      html_line_t values.
 *
 * The tokenizer is the part that must stay a pure function of
 * (state, byte). Every time something in this file wanted "just one
 * character of lookahead" the answer was another state, and that is
 * not a stylistic preference: html_restore() works only because
 * feeding the same bytes from the same state always lands in the same
 * place.
 */

#include <string.h>
#include <stdio.h>

#include "html.h"
#include "uni.h"

// -- 1. tags ------------------------------------------------------

enum {
	T_UNKNOWN = 0,
	T_A, T_ABBR, T_ADDRESS, T_ARTICLE, T_ASIDE, T_AUDIO,
	T_B, T_BASE, T_BLOCKQUOTE, T_BODY, T_BR, T_BUTTON,
	T_CANVAS, T_CAPTION, T_CENTER, T_CITE, T_CODE, T_COL, T_COLGROUP,
	T_DD, T_DEL, T_DIV, T_DL, T_DT,
	T_EM, T_FIGCAPTION, T_FIGURE, T_FOOTER, T_FORM,
	T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_HEAD, T_HEADER, T_HR, T_HTML,
	T_I, T_IFRAME, T_IMG, T_INPUT, T_INS,
	T_KBD, T_LABEL, T_LI, T_LINK, T_MAIN, T_MARK, T_META,
	T_NAV, T_NOSCRIPT, T_OBJECT, T_OL, T_OPTION,
	T_P, T_PICTURE, T_PRE, T_Q,
	T_S, T_SAMP, T_SCRIPT, T_SECTION, T_SELECT, T_SMALL, T_SOURCE,
	T_SPAN, T_STRONG, T_STYLE, T_SUB, T_SUP, T_SVG,
	T_TABLE, T_TBODY, T_TD, T_TEMPLATE, T_TEXTAREA, T_TFOOT, T_TH,
	T_THEAD, T_TIME, T_TITLE, T_TR, T_TT,
	T_U, T_UL, T_VAR, T_VIDEO, T_WBR,
	T_COUNT
};

#define TF_BLOCK  0x01		// starts and ends a block
#define TF_VOID   0x02		// never has a close tag
#define TF_DROP   0x04		// its content is not text
#define TF_RAW    0x08		// its content is not markup either

typedef struct { const char *name; uint8_t id; uint8_t flags; } tag_ent_t;

// Sorted by nothing in particular -- lookup is a linear scan with a
// first-character check, which on this table measures faster than a
// binary search because the vast majority of misses are rejected by
// the first byte alone.
static const tag_ent_t tags[] = {
	{ "a",           T_A,          0 },
	{ "abbr",        T_ABBR,       0 },
	{ "address",     T_ADDRESS,    TF_BLOCK },
	{ "article",     T_ARTICLE,    TF_BLOCK },
	{ "aside",       T_ASIDE,      TF_BLOCK },
	{ "audio",       T_AUDIO,      TF_BLOCK | TF_DROP },
	{ "b",           T_B,          0 },
	{ "base",        T_BASE,       TF_VOID },
	{ "blockquote",  T_BLOCKQUOTE, TF_BLOCK },
	{ "body",        T_BODY,       TF_BLOCK },
	{ "br",          T_BR,         TF_VOID },
	{ "button",      T_BUTTON,     0 },
	{ "canvas",      T_CANVAS,     TF_BLOCK | TF_DROP },
	{ "caption",     T_CAPTION,    TF_BLOCK },
	{ "center",      T_CENTER,     TF_BLOCK },
	{ "cite",        T_CITE,       0 },
	{ "code",        T_CODE,       0 },
	{ "col",         T_COL,        TF_VOID },
	{ "colgroup",    T_COLGROUP,   TF_BLOCK },
	{ "dd",          T_DD,         TF_BLOCK },
	{ "del",         T_DEL,        0 },
	{ "div",         T_DIV,        TF_BLOCK },
	{ "dl",          T_DL,         TF_BLOCK },
	{ "dt",          T_DT,         TF_BLOCK },
	{ "em",          T_EM,         0 },
	{ "figcaption",  T_FIGCAPTION, TF_BLOCK },
	{ "figure",      T_FIGURE,     TF_BLOCK },
	{ "footer",      T_FOOTER,     TF_BLOCK },
	{ "form",        T_FORM,       TF_BLOCK },
	{ "h1",          T_H1,         TF_BLOCK },
	{ "h2",          T_H2,         TF_BLOCK },
	{ "h3",          T_H3,         TF_BLOCK },
	{ "h4",          T_H4,         TF_BLOCK },
	{ "h5",          T_H5,         TF_BLOCK },
	{ "h6",          T_H6,         TF_BLOCK },
	{ "head",        T_HEAD,       TF_BLOCK | TF_DROP },
	{ "header",      T_HEADER,     TF_BLOCK },
	{ "hr",          T_HR,         TF_VOID },
	{ "html",        T_HTML,       TF_BLOCK },
	{ "i",           T_I,          0 },
	{ "iframe",      T_IFRAME,     TF_BLOCK | TF_DROP },
	{ "img",         T_IMG,        TF_VOID },
	{ "input",       T_INPUT,      TF_VOID },
	{ "ins",         T_INS,        0 },
	{ "kbd",         T_KBD,        0 },
	{ "label",       T_LABEL,      0 },
	{ "li",          T_LI,         TF_BLOCK },
	{ "link",        T_LINK,       TF_VOID },
	{ "main",        T_MAIN,       TF_BLOCK },
	{ "mark",        T_MARK,       0 },
	{ "meta",        T_META,       TF_VOID },
	{ "nav",         T_NAV,        TF_BLOCK },
	{ "noscript",    T_NOSCRIPT,   TF_BLOCK },
	{ "object",      T_OBJECT,     TF_BLOCK | TF_DROP },
	{ "ol",          T_OL,         TF_BLOCK },
	{ "option",      T_OPTION,     TF_BLOCK | TF_DROP },
	{ "p",           T_P,          TF_BLOCK },
	{ "picture",     T_PICTURE,    0 },
	{ "pre",         T_PRE,        TF_BLOCK },
	{ "q",           T_Q,          0 },
	{ "s",           T_S,          0 },
	{ "samp",        T_SAMP,       0 },
	{ "script",      T_SCRIPT,     TF_BLOCK | TF_DROP | TF_RAW },
	{ "section",     T_SECTION,    TF_BLOCK },
	{ "select",      T_SELECT,     TF_BLOCK | TF_DROP },
	{ "small",       T_SMALL,      0 },
	{ "source",      T_SOURCE,     TF_VOID },
	{ "span",        T_SPAN,       0 },
	{ "strong",      T_STRONG,     0 },
	{ "style",       T_STYLE,      TF_BLOCK | TF_DROP | TF_RAW },
	{ "sub",         T_SUB,        0 },
	{ "sup",         T_SUP,        0 },
	{ "svg",         T_SVG,        TF_BLOCK | TF_DROP },
	{ "table",       T_TABLE,      TF_BLOCK },
	{ "tbody",       T_TBODY,      TF_BLOCK },
	{ "td",          T_TD,         0 },
	{ "template",    T_TEMPLATE,   TF_BLOCK | TF_DROP },
	{ "textarea",    T_TEXTAREA,   TF_BLOCK | TF_DROP | TF_RAW },
	{ "tfoot",       T_TFOOT,      TF_BLOCK },
	{ "th",          T_TH,         0 },
	{ "thead",       T_THEAD,      TF_BLOCK },
	{ "time",        T_TIME,       0 },
	{ "title",       T_TITLE,      TF_BLOCK },
	{ "tr",          T_TR,         TF_BLOCK },
	{ "tt",          T_TT,         0 },
	{ "u",           T_U,          0 },
	{ "ul",          T_UL,         TF_BLOCK },
	{ "var",         T_VAR,        0 },
	{ "video",       T_VIDEO,      TF_BLOCK | TF_DROP },
	{ "wbr",         T_WBR,        TF_VOID },
};

#define NTAGS ((int)(sizeof(tags) / sizeof(tags[0])))

static const tag_ent_t *tag_lookup(const char *name) {
	for (int i = 0; i < NTAGS; i++)
		if (tags[i].name[0] == name[0] && !strcmp(tags[i].name, name))
			return &tags[i];
	return NULL;
}

static uint8_t tag_flags(uint8_t id) {
	for (int i = 0; i < NTAGS; i++) if (tags[i].id == id) return tags[i].flags;
	return 0;
}

// -- 2. small helpers ---------------------------------------------

static char lc(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool ws(char c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

// Windows-1252's 0x80..0x9F, which is the only place it differs from
// Latin-1. See html.h's HTML_CS_CP1252 on why the two are one thing
// here.
static const uint16_t cp1252_high[32] = {
	0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
	0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
	0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
	0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

// -- named entities --
//
// The set that actually appears, not the set that exists. HTML5 names
// 2231 entities; a table of those is 30KB or so and the tail of it is
// mathematical symbols this display cannot draw anyway. These are the
// ones that turn up in ordinary prose, plus the five that are
// structural.
static const struct { const char *name; uint16_t cp; } entities[] = {
	{ "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
	{ "apos", '\'' }, { "nbsp", 0x00A0 }, { "shy", 0x00AD },
	{ "copy", 0x00A9 }, { "reg", 0x00AE }, { "trade", 0x2122 },
	{ "deg", 0x00B0 }, { "plusmn", 0x00B1 }, { "sup2", '2' },
	{ "sup3", '3' }, { "micro", 0x00B5 }, { "middot", 0x00B7 },
	{ "frac12", 0x00BD }, { "frac14", 0x00BC }, { "frac34", 0x00BE },
	{ "times", 0x00D7 }, { "divide", 0x00F7 },
	{ "ndash", 0x2013 }, { "mdash", 0x2014 },
	{ "lsquo", 0x2018 }, { "rsquo", 0x2019 },
	{ "ldquo", 0x201C }, { "rdquo", 0x201D },
	{ "sbquo", 0x201A }, { "bdquo", 0x201E },
	{ "dagger", 0x2020 }, { "Dagger", 0x2021 },
	{ "bull", 0x2022 }, { "hellip", 0x2026 }, { "prime", 0x2032 },
	{ "permil", 0x2030 }, { "lsaquo", 0x2039 }, { "rsaquo", 0x203A },
	{ "laquo", 0x00AB }, { "raquo", 0x00BB },
	{ "euro", 0x20AC }, { "pound", 0x00A3 }, { "yen", 0x00A5 },
	{ "cent", 0x00A2 }, { "sect", 0x00A7 }, { "para", 0x00B6 },
	{ "larr", 0x2190 }, { "rarr", 0x2192 }, { "harr", 0x2194 },
	{ "minus", 0x2212 }, { "ensp", 0x2002 }, { "emsp", 0x2003 },
	{ "thinsp", 0x2009 }, { "zwnj", 0x200C }, { "zwj", 0x200D },
	{ "aacute", 0x00E1 }, { "eacute", 0x00E9 }, { "iacute", 0x00ED },
	{ "oacute", 0x00F3 }, { "uacute", 0x00FA }, { "ntilde", 0x00F1 },
	{ "agrave", 0x00E0 }, { "egrave", 0x00E8 }, { "ugrave", 0x00F9 },
	{ "auml", 0x00E4 }, { "ouml", 0x00F6 }, { "uuml", 0x00FC },
	{ "Auml", 0x00C4 }, { "Ouml", 0x00D6 }, { "Uuml", 0x00DC },
	{ "szlig", 0x00DF }, { "ccedil", 0x00E7 }, { "aring", 0x00E5 },
	{ "oslash", 0x00F8 }, { "aelig", 0x00E6 }, { "eth", 0x00F0 },
	{ "thorn", 0x00FE }, { "alpha", 'a' }, { "beta", 'b' },
	{ "pi", 0x03C0 }, { "mu", 0x00B5 }, { "infin", 0x221E },
};

#define NENTS ((int)(sizeof(entities) / sizeof(entities[0])))

// Returns the codepoint, or 0 if the name is not one we know.
//
// A numeric reference in the 0x80..0x9F range is remapped through
// cp1252_high, which is what the HTML specification itself requires:
// `&#151;` is overwhelmingly a page that meant an em dash, and
// decoding it as the C1 control it literally names produces nothing
// on screen.
static uint32_t entity_lookup(const char *name) {

	if (name[0] == '#') {

		uint32_t v = 0;
		const char *p = name + 1;

		if (*p == 'x' || *p == 'X') {
			for (p++; *p; p++) {
				char c = lc(*p);
				if (c >= '0' && c <= '9') v = v * 16 + (uint32_t)(c - '0');
				else if (c >= 'a' && c <= 'f') v = v * 16 + (uint32_t)(c - 'a' + 10);
				else return 0;
				if (v > 0x10FFFF) return 0;
			}
		} else {
			if (!*p) return 0;
			for (; *p; p++) {
				if (*p < '0' || *p > '9') return 0;
				v = v * 10 + (uint32_t)(*p - '0');
				if (v > 0x10FFFF) return 0;
			}
		}

		if (v >= 0x80 && v <= 0x9F) return cp1252_high[v - 0x80];
		return v;

	}

	for (int i = 0; i < NENTS; i++)
		if (entities[i].name[0] == name[0] && !strcmp(entities[i].name, name))
			return entities[i].cp;

	return 0;

}

// Rewrites `s` in place with entities decoded and everything folded to
// ASCII (uni.h). Used on attribute VALUES only -- href query strings
// are full of `&amp;`, and a URL with a literal "&amp;" in it fetches
// the wrong thing.
//
// Text content does not come through here: it is decoded character by
// character on the way into a block, because that path also has to
// collapse whitespace and track span boundaries.
static void decode_value(char *s) {

	char *r = s, *w = s;

	while (*r) {

		if (*r == '&') {

			char name[16];
			uint32_t n = 0;
			char *q = r + 1;

			while (*q && *q != ';' && n < sizeof(name) - 1 &&
				!ws(*q) && *q != '&') name[n++] = *q++;
			name[n] = '\0';

			if (*q == ';' && n) {
				uint32_t cp = entity_lookup(name);
				if (cp) {
					char fold[UNI_FOLD_MAX];
					uint32_t fn = uni_fold(cp, fold);
					for (uint32_t i = 0; i < fn; i++) *w++ = fold[i];
					r = q + 1;
					continue;
				}
			}

		}

		*w++ = *r++;

	}

	*w = '\0';

}

// -- 3. block assembly --------------------------------------------

static void put_char(html_ctx_t *c, char ch);

static bool stack_has(const html_ctx_t *c, uint8_t id) {
	for (int i = 0; i < c->st.depth; i++) if (c->st.stack[i] == id) return true;
	return false;
}

static void span_close(html_ctx_t *c) {

	if (!c->span_kind) return;

	if (c->line.len > c->span_start && c->line.nspans < HTML_MAX_SPANS) {
		html_span_t *s = &c->line.spans[c->line.nspans++];
		s->start = c->span_start;
		s->len = (uint16_t)(c->line.len - c->span_start);
		s->kind = c->span_kind;
		s->link = (c->cur_link >= 0) ? (uint8_t)c->cur_link : 0;
	}

	c->span_kind = 0;

}

static void span_open(html_ctx_t *c, uint8_t kind) {
	if (c->span_kind == kind) return;
	span_close(c);
	c->span_kind = kind;
	c->span_start = c->line.len;
}

// What kind of block is the text arriving right now part of?
//
// Derived from the element stack rather than remembered, so that a
// block started implicitly (text directly inside a <div>, which is
// extremely common) gets the same answer as one started by a tag.
static void classify(html_ctx_t *c) {

	if (c->cur_kind) return;

	if (c->st.pre_depth) { c->cur_kind = HTML_PRE; return; }

	if (stack_has(c, T_TR)) { c->cur_kind = HTML_TABLE; return; }

	if (c->st.quote_depth) {
		c->cur_kind = HTML_QUOTE;
		c->cur_level = c->st.quote_depth;
		return;
	}

	if (stack_has(c, T_LI) || stack_has(c, T_DD)) {
		c->cur_kind = HTML_LIST;
		return;
	}

	c->cur_kind = HTML_PARA;

}

static void flush_block(html_ctx_t *c) {

	bool emitted;

	span_close(c);

	emitted = c->have_text;

	if (c->have_text) {
		classify(c);
		c->line.kind = (html_kind_t)c->cur_kind;
		c->line.level = c->cur_level;

		// An image keeps its own kind. classify() reports the
		// enclosing element, which for an <img> inside a <p> is the
		// paragraph -- correct for text, wrong for this.
		if (c->img_pending) c->line.kind = HTML_IMAGE;
		c->line.tight = c->line_tight;
		c->line.text[c->line.len] = '\0';
		if (c->emit) c->emit(c->user, &c->line);
	}

	c->line.len = 0;
	c->line.text[0] = '\0';
	c->line.nspans = 0;
	c->line.nlinks = 0;
	c->line.truncated = false;
	c->line.img_w = 0;
	c->line.img_h = 0;
	c->img_pending = false;
	c->have_text = false;
	c->pending_space = false;
	c->cur_link = -1;
	c->span_kind = 0;
	c->span_start = 0;

	// The block kind, its level and its list marker survive a flush
	// that emitted NOTHING.
	//
	// `<li><div>text</div></li>` is the case, and it is not exotic --
	// it is how most navigation menus on the web are built. The <div>
	// flushes before any text has arrived, and clearing the marker
	// there leaves the list item that follows with no bullet. The same
	// thing happens to a heading whose text is wrapped in a block
	// element: the level set by <h1> is gone before the text shows up.
	//
	// Cleared only on a flush that really produced a block, which is
	// what stops a second <div> inside the same <li> from being given
	// the bullet as well.
	if (emitted) {
		c->line.marker[0] = '\0';
		c->line.level = 0;
		c->cur_kind = 0;
		c->cur_level = 0;
		c->line_tight = false;
	}

	// An anchor that spans a block boundary reopens in the next block
	// -- <a><div>x</div></a> is invalid and appears anyway. The href
	// is still in cur_href, so the link survives; only the span had to
	// be closed.
	if (c->cur_href[0] && c->anchor_depth) {
		if (c->line.nlinks < HTML_MAX_LINKS) {
			strncpy(c->line.links[c->line.nlinks], c->cur_href,
				HTML_LINK_MAX - 1);
			c->line.links[c->line.nlinks][HTML_LINK_MAX - 1] = '\0';
			c->cur_link = (int8_t)c->line.nlinks++;
		}
	}

}

// Emits a standalone block that has no text of its own.
static void emit_marker_block(html_ctx_t *c, html_kind_t kind) {

	flush_block(c);

	memset(&c->line, 0, sizeof(c->line));
	c->line.kind = kind;
	if (c->emit) c->emit(c->user, &c->line);

	c->line.len = 0;
	c->line.nlinks = 0;
	c->cur_link = -1;

}

static void put_char(html_ctx_t *c, char ch) {

	if (c->line.len + 1 >= HTML_LINE_MAX) {
		c->line.truncated = true;
		return;
	}

	c->line.text[c->line.len++] = ch;
	c->line.text[c->line.len] = '\0';

}

static void put_text(html_ctx_t *c, const char *s) {
	while (*s) put_char(c, *s++);
}

// One decoded codepoint of document text.
//
// This is where whitespace collapsing lives, and the rule is the one
// every browser uses: a run of whitespace between two pieces of text
// becomes exactly one space, and whitespace at the start or end of a
// block disappears entirely. Doing it here rather than in the layout
// engine matters because a block's text is what gets INDEXED and
// searched, and leading whitespace that only the renderer knows about
// would make every offset in it wrong.
static void put_cp(html_ctx_t *c, uint32_t cp) {

	char fold[UNI_FOLD_MAX];
	uint32_t n;

	// The title check comes BEFORE the drop check, because <title>
	// lives inside <head> and <head> is TF_DROP -- everything else in
	// there is not text. Checked the other way round, the window title
	// is silently always empty.
	//
	// Guarded on not having seen <body> so that a <title> inside an
	// inline <svg> later in the document cannot overwrite it.
	if ((c->st.flags & HTML_ST_IN_TITLE) &&
		!(c->st.flags & HTML_ST_SEEN_BODY)) {
		if (uni_is_space(cp)) cp = ' ';
		n = uni_fold(cp, fold);
		for (uint32_t i = 0; i < n; i++) {
			// Collapse runs, and refuse a leading space, so the title
			// bar does not start with the indentation of the source.
			if (fold[i] == ' ' &&
				(c->title_len == 0 || c->title[c->title_len - 1] == ' '))
				continue;
			if (c->title_len + 1 < HTML_TITLE_MAX)
				c->title[c->title_len++] = fold[i];
		}
		c->title[c->title_len] = '\0';
		return;
	}

	if (c->st.drop_depth) return;

	if (c->st.pre_depth) {
		// Inside <pre> a newline ends the visual line and nothing is
		// collapsed. Everything else about block assembly is the same,
		// which is why this is a flush rather than a separate path.
		if (cp == '\n') { flush_block(c); c->cur_kind = HTML_PRE; return; }
		if (cp == '\r') return;
		classify(c);
		n = uni_fold(cp, fold);
		for (uint32_t i = 0; i < n; i++) put_char(c, fold[i]);
		if (n) c->have_text = true;
		return;
	}

	if (uni_is_space(cp)) {
		if (c->have_text) c->pending_space = true;
		return;
	}

	classify(c);

	if (c->pending_space) {
		put_char(c, ' ');
		c->pending_space = false;
	}

	n = uni_fold(cp, fold);
	for (uint32_t i = 0; i < n; i++) put_char(c, fold[i]);
	if (n) c->have_text = true;

}

// -- attributes we care about --
//
// Only three, and each for one reason: href makes a link, alt makes an
// image legible, and charset/content on <meta> can change how every
// byte after it is decoded.
// The last path component of a URL, without any query.
//
// Used as the caption when there is no alt text: a filename says more
// about a missing image than the word "image" does, and it is what a
// reader would need in order to go and look at it.
static void basename_of(const char *url, char *out, uint32_t cap) {

	const char *p = url, *last = url;
	uint32_t n = 0;

	for (; *p; p++) {
		if (*p == '/') last = p + 1;
		if (*p == '?' || *p == '#') break;
	}

	while (last < p && n + 1 < cap) out[n++] = *last++;
	out[n] = '\0';

}

// Emits an image as a block of its own.
//
// Its own block, not inline text, because the layout engine draws it
// as a box and a box cannot sit inside a wrapped line. The cost is
// that an image inside a paragraph splits it, which reads acceptably
// and is much simpler than inline boxes would be.
static void emit_image(html_ctx_t *c) {

	flush_block(c);

	// Caption: the alt text if there is one, otherwise the filename.
	if (c->pend_alt_set && c->pend_alt[0]) {
		put_text(c, c->pend_alt);
	} else if (c->pend_src[0]) {
		char name[64];
		basename_of(c->pend_src, name, sizeof(name));
		put_text(c, name[0] ? name : "image");
	} else {
		put_text(c, "image");
	}

	// The src goes in as link 0, so the renderer can register a hit
	// rectangle over the box and the browser can resolve it against
	// the page URL exactly as it does an anchor. An image with no src
	// is still drawn -- it just cannot be loaded.
	if (c->pend_src[0] && c->line.nlinks < HTML_MAX_LINKS) {
		strncpy(c->line.links[0], c->pend_src, HTML_LINK_MAX - 1);
		c->line.links[0][HTML_LINK_MAX - 1] = '\0';
		c->line.nlinks = 1;
	}

	// A flag rather than an assignment: flush_block() sets the kind
	// from the open element stack, so anything written here would be
	// overwritten on the way out.
	c->img_pending = true;
	c->line.img_w = c->pend_w;
	c->line.img_h = c->pend_h;

	c->have_text = true;
	flush_block(c);

}

static void attr_done(html_ctx_t *c) {

	if (!c->attr_len) return;

	decode_value(c->val);

	if (!strcmp(c->attr, "href")) {
		strncpy(c->pend_href, c->val, HTML_LINK_MAX - 1);
		c->pend_href[HTML_LINK_MAX - 1] = '\0';
	} else if (!strcmp(c->attr, "src")) {
		strncpy(c->pend_src, c->val, HTML_LINK_MAX - 1);
		c->pend_src[HTML_LINK_MAX - 1] = '\0';
	} else if (!strcmp(c->attr, "width") || !strcmp(c->attr, "height")) {
		// Digits only. `width="50%"` and `width="auto"` are both
		// legal and neither is a pixel count, so anything that is
		// not a plain number is treated as absent rather than
		// guessed at.
		uint32_t v = 0;
		const char *p = c->val;
		bool ok = (*p != '\0');
		for (; *p; p++) {
			if (*p < '0' || *p > '9') { ok = false; break; }
			v = v * 10 + (uint32_t)(*p - '0');
			if (v > 10000) { ok = false; break; }
		}
		if (ok) {
			if (c->attr[0] == 'w') c->pend_w = (uint16_t)v;
			else c->pend_h = (uint16_t)v;
		}
	} else if (!strcmp(c->attr, "alt")) {
		strncpy(c->pend_alt, c->val, sizeof(c->pend_alt) - 1);
		c->pend_alt[sizeof(c->pend_alt) - 1] = '\0';
		c->pend_alt_set = true;
	} else if (!strcmp(c->attr, "charset")) {
		strncpy(c->pend_charset, c->val, sizeof(c->pend_charset) - 1);
		c->pend_charset[sizeof(c->pend_charset) - 1] = '\0';
	} else if (!strcmp(c->attr, "content")) {
		strncpy(c->pend_content, c->val, sizeof(c->pend_content) - 1);
		c->pend_content[sizeof(c->pend_content) - 1] = '\0';
	} else if (!strcmp(c->attr, "type")) {
		strncpy(c->pend_type, c->val, sizeof(c->pend_type) - 1);
		c->pend_type[sizeof(c->pend_type) - 1] = '\0';
	} else if (!strcmp(c->attr, "value")) {
		strncpy(c->pend_value, c->val, sizeof(c->pend_value) - 1);
		c->pend_value[sizeof(c->pend_value) - 1] = '\0';
	}

	c->attr_len = 0;
	c->attr[0] = '\0';
	c->val_len = 0;
	c->val[0] = '\0';

}

static void list_push(html_ctx_t *c, bool ordered) {
	if (c->st.list_depth < 8) {
		if (ordered) c->st.list_ordered |= (uint8_t)(1u << c->st.list_depth);
		else c->st.list_ordered &= (uint8_t)~(1u << c->st.list_depth);
		c->st.list_index[c->st.list_depth] = 0;
	}
	if (c->st.list_depth < 255) c->st.list_depth++;
}

static void list_pop(html_ctx_t *c) {
	if (c->st.list_depth) c->st.list_depth--;
}

static void li_marker(html_ctx_t *c) {

	uint8_t d = c->st.list_depth ? (uint8_t)(c->st.list_depth - 1) : 0;

	if (d < 8 && (c->st.list_ordered & (1u << d))) {
		c->st.list_index[d]++;
		snprintf(c->line.marker, sizeof(c->line.marker), "%u.",
			(unsigned)c->st.list_index[d]);
	} else {
		// Two markers, alternating by depth, so a nested list reads as
		// nested even though the indent is only two characters wide at
		// this font size.
		c->line.marker[0] = (d & 1) ? '-' : '*';
		c->line.marker[1] = '\0';
	}

	c->cur_level = d;

}

// -- open and close --

static void open_tag(html_ctx_t *c, const tag_ent_t *t, bool self_closing) {

	uint8_t id = t ? t->id : T_UNKNOWN;
	uint8_t fl = t ? t->flags : 0;

	if (fl & TF_BLOCK) flush_block(c);

	switch (id) {

	case T_BR:
		// A line break inside a paragraph, not a new paragraph. The
		// `tight` flag is what tells the layout engine not to add the
		// inter-block gap it would otherwise put here.
		if (c->have_text) {
			flush_block(c);
			c->line_tight = true;
		} else {
			emit_marker_block(c, HTML_BLANK);
		}
		return;

	case T_HR:
		emit_marker_block(c, HTML_RULE);
		return;

	case T_WBR:
		return;

	case T_IMG:
		// An image with an EMPTY alt is decorative and says so.
		// Drawing a box for every spacer GIF and tracking pixel on a
		// page would bury the content it is spacing.
		if (c->pend_alt_set && !c->pend_alt[0]) return;
		emit_image(c);
		return;

	case T_INPUT:
		if (!strcmp(c->pend_type, "hidden")) return;
		if (!strcmp(c->pend_type, "submit") || !strcmp(c->pend_type, "button")) {
			put_cp(c, '[');
			for (const char *p = c->pend_value; *p; p++) put_cp(c, (unsigned char)*p);
			if (!c->pend_value[0]) { put_cp(c, 'o'); put_cp(c, 'k'); }
			put_cp(c, ']');
		} else if (!strcmp(c->pend_type, "checkbox") ||
			!strcmp(c->pend_type, "radio")) {
			put_cp(c, '['); put_cp(c, ' '); put_cp(c, ']');
		} else {
			classify(c);
			if (c->pending_space) { put_char(c, ' '); c->pending_space = false; }
			put_text(c, "[______]");
			c->have_text = true;
		}
		return;

	case T_META:
		// `<meta charset>` and its http-equiv spelling. The transport's
		// Content-Type wins if it said anything (html.h), so this only
		// applies when nothing else did -- and only before any text has
		// been decoded, since re-decoding what already went past is not
		// possible in a streaming parser.
		if (c->charset_locked) return;
		{
			const char *cs = c->pend_charset[0] ? c->pend_charset : NULL;
			if (!cs && c->pend_content[0]) {
				const char *p = strstr(c->pend_content, "charset=");
				if (p) cs = p + 8;
			}
			if (cs) {
				char low[32];
				uint32_t i = 0;
				while (cs[i] && i < sizeof(low) - 1) { low[i] = lc(cs[i]); i++; }
				low[i] = '\0';
				if (strstr(low, "8859-1") || strstr(low, "1252") ||
					strstr(low, "latin1"))
					c->charset = HTML_CS_CP1252;
			}
		}
		return;

	default:
		break;
	}

	if (fl & TF_VOID) return;

	// -- elements that carry state --

	switch (id) {
	case T_UL: list_push(c, false); break;
	case T_OL: list_push(c, true); break;
	case T_LI: li_marker(c); c->cur_kind = HTML_LIST; break;
	case T_DD: c->cur_kind = HTML_LIST; c->cur_level = 1; c->line.marker[0] = '\0'; break;
	case T_BLOCKQUOTE: if (c->st.quote_depth < 255) c->st.quote_depth++; break;
	case T_PRE: if (c->st.pre_depth < 255) c->st.pre_depth++; break;
	case T_TITLE: c->st.flags |= HTML_ST_IN_TITLE; break;
	case T_BODY: c->st.flags |= HTML_ST_SEEN_BODY; break;
	case T_H1: case T_H2: case T_H3:
	case T_H4: case T_H5: case T_H6:
		c->cur_kind = HTML_HEADING;
		c->cur_level = (uint8_t)(id - T_H1 + 1);
		break;
	case T_TD: case T_TH:
		// A cell separator, but only between cells -- a leading " | "
		// on every row wastes three of the ~120 columns available.
		if (c->have_text) { span_close(c); put_text(c, " | "); }
		c->cur_kind = HTML_TABLE;
		break;
	case T_CODE: case T_KBD: case T_SAMP: case T_TT: case T_VAR:
		span_open(c, HTML_SPAN_CODE);
		break;
	case T_A:
		if (c->pend_href[0]) {
			strncpy(c->cur_href, c->pend_href, HTML_LINK_MAX - 1);
			c->cur_href[HTML_LINK_MAX - 1] = '\0';
			if (c->line.nlinks < HTML_MAX_LINKS) {
				strncpy(c->line.links[c->line.nlinks], c->cur_href,
					HTML_LINK_MAX - 1);
				c->line.links[c->line.nlinks][HTML_LINK_MAX - 1] = '\0';
				c->cur_link = (int8_t)c->line.nlinks++;
				span_open(c, HTML_SPAN_LINK);
			}
			if (c->anchor_depth < 255) c->anchor_depth++;
		}
		break;
	default:
		break;
	}

	if (fl & TF_DROP) {
		if (c->st.drop_depth < 255) c->st.drop_depth++;
	}

	if (self_closing) {
		// XHTML-style `<div/>`. Treated as open-then-close so the
		// stack stays balanced; on real pages this appears mostly on
		// <br/> and <img/>, which returned above.
		if (fl & TF_DROP) { if (c->st.drop_depth) c->st.drop_depth--; }
		switch (id) {
		case T_UL: case T_OL: list_pop(c); break;
		case T_BLOCKQUOTE: if (c->st.quote_depth) c->st.quote_depth--; break;
		case T_PRE: if (c->st.pre_depth) c->st.pre_depth--; break;
		case T_TITLE: c->st.flags &= (uint8_t)~HTML_ST_IN_TITLE; break;
		default: break;
		}
		if (fl & TF_BLOCK) flush_block(c);
		return;
	}

	if (c->st.depth < HTML_STACK_MAX) c->st.stack[c->st.depth++] = id;
	else if (c->st.overflow < 0xFFFF) c->st.overflow++;

}

static void close_tag(html_ctx_t *c, const tag_ent_t *t) {

	uint8_t id = t ? t->id : T_UNKNOWN;
	uint8_t fl = t ? t->flags : 0;
	int at = -1;

	if (fl & TF_VOID) return;

	// An overflowed open is consumed by its own close before the stack
	// is touched, which is what keeps the two in step. Without this a
	// page deeper than HTML_STACK_MAX would pop real elements to pay
	// for opens that were never pushed.
	if (c->st.overflow) { c->st.overflow--; return; }

	for (int i = c->st.depth - 1; i >= 0; i--)
		if (c->st.stack[i] == id) { at = i; break; }

	// A close with no matching open is ignored entirely. See html.h on
	// why there is no adoption agency here.
	if (at < 0) return;

	if (fl & TF_BLOCK) flush_block(c);

	// Everything still open inside it is closed too, implicitly. That
	// is what makes `<p>a<p>b` and an unclosed <li> work, and it is
	// most of what the real tree builder's error recovery buys.
	for (int i = c->st.depth - 1; i >= at; i--) {

		uint8_t oid = c->st.stack[i];
		uint8_t ofl = tag_flags(oid);

		if (ofl & TF_DROP) { if (c->st.drop_depth) c->st.drop_depth--; }

		switch (oid) {
		case T_UL: case T_OL: list_pop(c); break;
		case T_BLOCKQUOTE: if (c->st.quote_depth) c->st.quote_depth--; break;
		case T_PRE: if (c->st.pre_depth) c->st.pre_depth--; break;
		case T_TITLE: c->st.flags &= (uint8_t)~HTML_ST_IN_TITLE; break;
		case T_A:
			if (c->anchor_depth) c->anchor_depth--;
			if (!c->anchor_depth) {
				span_close(c);
				c->cur_href[0] = '\0';
				c->cur_link = -1;
			}
			break;
		case T_CODE: case T_KBD: case T_SAMP: case T_TT: case T_VAR:
			span_close(c);
			break;
		case T_TR:
			flush_block(c);
			break;
		default: break;
		}

	}

	c->st.depth = (uint8_t)at;

	if (fl & TF_BLOCK) flush_block(c);

}

// -- tag dispatch --

static void tag_done(html_ctx_t *c, bool closing, bool self_closing) {

	const tag_ent_t *t;

	attr_done(c);
	c->tag[c->tag_len] = '\0';
	t = tag_lookup(c->tag);

	if (closing) close_tag(c, t);
	else open_tag(c, t, self_closing);

	// Any text at all locks the charset: a <meta> after the first
	// decoded character cannot retroactively change bytes that have
	// already been folded.
	if (c->have_text) c->charset_locked = true;

	c->tag_len = 0;
	c->pend_href[0] = '\0';
	c->pend_src[0] = '\0';
	c->pend_w = 0;
	c->pend_h = 0;
	c->pend_alt[0] = '\0';
	c->pend_alt_set = false;
	c->pend_charset[0] = '\0';
	c->pend_content[0] = '\0';
	c->pend_type[0] = '\0';
	c->pend_value[0] = '\0';

	// -- when a mark is legal --
	//
	// html_state_t holds the element stack and the counters. It does
	// NOT hold the block currently being assembled: its kind, its list
	// marker, the anchor whose text is running, the span that is open.
	// Those live in the context and are reset by html_restore().
	//
	// So a mark is only legal at a point where all of them are EMPTY.
	// Taking one anywhere else produces a resume that silently
	// disagrees with the original parse -- a heading that comes back
	// as a paragraph, a numbered list item that comes back with no
	// number. Both of those were real, and neither showed up in any
	// unit case: the parse from the start was correct every time, and
	// only the resume invariant in tests/test_html.c caught them.
	//
	// The tokenizer's own state is likewise not saved, so a mark must
	// not be taken inside a raw-text element either. Every raw element
	// (script, style, textarea) is also TF_DROP, so the drop_depth
	// check below covers that too -- and would need revisiting if a
	// raw element that is not dropped were ever added.
	//
	// The contract this creates for a caller is worth stating plainly,
	// because it is not the obvious one: THE MARK VISIBLE DURING AN
	// EMIT IS THE RESUME POINT FOR THAT SAME BLOCK, not for the one
	// after it. An index records (mark, blocks_emitted_before_this).
	if (!c->have_text && !c->pending_space && !c->cur_kind &&
		!c->span_kind && !c->anchor_depth && !c->line.nlinks &&
		!c->line_tight && !c->line.marker[0] && !c->st.drop_depth)
		{ c->mark_off = c->off; c->mark_st = c->st; }

}

// -- 4. tokenizer -------------------------------------------------

enum {
	TS_TEXT = 0, TS_ENT, TS_LT, TS_TAG_NAME, TS_END_NAME,
	TS_ATTRS, TS_ATTR_NAME, TS_AFTER_NAME, TS_BEFORE_VAL,
	TS_VAL_Q, TS_VAL_U, TS_SELFCLOSE, TS_COMMENT, TS_BOGUS,
	TS_RAW, TS_RAW_LT, TS_RAW_NAME
};

#define TFL_CLOSING 0x01

static void flush_entity(html_ctx_t *c, bool terminated) {

	uint32_t cp;

	c->ent[c->ent_len] = '\0';

	if (terminated && c->ent_len) {
		cp = entity_lookup(c->ent);
		if (cp) { put_cp(c, cp); c->ent_len = 0; return; }
	}

	// Not an entity after all. `&` is a legal character in text and in
	// a query string that someone forgot to escape, so it goes through
	// literally rather than being dropped.
	put_cp(c, '&');
	for (uint16_t i = 0; i < c->ent_len; i++) put_cp(c, (unsigned char)c->ent[i]);
	if (terminated) put_cp(c, ';');
	c->ent_len = 0;

}

// One byte, already de-UTF-8'd where that applies. `cp` is a codepoint
// for text and a raw byte for markup, which is safe because every
// character with syntactic meaning in HTML is ASCII.
static void step(html_ctx_t *c, uint32_t cp) {

	char ch = (cp < 0x80) ? (char)cp : (char)0xFF;

	switch (c->ts) {

	case TS_TEXT:
		if (ch == '<') { c->ts = TS_LT; c->tag_len = 0; c->tflags = 0; }
		else if (ch == '&') { c->ts = TS_ENT; c->ent_len = 0; }
		else put_cp(c, cp);
		break;

	case TS_ENT:
		if (ch == ';') { flush_entity(c, true); c->ts = TS_TEXT; }
		else if (ch == '&') { flush_entity(c, false); c->ent_len = 0; }
		else if (c->ent_len >= sizeof(c->ent) - 1 || ws(ch) || ch == '<') {
			flush_entity(c, false);
			c->ts = TS_TEXT;
			step(c, cp);		// reprocess -- see the note below
		}
		else c->ent[c->ent_len++] = ch;
		break;

	case TS_LT:
		if (ch == '/') { c->tflags |= TFL_CLOSING; c->ts = TS_END_NAME; }
		else if (ch == '!' || ch == '?') { c->ts = TS_BOGUS; c->raw_match = 0; }
		else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
			c->tag[0] = lc(ch); c->tag_len = 1; c->ts = TS_TAG_NAME;
		} else {
			// "a < b" in running text. The '<' is content.
			put_cp(c, '<');
			c->ts = TS_TEXT;
			step(c, cp);
		}
		break;

	case TS_TAG_NAME:
	case TS_END_NAME:
		if (ch == '>') {
			bool closing = (c->tflags & TFL_CLOSING) != 0;
			uint8_t was_raw = 0;
			const tag_ent_t *t;
			c->tag[c->tag_len] = '\0';
			t = tag_lookup(c->tag);
			if (!closing && t && (t->flags & TF_RAW)) was_raw = t->id;
			tag_done(c, closing, false);
			if (was_raw) {
				c->ts = TS_RAW;
				c->raw_tag = was_raw;
				c->raw_match = 0;
			} else c->ts = TS_TEXT;
		}
		else if (ch == '/') c->ts = TS_SELFCLOSE;
		else if (ws(ch)) { c->ts = TS_ATTRS; c->attr_len = 0; c->val_len = 0; }
		else if (c->tag_len < sizeof(c->tag) - 1) c->tag[c->tag_len++] = lc(ch);
		break;

	case TS_ATTRS:
		if (ch == '>') {
			bool closing = (c->tflags & TFL_CLOSING) != 0;
			uint8_t was_raw = 0;
			const tag_ent_t *t;
			c->tag[c->tag_len] = '\0';
			t = tag_lookup(c->tag);
			if (!closing && t && (t->flags & TF_RAW)) was_raw = t->id;
			tag_done(c, closing, false);
			if (was_raw) { c->ts = TS_RAW; c->raw_tag = was_raw; c->raw_match = 0; }
			else c->ts = TS_TEXT;
		}
		else if (ch == '/') c->ts = TS_SELFCLOSE;
		else if (ws(ch)) { /* stay */ }
		else {
			attr_done(c);
			c->attr_len = 0;
			c->attr[0] = lc(ch);
			c->attr_len = 1;
			c->ts = TS_ATTR_NAME;
		}
		break;

	case TS_ATTR_NAME:
		if (ch == '=') { c->ts = TS_BEFORE_VAL; c->val_len = 0; }
		else if (ws(ch)) c->ts = TS_AFTER_NAME;
		else if (ch == '>' || ch == '/') { c->ts = TS_ATTRS; step(c, cp); }
		else {
			if (c->attr_len < sizeof(c->attr) - 1) c->attr[c->attr_len++] = lc(ch);
			c->attr[c->attr_len] = '\0';
		}
		break;

	case TS_AFTER_NAME:
		if (ch == '=') { c->ts = TS_BEFORE_VAL; c->val_len = 0; }
		else if (ws(ch)) { /* stay */ }
		else { c->ts = TS_ATTRS; step(c, cp); }
		break;

	case TS_BEFORE_VAL:
		if (ws(ch)) { /* stay */ }
		else if (ch == '"' || ch == '\'') { c->quote = ch; c->ts = TS_VAL_Q; }
		else if (ch == '>') { c->ts = TS_ATTRS; step(c, cp); }
		else { c->ts = TS_VAL_U; step(c, cp); }
		break;

	case TS_VAL_Q:
		if (ch == c->quote) { c->val[c->val_len] = '\0'; c->ts = TS_ATTRS; }
		else if (c->val_len < sizeof(c->val) - 1) {
			c->val[c->val_len++] = (cp < 0x80) ? (char)cp : '?';
			c->val[c->val_len] = '\0';
		}
		break;

	case TS_VAL_U:
		if (ws(ch) || ch == '>') { c->val[c->val_len] = '\0'; c->ts = TS_ATTRS; step(c, cp); }
		else if (c->val_len < sizeof(c->val) - 1) {
			c->val[c->val_len++] = (cp < 0x80) ? (char)cp : '?';
			c->val[c->val_len] = '\0';
		}
		break;

	case TS_SELFCLOSE:
		if (ch == '>') {
			tag_done(c, (c->tflags & TFL_CLOSING) != 0, true);
			c->ts = TS_TEXT;
		} else { c->ts = TS_ATTRS; step(c, cp); }
		break;

	case TS_COMMENT:
		// "-->" only. The specification's "--!>" variant and its
		// several bogus-comment cases are not worth the states; a
		// comment that never terminates swallows the rest of the
		// document, which is exactly what a browser does too.
		if (ch == '-') { if (c->raw_match < 2) c->raw_match++; }
		else if (ch == '>' && c->raw_match >= 2) { c->raw_match = 0; c->ts = TS_TEXT; }
		else c->raw_match = 0;
		break;

	case TS_BOGUS:
		// <!DOCTYPE ...>, <?xml ...>, and the opening of a comment.
		if (c->raw_match == 0 && ch == '-') { c->raw_match = 1; break; }
		if (c->raw_match == 1 && ch == '-') { c->raw_match = 0; c->ts = TS_COMMENT; break; }
		c->raw_match = 0;
		if (ch == '>') c->ts = TS_TEXT;
		break;

	case TS_RAW:
		// Inside <script>/<style>/<textarea>: no markup, no entities,
		// nothing until the matching close tag. Anything else and a
		// `if (a < b)` in a script becomes a tag.
		if (ch == '<') { c->ts = TS_RAW_LT; c->raw_match = 0; }
		break;

	case TS_RAW_LT:
		if (ch == '/') { c->ts = TS_RAW_NAME; c->tag_len = 0; }
		else { c->ts = TS_RAW; step(c, cp); }
		break;

	case TS_RAW_NAME:
		if (ch == '>' || ws(ch)) {
			const tag_ent_t *t;
			c->tag[c->tag_len] = '\0';
			t = tag_lookup(c->tag);
			if (t && t->id == c->raw_tag) {
				if (ch == '>') { tag_done(c, true, false); c->ts = TS_TEXT; }
				else { c->tflags = TFL_CLOSING; c->ts = TS_ATTRS; }
			} else {
				c->ts = TS_RAW;
			}
		}
		else if (c->tag_len < sizeof(c->tag) - 1) c->tag[c->tag_len++] = lc(ch);
		else c->ts = TS_RAW;
		break;

	default:
		c->ts = TS_TEXT;
		break;

	}

}

// -- 5. public API ------------------------------------------------

void html_init(html_ctx_t *ctx, html_emit_fn emit, void *user) {
	memset(ctx, 0, sizeof(*ctx));
	ctx->emit = emit;
	ctx->user = user;
	ctx->cur_link = -1;
}

void html_set_charset(html_ctx_t *ctx, html_charset_t cs) {
	ctx->charset = cs;
	// A charset from the transport is authoritative and cannot be
	// overridden by a <meta> later in the document (html.h).
	ctx->charset_locked = true;
}

void html_restore(html_ctx_t *ctx, const html_state_t *st, uint32_t off) {

	html_emit_fn emit = ctx->emit;
	void *user = ctx->user;
	html_charset_t cs = ctx->charset;
	bool locked = ctx->charset_locked;
	char title[HTML_TITLE_MAX];
	uint16_t tlen = ctx->title_len;

	memcpy(title, ctx->title, sizeof(title));

	memset(ctx, 0, sizeof(*ctx));
	ctx->emit = emit;
	ctx->user = user;
	ctx->charset = cs;
	ctx->charset_locked = locked;
	ctx->cur_link = -1;
	ctx->st = *st;
	ctx->off = off;
	ctx->mark_off = off;
	ctx->mark_st = *st;

	// The title is carried across a restore rather than re-derived.
	// It was found during the first pass over the head; a resume in
	// the middle of the body would never see it again, and the window
	// title would blank out on every scroll.
	memcpy(ctx->title, title, sizeof(title));
	ctx->title_len = tlen;

}

// How many bytes the UTF-8 sequence starting with `lead` occupies, or
// 0 if it cannot start one.
static uint32_t utf8_need(unsigned char lead) {
	if (lead < 0x80) return 1;
	if ((lead & 0xE0) == 0xC0) return 2;
	if ((lead & 0xF0) == 0xE0) return 3;
	if ((lead & 0xF8) == 0xF0) return 4;
	return 0;
}

void html_feed(html_ctx_t *ctx, const char *data, uint32_t len) {

	uint32_t i = 0;

	// -- finish a character held over from the previous chunk --
	//
	// A multi-byte sequence can straddle a feed boundary, and chunks
	// here are 512 bytes (ZSTREAM_CHUNK_SIZE_DEFAULT), so on a page
	// with any accented text at all this happens constantly rather
	// than rarely. Without the carry, every one of those boundaries
	// produces two replacement marks in the middle of a word.
	//
	// If the held bytes turn out to be malformed, the sequence is
	// reported as one replacement character and the rest of the carry
	// is dropped rather than re-examined. That loses at most three
	// bytes of an already broken document, and re-examining them means
	// a pushback buffer, which is exactly the thing a resumable
	// tokenizer must not have.
	if (ctx->pend_len) {

		uint32_t need = utf8_need((unsigned char)ctx->pend[0]);
		uint32_t cp, used;

		while (ctx->pend_len < need && ctx->pend_len < sizeof(ctx->pend) &&
			i < len) {
			ctx->pend[ctx->pend_len++] = data[i++];
			ctx->off++;
		}

		if (ctx->pend_len >= need || i >= len) {
			if (ctx->pend_len >= need) {
				used = uni_utf8_next(ctx->pend, ctx->pend_len, &cp);
				(void)used;
				step(ctx, cp);
				ctx->pend_len = 0;
			} else {
				// Still short and the chunk is exhausted; keep waiting.
				return;
			}
		}

	}

	while (i < len) {

		uint32_t cp, used;

		if (ctx->charset == HTML_CS_CP1252) {

			unsigned char b = (unsigned char)data[i];
			cp = (b >= 0x80 && b <= 0x9F) ? cp1252_high[b - 0x80] : b;
			used = 1;

		} else if ((unsigned char)data[i] < 0x80) {

			cp = (unsigned char)data[i];
			used = 1;

		} else {

			uint32_t need = utf8_need((unsigned char)data[i]);

			if (need > 1 && i + need > len) {
				uint32_t avail = len - i;
				memcpy(ctx->pend, data + i, avail);
				ctx->pend_len = (uint8_t)avail;
				ctx->off += avail;
				return;
			}

			used = uni_utf8_next(data + i, len - i, &cp);

		}

		// The offset is advanced BEFORE the byte is processed, not
		// after, and that is not a detail: tag_done() records
		// ctx->off as a resume point, and a resume point must be the
		// offset of the byte AFTER the tag's '>'. Advancing
		// afterwards made every mark point at the '>' itself, so a
		// resumed parse began with a stray '>' in the first block.
		// Caught by tests/test_html.c's resume invariant, which is
		// exactly the class of bug it exists for -- the unit cases
		// all passed.
		i += used;
		ctx->off += used;
		step(ctx, cp);

	}

}

void html_finish(html_ctx_t *ctx) {
	if (ctx->ts == TS_ENT) flush_entity(ctx, false);
	flush_block(ctx);
}

uint32_t html_mark_offset(const html_ctx_t *ctx) { return ctx->mark_off; }
const html_state_t *html_mark_state(const html_ctx_t *ctx) { return &ctx->mark_st; }
const char *html_title(const html_ctx_t *ctx) { return ctx->title; }
