/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See url.h.
 */

#include <string.h>
#include <stdio.h>

#include "url.h"

// -- small helpers ------------------------------------------------
//
// Written out rather than using <ctype.h>: those are locale-dependent
// in principle and take an int that must not be a negative char,
// which is a real portability trap when the input is arbitrary bytes
// off the network. Everything here is deliberately ASCII-only, which
// is also all the display can render.

static char lc(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool is_alpha(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_scheme_char(char c) {
	return is_alpha(c) || is_digit(c) || c == '+' || c == '-' || c == '.';
}

// Copy with a hard cap, always NUL terminating. Returns false if the
// source did not fit -- every caller here treats that as a parse
// failure rather than using the truncation, for the reason url.h
// gives.
static bool copy_n(char *dst, uint32_t cap, const char *src, uint32_t len) {
	if (len >= cap) return false;
	memcpy(dst, src, len);
	dst[len] = '\0';
	return true;
}

static void append(char *dst, uint32_t cap, uint32_t *len, const char *s) {
	while (*s && *len + 1 < cap) dst[(*len)++] = *s++;
	dst[*len] = '\0';
}

static void append_ch(char *dst, uint32_t cap, uint32_t *len, char c) {
	if (*len + 1 < cap) { dst[(*len)++] = c; dst[*len] = '\0'; }
}

// -- scheme -------------------------------------------------------

static url_scheme_t scheme_from_text(const char *s) {
	if (!strcmp(s, "http"))  return URL_SCHEME_HTTP;
	if (!strcmp(s, "https")) return URL_SCHEME_HTTPS;
	if (!strcmp(s, "file"))  return URL_SCHEME_FILE;
	return URL_SCHEME_OTHER;
}

// -- dot segment removal (RFC 3986 5.2.4) -------------------------
//
// The specification writes this as a loop over an input buffer with
// five cases. Implemented here as a stack of segment start offsets
// instead, which is the same algorithm read backwards and is far
// easier to be sure about: "..' pops, everything else pushes, and the
// output is whatever is left.
//
// The one case worth stating: a ".." that would pop past the root is
// DISCARDED, not propagated. `http://host/../x` is `http://host/x`.
// Servers vary on what they do with the literal form and at least one
// well-known one treats it as a traversal attempt and 400s.
static void remove_dot_segments(const char *in, char *out, uint32_t cap) {

	// The most segments a path can hold: each surviving one costs at
	// least a '/' and one byte, and empty segments are skipped rather
	// than stored.
	uint16_t start[URL_PATH_MAX / 2];
	int n = 0;
	uint32_t i = 0, olen = 0;
	bool trailing_slash = false;

	out[0] = '\0';
	if (!in || !*in) return;

	// Walk segments. A segment is the text between '/' characters.
	while (in[i]) {

		uint32_t seg_start, seg_len;

		if (in[i] == '/') { i++; continue; }

		seg_start = i;
		while (in[i] && in[i] != '/') i++;
		seg_len = i - seg_start;

		if (seg_len == 1 && in[seg_start] == '.') {
			trailing_slash = true;
			continue;
		}

		if (seg_len == 2 && in[seg_start] == '.' && in[seg_start + 1] == '.') {
			if (n > 0) n--;
			trailing_slash = true;
			continue;
		}

		if (n < (int)(sizeof(start) / sizeof(start[0]))) {
			// Stash (offset, length) packed as two entries would be
			// fiddly; store the offset and re-derive the length by
			// scanning, which costs nothing at these sizes and cannot
			// disagree with itself.
			start[n++] = (uint16_t)seg_start;
		}
		trailing_slash = (in[i] == '/');
	}

	for (int k = 0; k < n; k++) {
		uint32_t p = start[k];
		append_ch(out, cap, &olen, '/');
		while (in[p] && in[p] != '/') append_ch(out, cap, &olen, in[p++]);
	}

	if (olen == 0) append_ch(out, cap, &olen, '/');
	else if (trailing_slash) append_ch(out, cap, &olen, '/');
}

// -- parse --------------------------------------------------------

bool url_parse(const char *text, url_t *out) {

	const char *p = text;
	const char *q;
	uint32_t len;

	if (!text || !out) return false;

	memset(out, 0, sizeof(*out));

	len = (uint32_t)strlen(text);
	if (len >= URL_MAX) return false;

	// The EMPTY reference is legal and means "this document" (RFC 3986
	// 5.4.1 lists it). It parses to all-empty with URL_SCHEME_NONE,
	// which url_resolve() then turns back into the base. Callers that
	// need an absolute URL -- the URL bar, a fetch -- check
	// url_is_network()/scheme themselves; rejecting it here instead
	// would break resolving a bare "#frag" link, which is the same
	// case with a fragment attached.
	if (len == 0) return true;

	// Reject control characters outright, wherever they appear. A URL
	// with a raw CR or LF in it is a request-splitting attempt when it
	// reaches http.c, and there is no legitimate source for one.
	for (uint32_t i = 0; i < len; i++)
		if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] == 0x7f)
			return false;

	// -- scheme --
	//
	// A scheme is ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":".
	// The leading-alpha requirement is what keeps "foo.html:8" or a
	// Windows-ish "c:/x" from being read as a scheme.
	if (is_alpha(*p)) {
		q = p;
		while (*q && is_scheme_char(*q)) q++;
		if (*q == ':') {
			char buf[16];
			uint32_t slen = (uint32_t)(q - p);
			if (slen >= sizeof(buf)) return false;
			for (uint32_t i = 0; i < slen; i++) buf[i] = lc(p[i]);
			buf[slen] = '\0';
			strcpy(out->scheme_text, buf);
			out->scheme = scheme_from_text(buf);
			p = q + 1;
		}
	}

	// Anything we cannot act on stops here. The rest of the reference
	// is kept verbatim in `path` so url_format() can echo it back for
	// display -- a `mailto:` link should show its address.
	if (out->scheme == URL_SCHEME_OTHER)
		return copy_n(out->path, sizeof(out->path), p, (uint32_t)strlen(p));

	// -- authority --
	if (p[0] == '/' && p[1] == '/') {

		const char *auth, *host_start, *host_end;

		p += 2;
		auth = p;
		while (*p && *p != '/' && *p != '?' && *p != '#') p++;

		host_start = auth;
		host_end = p;

		// userinfo -- located so it can be DISCARDED. See url.h.
		for (const char *at = auth; at < p; at++)
			if (*at == '@') host_start = at + 1;

		// Port.
		//
		// Located as the LAST ':' at or after the last ']', so a colon
		// inside an IPv6 literal ("[::1]:8080") is not mistaken for
		// the separator. Once found, everything after it MUST be a
		// non-empty run of digits -- an authority like
		// "example.com:80x" is malformed, and the alternative
		// (treating the whole thing as a hostname) means a URL that
		// looks like it names a port silently resolving somewhere
		// else entirely.
		{
			const char *colon = NULL;
			const char *from = host_start;

			for (const char *c = host_start; c < host_end; c++)
				if (*c == ']') from = c + 1;

			for (const char *c = from; c < host_end; c++)
				if (*c == ':') colon = c;

			if (colon) {
				uint32_t port = 0;
				const char *d = colon + 1;
				if (d == host_end) return false;		// "host:" alone
				for (; d < host_end; d++) {
					if (!is_digit(*d)) return false;
					port = port * 10 + (uint32_t)(*d - '0');
					if (port > 65535) return false;
				}
				if (port == 0) return false;			// "host:0"
				out->port = (uint16_t)port;
				host_end = colon;
			}
		}

		// Strip the brackets from an IPv6 literal. Nothing in this
		// tree can connect to one yet (ip.c is v4 only), but parsing
		// it correctly and failing later with "no IPv6 support" is a
		// better failure than a mangled host.
		if (host_end - host_start >= 2 && host_start[0] == '[' &&
			host_end[-1] == ']') {
			host_start++;
			host_end--;
		}

		if (!copy_n(out->host, sizeof(out->host), host_start,
			(uint32_t)(host_end - host_start))) return false;

		for (char *h = out->host; *h; h++) {
			if ((unsigned char)*h > 0x7f) return false;		// see url.h
			*h = lc(*h);
		}
	}

	// -- path, query, fragment --
	{
		const char *path_start = p;
		const char *path_end;

		while (*p && *p != '?' && *p != '#') p++;
		path_end = p;

		if (!copy_n(out->path, sizeof(out->path), path_start,
			(uint32_t)(path_end - path_start))) return false;

		if (*p == '?') {
			const char *qs = ++p;
			while (*p && *p != '#') p++;
			out->has_query = true;
			if (!copy_n(out->query, sizeof(out->query), qs,
				(uint32_t)(p - qs))) return false;
		}

		if (*p == '#') {
			p++;
			if (!copy_n(out->fragment, sizeof(out->fragment), p,
				(uint32_t)strlen(p))) return false;
		}
	}

	// An absolute http(s) URL with no path means "/". Doing this at
	// parse time rather than at request time means url_format() and
	// the URL bar agree with what actually went on the wire.
	if (out->path[0] == '\0' && out->host[0] != '\0')
		strcpy(out->path, "/");

	if ((out->scheme == URL_SCHEME_HTTP || out->scheme == URL_SCHEME_HTTPS) &&
		out->host[0] == '\0')
		return false;

	return true;

}

