/*
 * Zeitlos -- sw/apps/web
 *
 * Gopher and Gemini -- see smallweb.h and docs/gopher_gemini.md.
 */

#include <string.h>

#include "smallweb.h"

// -- output helpers --

static void emit(sw_ctx_t *c, const char *s, uint32_t n) {
	if (n) c->out(c->user, s, n);
}

static void emits(sw_ctx_t *c, const char *s) {
	emit(c, s, (uint32_t)strlen(s));
}

// Text as HTML: the four characters that would otherwise be markup.
// Runs of ordinary bytes go out in one call.
static void emit_text(sw_ctx_t *c, const char *s, uint32_t n) {
	uint32_t i, from = 0;
	for (i = 0; i < n; i++) {
		const char *rep = NULL;
		switch (s[i]) {
		case '&': rep = "&amp;"; break;
		case '<': rep = "&lt;"; break;
		case '>': rep = "&gt;"; break;
		case '"': rep = "&quot;"; break;
		default: break;
		}
		if (rep) {
			emit(c, s + from, i - from);
			emits(c, rep);
			from = i + 1;
		}
	}
	emit(c, s + from, n - from);
}

static bool is_ws(char ch) { return ch == ' ' || ch == '\t'; }

static const char hexd[] = "0123456789ABCDEF";

// A selector into a URL path: everything but the unreserved set and
// '/' as %XX, so a selector with spaces, '?' or '#' in it survives the
// round trip through url_parse() and back out in sw_request().
static void emit_pct(sw_ctx_t *c, const char *s, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)s[i];
		if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
		    (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' ||
		    ch == '_' || ch == '~' || ch == '/') {
			emit(c, (const char *)&s[i], 1);
		} else {
			char e[3] = { '%', hexd[ch >> 4], hexd[ch & 15] };
			emit(c, e, 3);
		}
	}
}

// %XX back to bytes, into out (NUL-terminated). Returns the length, or
// -1 if it does not fit. A '%' not followed by two hex digits is kept.
static int pct_decode(const char *s, char *out, uint32_t cap) {
	uint32_t n = 0;
	for (; *s; s++) {
		char ch = *s;
		if (ch == '%' && s[1] && s[2]) {
			int hi = -1, lo = -1;
			const char *p;
			if ((p = strchr(hexd, (s[1] >= 'a' && s[1] <= 'f') ? s[1] - 32 : s[1]))) hi = (int)(p - hexd);
			if ((p = strchr(hexd, (s[2] >= 'a' && s[2] <= 'f') ? s[2] - 32 : s[2]))) lo = (int)(p - hexd);
			if (hi >= 0 && lo >= 0 && s[1] && s[2]) { ch = (char)(hi * 16 + lo); s += 2; }
		}
		if (n + 1 >= cap) return -1;
		out[n++] = ch;
	}
	out[n] = 0;
	return (int)n;
}

// -- requests --

char sw_gopher_type(const url_t *u) {
	if (u->path[0] == '/' && u->path[1]) return u->path[1];
	return '1';
}

uint32_t sw_request(const url_t *u, char *out, uint32_t cap) {

	uint32_t n;

	if (u->scheme == URL_SCHEME_GEMINI) {
		// Gemini: the absolute URL, at most 1024 bytes, then CRLF.
		//
		// Percent-encoded on the way out: a query typed into the URL
		// bar in answer to an input request (status 1x) arrives with
		// its spaces and non-ASCII letters as typed, and url.c keeps
		// them. A server is entitled to refuse those, and a space
		// would end the request line early at some servers.
		char raw[1100];
		url_format(u, raw, sizeof(raw), false);
		n = 0;
		for (const char *p = raw; *p; p++) {
			unsigned char ch = (unsigned char)*p;
			if (ch <= 0x20 || ch >= 0x7f) {
				if (n + 3 >= cap) return 0;
				out[n++] = '%'; out[n++] = hexd[ch >> 4]; out[n++] = hexd[ch & 15];
			} else {
				if (n + 1 >= cap) return 0;
				out[n++] = (char)ch;
			}
		}
		if (n == 0 || n > 1024 || n + 3 > cap) return 0;
		memcpy(out + n, "\r\n", 3);
		return n + 2;
	}

	if (u->scheme == URL_SCHEME_GOPHER) {
		// Gopher: the selector is the path after "/" and the type.
		const char *sel = (u->path[0] == '/' && u->path[1]) ? u->path + 2 : "";
		int k = pct_decode(sel, out, cap);
		if (k < 0) return 0;
		n = (uint32_t)k;
		// A search (type 7): the terms follow a TAB.
		if (u->has_query) {
			if (n + 2 > cap) return 0;
			out[n++] = '\t';
			k = pct_decode(u->query, out + n, cap - n);
			if (k < 0) return 0;
			n += (uint32_t)k;
		}
		if (n + 3 > cap) return 0;
		memcpy(out + n, "\r\n", 3);
		return n + 2;
	}

	return 0;

}

