/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * httpc -- see httpc.h.
 */

#include "httpc.h"

#define HC_HEADERS_MAX 8192

static uint32_t put_s(char *o, uint32_t at, uint32_t max, const char *s) {
	while (*s) {
		if (at + 1 >= max) return max;
		o[at++] = *s++;
	}
	return at;
}

static uint32_t put_u(char *o, uint32_t at, uint32_t max, uint32_t v) {
	char d[10];
	int n = 0;
	do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
	while (n) {
		if (at + 1 >= max) return max;
		o[at++] = d[--n];
	}
	return at;
}

uint32_t hc_request(hc_t *h, char *out, uint32_t outmax, const char *host,
	const char *path, uint32_t off, uint32_t len) {
	uint32_t at = 0;
	if (!len || off + len < off) return 0;
	at = put_s(out, at, outmax, "GET ");
	at = put_s(out, at, outmax, path);
	at = put_s(out, at, outmax, " HTTP/1.1\r\nHost: ");
	at = put_s(out, at, outmax, host);
	at = put_s(out, at, outmax, "\r\nRange: bytes=");
	at = put_u(out, at, outmax, off);
	at = put_s(out, at, outmax, "-");
	at = put_u(out, at, outmax, off + len - 1);
	at = put_s(out, at, outmax, "\r\nUser-Agent: zeitlos-cinema\r\nAccept-Encoding: identity\r\nConnection: keep-alive\r\n\r\n");
	if (at >= outmax) return 0;
	out[at] = 0;
	h->state = HC_HEAD;
	h->err = HC_E_NONE;
	h->status = 0;
	h->off = off;
	h->want = len;
	h->got = 0;
	h->left = 0;
	h->take = 0;
	h->hbytes = 0;
	h->has_len = false;
	h->to_close = false;
	h->skip = false;
	h->ll = 0;
	return at;
}