// -- resolve ------------------------------------------------------

// Scratch for url_resolve(), static rather than automatic.
//
// A url_t is around 1.4KB and url_resolve() needs one plus a merge
// buffer; on the stack that is over 2KB, in a function that sits on
// the redirect path, inside an app whose whole stack+heap allowance is
// 16KB by default (Z_PROC_STACK_SIZE_DEFAULT, sw/os/kernel.h). Static
// is safe here for the same reason it is safe throughout this app:
// one thread, no recursion, and nothing it calls calls back into it.
//
// It also does NOT stop `out` aliasing `base` or `ref` -- that is the
// property callers rely on (a redirect resolves in place) and it is
// exactly why the scratch exists rather than writing through `out`
// directly.
static url_t url_scratch;
static char url_merge[URL_PATH_MAX * 2];

bool url_resolve(const url_t *base, const url_t *ref, url_t *out) {

	url_t *r = &url_scratch;

	if (!base || !ref || !out) return false;
	if (base->scheme == URL_SCHEME_NONE || base->scheme == URL_SCHEME_OTHER)
		return false;
	if ((base->scheme == URL_SCHEME_HTTP || base->scheme == URL_SCHEME_HTTPS) &&
		base->host[0] == '\0')
		return false;

	memset(r, 0, sizeof(*r));

	// A reference we cannot act on resolves to itself. The caller gets
	// it back intact and reports it; see url.h.
	if (ref->scheme == URL_SCHEME_OTHER) { *out = *ref; return true; }

	if (ref->scheme != URL_SCHEME_NONE) {

		*r = *ref;
		remove_dot_segments(ref->path, url_merge, sizeof(url_merge));
		if (strlen(url_merge) >= sizeof(r->path)) return false;
		strcpy(r->path, url_merge);

	} else if (ref->host[0] != '\0') {

		// A network-path reference ("//other.example/x"). Rare but
		// real, and it inherits ONLY the scheme.
		r->scheme = base->scheme;
		strcpy(r->scheme_text, base->scheme_text);
		strcpy(r->host, ref->host);
		r->port = ref->port;
		remove_dot_segments(ref->path, r->path, sizeof(r->path));
		r->has_query = ref->has_query;
		strcpy(r->query, ref->query);
		strcpy(r->fragment, ref->fragment);

	} else {

		r->scheme = base->scheme;
		strcpy(r->scheme_text, base->scheme_text);
		strcpy(r->host, base->host);
		r->port = base->port;

		if (ref->path[0] == '\0') {

			// Empty path: keep the base's path, and take the query
			// only if the reference actually had one.
			//
			// `#frag` alone must NOT drop the base's query string --
			// that is the difference between jumping within the page
			// you are on and reloading a different one. RFC 3986 5.3
			// says so and it is easy to get backwards.
			strcpy(r->path, base->path);
			if (ref->has_query) {
				r->has_query = true;
				strcpy(r->query, ref->query);
			} else {
				r->has_query = base->has_query;
				strcpy(r->query, base->query);
			}

		} else if (ref->path[0] == '/') {

			remove_dot_segments(ref->path, r->path, sizeof(r->path));
			r->has_query = ref->has_query;
			strcpy(r->query, ref->query);

		} else {

			// Merge (RFC 3986 5.3): everything up to and including the
			// base's last '/', then the reference.
			uint32_t mlen = 0;
			const char *slash = strrchr(base->path, '/');

			url_merge[0] = '\0';
			if (slash) {
				uint32_t keep = (uint32_t)(slash - base->path) + 1;
				if (keep >= sizeof(url_merge)) return false;
				memcpy(url_merge, base->path, keep);
				mlen = keep;
				url_merge[mlen] = '\0';
			} else {
				append_ch(url_merge, sizeof(url_merge), &mlen, '/');
			}
			append(url_merge, sizeof(url_merge), &mlen, ref->path);

			remove_dot_segments(url_merge, r->path, sizeof(r->path));
			r->has_query = ref->has_query;
			strcpy(r->query, ref->query);
		}

		strcpy(r->fragment, ref->fragment);
	}

	*out = *r;
	return true;

}