// -- the body --

static void start(sw_ctx_t *c) {
	if (c->started) return;
	c->started = true;
	emits(c, "<html><head><meta charset=\"utf-8\"></head><body>\n");
	if (c->body == SW_BODY_MENU || c->body == SW_BODY_TEXT)
		emits(c, "<pre>");
}

// Chooses what the body is, once the Gemini header is known.
static void gemini_choose(sw_ctx_t *c) {

	const char *m = c->meta;

	if (sw_status_class(c) != 2) { c->body = SW_BODY_DISCARD; return; }

	// "The default is text/gemini; charset=utf-8" for an empty meta.
	if (!m[0] || !strncmp(m, "text/gemini", 11)) c->body = SW_BODY_GEMTEXT;
	else if (!strncmp(m, "text/html", 9)) c->body = SW_BODY_RAW;
	else if (!strncmp(m, "text/", 5)) c->body = SW_BODY_TEXT;
	else c->body = SW_BODY_DISCARD;

}

// "20 text/gemini\r\n": two digits, a space, the meta. Returns false
// for a header that is not one, which the caller reports.
static void gemini_header(sw_ctx_t *c, const char *l, uint32_t n) {

	uint32_t i = 0;

	c->header_done = true;
	c->status = -1;
	c->meta[0] = 0;

	if (n >= 2 && l[0] >= '1' && l[0] <= '6' && l[1] >= '0' && l[1] <= '9') {
		c->status = (l[0] - '0') * 10 + (l[1] - '0');
		i = 2;
		while (i < n && is_ws(l[i])) i++;
		if (n - i >= SW_META_MAX) n = i + SW_META_MAX - 1;
		memcpy(c->meta, l + i, n - i);
		c->meta[n - i] = 0;
	}

	gemini_choose(c);

}

static void para_close(sw_ctx_t *c) {
	if (c->para) { emits(c, "</p>\n"); c->para = false; }
}

static void list_close(sw_ctx_t *c) {
	if (c->list) { emits(c, "</ul>\n"); c->list = false; }
}