static void fail(hc_t *h, uint8_t e) {
	h->state = HC_FAIL;
	h->err = e;
	h->to_close = true;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* does the line start with `name` (lower case, with its colon)? then the value */
static const char *header(const char *line, const char *name) {
	while (*name) {
		if (lower(*line) != *name) return 0;
		line++; name++;
	}
	while (*line == ' ' || *line == '\t') line++;
	return line;
}

static bool number(const char *s, uint32_t *out) {
	uint32_t v = 0;
	int n = 0;
	while (*s >= '0' && *s <= '9') {
		if (v > 429496729u || (v == 429496729u && *s > '5')) return false;
		v = v * 10 + (uint32_t)(*s - '0');
		s++; n++;
	}
	if (!n) return false;
	*out = v;
	return true;
}

static bool has_word(const char *s, const char *w) {
	for (; *s; s++) {
		const char *a = s, *b = w;
		while (*b && lower(*a) == *b) { a++; b++; }
		if (!*b) return true;
	}
	return false;
}

static void headers_done(hc_t *h) {
	if (h->status == 416) {             /* past the end: nothing, and that is an answer */
		h->left = h->has_len ? h->left : 0;
		h->take = 0;
		if (!h->has_len) h->to_close = true;
		h->state = h->left ? HC_BODY : HC_DONE;
		return;
	}
	if (h->status == 200) {
		if (h->off) { fail(h, HC_E_NORANGE); return; }
		h->to_close = true;             /* the rest of the file is coming: not wanted */
	} else if (h->status != 206) {
		fail(h, HC_E_STATUS);
		return;
	}
	if (!h->has_len) { h->left = 0xffffffffu; h->to_close = true; }
	h->take = h->want;
	if (h->left == 0) { h->state = HC_DONE; return; }
	h->state = HC_BODY;
}

static void line_done(hc_t *h) {
	const char *v;
	h->line[h->ll] = 0;
	if (!h->status) {
		/* "HTTP/1.x SSS ..." */
		uint32_t s;
		if (h->ll < 12 || h->line[0] != 'H' || h->line[1] != 'T' || h->line[2] != 'T' || h->line[3] != 'P' ||
		    h->line[4] != '/' || h->line[8] != ' ' || !number(h->line + 9, &s) || s < 100 || s > 599) {
			fail(h, HC_E_HEADER);
			return;
		}
		h->status = (uint16_t)s;
		if (h->line[7] == '0') h->to_close = true;      /* HTTP/1.0: one answer a connection */
		return;
	}
	if (!h->ll) {
		if (h->status < 200) { h->status = 0; h->has_len = false; return; }     /* 1xx: the real one follows */
		headers_done(h);
		return;
	}
	if ((v = header(h->line, "content-length:"))) {
		uint32_t n;
		if (!number(v, &n) || (h->has_len && n != h->left)) { fail(h, HC_E_HEADER); return; }
		h->left = n;
		h->has_len = true;
	} else if ((v = header(h->line, "transfer-encoding:"))) {
		if (has_word(v, "chunked")) fail(h, HC_E_CHUNKED);
	} else if ((v = header(h->line, "connection:"))) {
		if (has_word(v, "close")) h->to_close = true;
	} else if ((v = header(h->line, "content-range:"))) {
		/* "bytes a-b/total" */
		while (*v && *v != '/') v++;
		if (*v == '/') { uint32_t t; if (number(v + 1, &t)) h->total = t; }
	}
}

void hc_feed(hc_t *h, const uint8_t *p, uint32_t n, hc_body_fn body, void *user) {
	uint32_t i = 0;
	while (i < n) {
		if (h->state == HC_HEAD) {
			char c = (char)p[i++];
			if (++h->hbytes > HC_HEADERS_MAX) { fail(h, HC_E_HEADER); return; }
			if (c == '\n') {
				if (h->ll && h->line[h->ll - 1] == '\r') h->ll--;
				if (h->skip) { h->skip = false; h->ll = 0; continue; }
				line_done(h);
				h->ll = 0;
			} else if (h->skip) {
				/* an over-long line: read past it */
			} else if (h->ll < HC_LINE_MAX - 1) {
				h->line[h->ll++] = c;
			} else {
				/* too long to keep. The status line and the headers
				 * acted on are short; one of those cut would be another
				 * header, so only a line that is none of them may go */
				h->line[h->ll] = 0;
				if (!h->status || header(h->line, "content-length:") || header(h->line, "transfer-encoding:") ||
				    header(h->line, "connection:") || header(h->line, "content-range:")) {
					fail(h, HC_E_HEADER);
					return;
				}
				h->skip = true;
			}
		} else if (h->state == HC_BODY) {
			uint32_t k = n - i, use;
			if (k > h->left) k = h->left;
			use = h->take - h->got < k ? h->take - h->got : k;
			if (use) {
				if (body) body(user, p + i, use);
				h->got += use;
			}
			i += k;
			if (h->left != 0xffffffffu) h->left -= k;
			if (h->left == 0) { h->state = HC_DONE; return; }
			if (h->got >= h->take && h->to_close) { h->state = HC_DONE; return; }   /* the rest is not ours */
		} else {
			return;
		}
	}
}

void hc_closed(hc_t *h) {
	if (h->state == HC_BODY) {
		if (h->left == 0xffffffffu || h->got >= h->take) h->state = HC_DONE;
		else fail(h, HC_E_SHORT);
	} else if (h->state == HC_HEAD) {
		fail(h, HC_E_SHORT);
	}
}

const char *hc_error(const hc_t *h) {
	switch (h->err) {
	case HC_E_STATUS:  return h->status == 404 ? "not found (404)" : "the server refused";
	case HC_E_NORANGE: return "the server does not serve ranges";
	case HC_E_CHUNKED: return "a chunked answer";
	case HC_E_HEADER:  return "not an HTTP answer";
	case HC_E_SHORT:   return "the connection closed early";
	default:           return "";
	}
}

bool hc_url(const char *url, char *host, uint32_t hostmax, uint16_t *port,
	char *path, uint32_t pathmax) {
	static const char pre[] = "http://";
	uint32_t i, n = 0;
	for (i = 0; pre[i]; i++) if (lower(url[i]) != pre[i]) return false;
	url += i;
	while (*url && *url != '/' && *url != ':') {
		if (n + 1 >= hostmax) return false;
		host[n++] = *url++;
	}
	host[n] = 0;
	if (!n) return false;
	*port = 80;
	if (*url == ':') {
		uint32_t v;
		url++;
		if (!number(url, &v) || !v || v > 65535) return false;
		*port = (uint16_t)v;
		while (*url >= '0' && *url <= '9') url++;
	}
	if (*url && *url != '/') return false;
	n = 0;
	if (!*url) path[n++] = '/';
	while (*url && *url != '#') {
		if (n + 1 >= pathmax) return false;
		if (*url == ' ') {                  /* a space typed in a name */
			if (n + 3 >= pathmax) return false;
			path[n++] = '%'; path[n++] = '2'; path[n++] = '0';
			url++;
			continue;
		}
		path[n++] = *url++;
	}
	path[n] = 0;
	return true;
}