bool url_resolve_str(const url_t *base, const char *text, url_t *out) {
	url_t ref;
	if (!url_parse(text, &ref)) return false;
	return url_resolve(base, &ref, out);
}

// -- format -------------------------------------------------------

static uint16_t default_port(url_scheme_t s) {
	return s == URL_SCHEME_HTTPS ? 443 : 80;
}

uint16_t url_port(const url_t *u) {
	if (u->port) return u->port;
	return default_port(u->scheme);
}

void url_format(const url_t *u, char *out, uint32_t cap, bool with_fragment) {

	uint32_t len = 0;

	if (!out || cap == 0) return;
	out[0] = '\0';
	if (!u) return;

	if (u->scheme == URL_SCHEME_OTHER) {
		append(out, cap, &len, u->scheme_text);
		append_ch(out, cap, &len, ':');
		append(out, cap, &len, u->path);
		return;
	}

	if (u->scheme != URL_SCHEME_NONE) {
		append(out, cap, &len, u->scheme_text);
		append_ch(out, cap, &len, ':');
	}

	if (u->host[0]) {
		append(out, cap, &len, "//");
		append(out, cap, &len, u->host);
		// The port is elided when it is the scheme default, so that
		// url_same_origin() and the URL bar agree with the way every
		// other client in the world writes the same address.
		if (u->port && u->port != default_port(u->scheme)) {
			char pbuf[8];
			snprintf(pbuf, sizeof(pbuf), ":%u", (unsigned)u->port);
			append(out, cap, &len, pbuf);
		}
	}

	append(out, cap, &len, u->path);

	if (u->has_query) {
		append_ch(out, cap, &len, '?');
		append(out, cap, &len, u->query);
	}

	if (with_fragment && u->fragment[0]) {
		append_ch(out, cap, &len, '#');
		append(out, cap, &len, u->fragment);
	}

}

void url_request_target(const url_t *u, char *out, uint32_t cap) {
	uint32_t len = 0;
	if (!out || cap == 0) return;
	out[0] = '\0';
	append(out, cap, &len, u->path[0] ? u->path : "/");
	if (u->has_query) {
		append_ch(out, cap, &len, '?');
		append(out, cap, &len, u->query);
	}
}

bool url_same_origin(const url_t *a, const url_t *b) {
	return a->scheme == b->scheme &&
		!strcmp(a->host, b->host) &&
		url_port(a) == url_port(b);
}

bool url_is_network(const url_t *u) {
	return u->scheme == URL_SCHEME_HTTP || u->scheme == URL_SCHEME_HTTPS;
}