// One line of text/gemini. The line types are decided by their first
// characters, and only those (gemini spec, section 5.4).
static void gemtext_line(sw_ctx_t *c, const char *l, uint32_t n) {

	if (n >= 3 && !memcmp(l, "```", 3)) {
		para_close(c);
		list_close(c);
		emits(c, c->pre ? "</pre>\n" : "<pre>");
		c->pre = !c->pre;
		return;
	}

	if (c->pre) {
		emit_text(c, l, n);
		emits(c, "\n");
		return;
	}

	if (!(n >= 2 && l[0] == '*' && l[1] == ' ')) list_close(c);

	// A blank line, BEFORE anything looks at l[0]. On an empty line
	// l[0] is not part of the line: the line buffer is reused, so it is
	// whatever the PREVIOUS line started with. After a heading that was
	// '#', and the heading branch below did `n - i` with n = 0 and
	// i = 1 -- unsigned, so four billion -- and emit_text() copied
	// memory past the buffer into the page until something gave out.
	// "# Title" then a blank line is the most ordinary gemtext there
	// is; skyjake.fi's Cosmos filled 20MB of spool before the card ran
	// short, and no click could stop it, because it all happened inside
	// one call. Every other test of l[0] below also checks n first.
	if (n == 0) { para_close(c); return; }

	if (n >= 2 && l[0] == '=' && l[1] == '>') {
		uint32_t i = 2, u0, u1;
		para_close(c);
		while (i < n && is_ws(l[i])) i++;
		u0 = i;
		while (i < n && !is_ws(l[i])) i++;
		u1 = i;
		while (i < n && is_ws(l[i])) i++;
		if (u1 == u0) return;			// "=>" with no URL: nothing
		emits(c, "<p>=&gt; <a href=\"");
		emit_text(c, l + u0, u1 - u0);
		emits(c, "\">");
		if (i < n) emit_text(c, l + i, n - i);
		else emit_text(c, l + u0, u1 - u0);
		emits(c, "</a></p>\n");
		return;
	}

	if (n >= 1 && l[0] == '#') {
		int lv = 1;
		uint32_t i = 1;
		char tag[6];
		para_close(c);
		while (i < n && l[i] == '#' && lv < 3) { lv++; i++; }
		while (i < n && is_ws(l[i])) i++;
		tag[0] = '<'; tag[1] = 'h'; tag[2] = (char)('0' + lv); tag[3] = '>'; tag[4] = 0;
		emits(c, tag);
		emit_text(c, l + i, n - i);
		tag[1] = '/'; tag[2] = 'h'; tag[3] = (char)('0' + lv); tag[4] = '>'; tag[5] = 0;
		emits(c, tag);
		emits(c, "\n");
		return;
	}

	if (n >= 2 && l[0] == '*' && l[1] == ' ') {
		para_close(c);
		if (!c->list) { emits(c, "<ul>"); c->list = true; }
		emits(c, "<li>");
		emit_text(c, l + 2, n - 2);
		emits(c, "</li>\n");
		return;
	}

	if (n >= 1 && l[0] == '>') {
		uint32_t i = 1;
		para_close(c);
		while (i < n && is_ws(l[i])) i++;
		emits(c, "<blockquote>");
		emit_text(c, l + i, n - i);
		emits(c, "</blockquote>\n");
		return;
	}

	// Text. Consecutive lines stay together (a poem, an address), and
	// a blank line (handled above) is what separates paragraphs --
	// which is how gemtext is written, one line per paragraph with
	// blank lines between, or several short lines that belong together.
	emits(c, c->para ? "<br>" : "<p>");
	c->para = true;
	emit_text(c, l, n);

}

// The marker in front of a Gopher menu item, by type, so a menu read
// on a 1bpp screen still says which lines go where.
static const char *menu_mark(char t) {
	switch (t) {
	case '0': return "[txt] ";
	case '1': return "[dir] ";
	case '7': return "[ ? ] ";
	case 'h': return "[www] ";
	case 'g': case 'I': case 'p': return "[img] ";
	case '9': case '5': case '4': case '6': return "[bin] ";
	case 's': case '<': return "[snd] ";
	case '8': case 'T': return "[tel] ";
	default: return "[ - ] ";
	}
}

// One line of a Gopher menu: TYPE DISPLAY \t SELECTOR \t HOST \t PORT.
static void menu_line(sw_ctx_t *c, const char *l, uint32_t n) {

	const char *f[4];
	uint32_t fl[4], nf = 0, i, from = 1;
	char t;

	if (c->menu_done || n == 0) return;
	if (n == 1 && l[0] == '.') { c->menu_done = true; return; }

	t = l[0];
	for (i = 1; i <= n && nf < 4; i++) {
		if (i == n || l[i] == '\t') {
			f[nf] = l + from;
			fl[nf] = i - from;
			nf++;
			from = i + 1;
		}
	}

	// Information lines, errors, and anything malformed: text.
	if (t == 'i' || t == '3' || nf < 4) {
		emits(c, "      ");
		emit_text(c, t == 'i' || t == '3' ? f[0] : l, t == 'i' || t == '3' ? fl[0] : n);
		emits(c, "\n");
		return;
	}

	emits(c, menu_mark(t));

	// Followable: text, menus, searches and web links. The rest are
	// shown for what they are but are not links -- this browser cannot
	// do anything with a binary or a telnet session.
	if (t == '0' || t == '1' || t == '7' || t == 'h') {
		emits(c, "<a href=\"");
		if (t == 'h' && fl[1] > 4 && !memcmp(f[1], "URL:", 4)) {
			emit_text(c, f[1] + 4, fl[1] - 4);
		} else {
			bool dflt = (fl[3] == 2 && !memcmp(f[3], "70", 2));
			char ty[2] = { t, 0 };
			emits(c, "gopher://");
			emit_text(c, f[2], fl[2]);
			if (!dflt && fl[3]) { emits(c, ":"); emit_text(c, f[3], fl[3]); }
			emits(c, "/");
			emits(c, ty);
			emit_pct(c, f[1], fl[1]);
		}
		emits(c, "\">");
		emit_text(c, f[0], fl[0]);
		emits(c, "</a>\n");
	} else {
		emit_text(c, f[0], fl[0]);
		emits(c, "\n");
	}

}

static void body_line(sw_ctx_t *c, const char *l, uint32_t n) {

	// CRLF is the norm in both protocols; a bare LF is common enough.
	if (n && l[n - 1] == '\r') n--;

	if (c->gemini && !c->header_done) { gemini_header(c, l, n); return; }

	switch (c->body) {
	case SW_BODY_GEMTEXT: start(c); gemtext_line(c, l, n); break;
	case SW_BODY_MENU:    start(c); menu_line(c, l, n); break;
	case SW_BODY_TEXT:
		start(c);
		// A Gopher text item ends with a "." line; ".." at the start
		// of a line stands for ".".
		if (!c->gemini) {
			if (c->menu_done) break;
			if (n == 1 && l[0] == '.') { c->menu_done = true; break; }
			if (n >= 2 && l[0] == '.' && l[1] == '.') { l++; n--; }
		}
		emit_text(c, l, n);
		emits(c, "\n");
		break;
	default: break;
	}

}

void sw_begin(sw_ctx_t *c, const url_t *u, sw_out_fn out, void *user) {

	memset(c, 0, sizeof(*c));
	c->out = out;
	c->user = user;
	c->status = -1;
	c->gemini = (u->scheme == URL_SCHEME_GEMINI);

	if (!c->gemini) {
		c->gopher_type = sw_gopher_type(u);
		switch (c->gopher_type) {
		case '1': case '7': c->body = SW_BODY_MENU; break;
		case '0':           c->body = SW_BODY_TEXT; break;
		case 'h':           c->body = SW_BODY_RAW; break;
		default:            c->body = SW_BODY_DISCARD; break;
		}
	}

}

void sw_feed(sw_ctx_t *c, const char *d, uint32_t n) {

	for (uint32_t i = 0; i < n; i++) {

		// Raw bodies go straight through, once any header is past.
		if (c->body == SW_BODY_RAW && (!c->gemini || c->header_done)) {
			emit(c, d + i, n - i);
			return;
		}

		if (d[i] == '\n') {
			body_line(c, c->line, c->llen);
			c->llen = 0;
			continue;
		}

		if (c->llen == sizeof(c->line)) {
			// A line longer than the buffer is converted in pieces --
			// except the Gemini header, which may not be this long.
			body_line(c, c->line, c->llen);
			c->llen = 0;
		}
		c->line[c->llen++] = d[i];

	}

}

void sw_end(sw_ctx_t *c) {

	if (c->llen) { body_line(c, c->line, c->llen); c->llen = 0; }

	if (!c->started) {
		if (c->body == SW_BODY_DISCARD || c->body == SW_BODY_RAW) return;
		start(c);		// an empty document is still a document
	}

	if (c->body == SW_BODY_GEMTEXT) {
		para_close(c);
		list_close(c);
		if (c->pre) emits(c, "</pre>");
	}
	if (c->body == SW_BODY_MENU || c->body == SW_BODY_TEXT) emits(c, "</pre>");
	emits(c, "</body></html>\n");

}
