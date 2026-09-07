/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See http.h.
 */

#include <string.h>
#include <stdio.h>

#include "http.h"

enum {
	ST_STATUS = 0,		// waiting for "HTTP/1.x nnn ..."
	ST_HEADERS,
	ST_BODY_LEN,		// Content-Length bytes remain
	ST_BODY_CLOSE,		// body ends when the connection does
	ST_BODY_CHUNKED,
	ST_DONE,
	ST_ERROR,
};

enum {
	CH_SIZE = 0,		// reading the hex chunk-size line
	CH_DATA,
	CH_DATA_CRLF,		// the CRLF that follows chunk data
	CH_TRAILER,			// trailer headers after the final 0 chunk
};

static char lc(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool ci_prefix(const char *s, const char *prefix) {
	while (*prefix) {
		if (lc(*s++) != lc(*prefix++)) return false;
	}
	return true;
}

// Is this the start of a header whose value this client acts on?
//
// Only these four change behaviour; everything else is read past. The
// test is on the prefix collected so far, so it can be made before
// the line is complete -- which is the whole point, since the line
// may never fit.
static bool header_matters(const char *prefix) {
	return ci_prefix(prefix, "connection") ||
		ci_prefix(prefix, "content-encoding") ||
		ci_prefix(prefix, "content-type") ||
		ci_prefix(prefix, "content-length") ||
		ci_prefix(prefix, "transfer-encoding") ||
		ci_prefix(prefix, "location");
}

static void fail(http_ctx_t *c, const char *why) {
	if (c->state == ST_ERROR) return;
	c->state = ST_ERROR;
	c->err = why;
}

// -- request building ---------------------------------------------

uint32_t http_build_request(char *out, uint32_t cap, const char *method,
	const url_t *u, const char *accept, bool keep_alive, bool gzip) {

	char target[URL_PATH_MAX * 2];
	char hostport[URL_HOST_MAX + 8];
	uint16_t port;
	int n;

	if (!out || !u || !method) return 0;
	if (!url_is_network(u)) return 0;
	if (!u->host[0]) return 0;

	// url.c rejects control characters at parse time, so a CR or LF
	// cannot reach here in the host or the path. Re-checked anyway,
	// because this is the one place where being wrong about that
	// turns into request splitting -- and the cost of the check is a
	// scan of a string that is about to be copied regardless.
	for (const char *p = u->host; *p; p++)
		if ((unsigned char)*p < 0x20 || *p == 0x7f) return 0;

	url_request_target(u, target, sizeof(target));

	for (const char *p = target; *p; p++)
		if ((unsigned char)*p < 0x20 || *p == 0x7f) return 0;

	port = url_port(u);

	// The Host header carries the port only when it is not the
	// scheme's default. Sending "example.com:80" is legal and a
	// surprising number of virtual-host configurations fail to match
	// on it. Built up front rather than with a conditional inside the
	// format string, because the version that tried to do it with
	// two %s/%u arguments emitted "example.com0" for the common case.
	if (port == (u->scheme == URL_SCHEME_HTTPS ? 443 : 80))
		snprintf(hostport, sizeof(hostport), "%s", u->host);
	else
		snprintf(hostport, sizeof(hostport), "%s:%u", u->host,
			(unsigned)port);

	n = snprintf(out, cap,
		"%s %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"User-Agent: Zeitlos/1.0 (text)\r\n"
		"Accept: %s\r\n"
		// Accept-Encoding is asked for by the CALLER, because it is
		// the caller that owns the 32KB window this needs. Asking for
		// gzip without a decompressor gets a body nothing can read.
		//
		// Connection, either way.
		//
		// This was an unconditional `close`, on the reasoning that
		// `net` has one TCB and a kept-alive connection would hold
		// the only socket in the system open doing nothing.
		//
		// The first half of that has expired: keeping a connection
		// open uses the SAME one TCB, it simply does not close it.
		// The second half is real, and web.c answers it with an idle
		// timer rather than by closing after every request -- because
		// closing costs a full TLS handshake, about fifteen seconds
		// on this hardware, to talk to a server we are already
		// connected to.
		"%s"
		"Connection: %s\r\n"
		"\r\n",
		method, target, hostport,
		accept ? accept : "text/html, text/plain, */*",
		gzip ? "Accept-Encoding: gzip\r\n" : "",
		keep_alive ? "keep-alive" : "close");

	if (n < 0 || (uint32_t)n >= cap) return 0;

	return (uint32_t)n;

}

// -- header parsing ------------------------------------------------

static void parse_status(http_ctx_t *c, const char *line) {

	const char *p = line;
	uint32_t code = 0;

	if (!ci_prefix(p, "http/")) { fail(c, "not an HTTP response"); return; }
	p += 5;

	// "1.1", "1.0", and tolerate "1" -- the version is not acted on,
	// only the framing rules are, and those are decided by the
	// headers rather than by this number.
	while (*p && *p != ' ') p++;
	while (*p == ' ') p++;

	if (!is_digit(p[0]) || !is_digit(p[1]) || !is_digit(p[2])) {
		fail(c, "malformed status line");
		return;
	}

	code = (uint32_t)(p[0] - '0') * 100 + (uint32_t)(p[1] - '0') * 10 +
		(uint32_t)(p[2] - '0');

	c->res.status = (uint16_t)code;

}

// Copies a header value with leading whitespace trimmed.
static void copy_value(char *dst, uint32_t cap, const char *src) {
	while (*src == ' ' || *src == '\t') src++;
	uint32_t n = 0;
	while (src[n] && n + 1 < cap) { dst[n] = src[n]; n++; }
	dst[n] = '\0';
	// Trailing whitespace too -- a value with a trailing space in a
	// Location turns into a URL with a space in it.
	while (n && (dst[n - 1] == ' ' || dst[n - 1] == '\t')) dst[--n] = '\0';
}

// The server's Connection header.
//
// HTTP/1.1 defaults to persistent, so the token that matters is
// `close`. A 1.0 response defaults the other way, and this client
// only ever speaks 1.1, so anything claiming 1.0 is treated as
// closing whatever it says.
static void parse_connection(http_ctx_t *c, const char *value) {
	if (ci_prefix(value, "close")) c->res.keep_alive = false;
}

static void parse_content_type(http_ctx_t *c, const char *value) {

	const char *semi;
	uint32_t n = 0;

	// The type itself, up to the first ';'.
	semi = value;
	while (*semi && *semi != ';') semi++;

	while (value + n < semi && n + 1 < HTTP_CTYPE_MAX &&
		value[n] != ' ' && value[n] != '\t') {
		c->res.content_type[n] = lc(value[n]);
		n++;
	}
	c->res.content_type[n] = '\0';

	// charset=..., the one parameter acted on. Everything else
	// (boundary, format, delsp) is ignored.
	{
		const char *p = semi;
		while (*p) {
			while (*p == ';' || *p == ' ' || *p == '\t') p++;
			if (ci_prefix(p, "charset=")) {
				uint32_t k = 0;
				p += 8;
				if (*p == '"') p++;
				while (*p && *p != ';' && *p != '"' && *p != ' ' &&
					k + 1 < HTTP_CHARSET_MAX)
					c->res.charset[k++] = lc(*p++);
				c->res.charset[k] = '\0';
				return;
			}
			while (*p && *p != ';') p++;
		}
	}

}

static void parse_header(http_ctx_t *c, char *line) {

	char *colon = strchr(line, ':');
	char *value;

	if (!colon) return;			// not a header; ignore rather than fail

	*colon = '\0';
	value = colon + 1;

	if (ci_prefix(line, "content-type") && !line[12]) {

		parse_content_type(c, value + strspn(value, " \t"));

	} else if (ci_prefix(line, "content-encoding") && !line[16]) {

		const char *v = value + strspn(value, " \t");

		// gzip and x-gzip are the same thing; `identity` and an
		// absent header both mean uncompressed.
		//
		// Anything ELSE -- deflate, br, zstd -- is not decoded, and
		// is left for headers_done() to refuse. Passing an unknown
		// encoding through as if it were text would render a page of
		// binary, which looks like a parser bug rather than a missing
		// feature.
		if (ci_prefix(v, "gzip") || ci_prefix(v, "x-gzip"))
			c->res.gzip = true;
		else if (!ci_prefix(v, "identity") && v[0])
			c->res.gzip = true, c->enc_unknown = true;

	} else if (ci_prefix(line, "connection") && !line[10]) {

		parse_connection(c, value + strspn(value, " \t"));

	} else if (ci_prefix(line, "content-length") && !line[14]) {

		char buf[24];
		uint32_t v = 0;
		const char *p;

		copy_value(buf, sizeof(buf), value);

		for (p = buf; *p; p++) {
			if (!is_digit(*p)) { fail(c, "bad Content-Length"); return; }
			// Overflow check before the multiply, not after: a length
			// of 2^32 is not a large file, it is a lie, and wrapping
			// it produces a small number that looks reasonable.
			if (v > (0xFFFFFFFFu - (uint32_t)(*p - '0')) / 10) {
				fail(c, "Content-Length too large");
				return;
			}
			v = v * 10 + (uint32_t)(*p - '0');
		}

		if (p == buf) { fail(c, "empty Content-Length"); return; }

		// Two Content-Length headers that disagree is the classic
		// request-smuggling setup. Nothing downstream of this browser
		// can be desynchronised by it, but a response whose own length
		// is ambiguous is not one to guess about.
		if (c->n_lengths && c->res.length != v) {
			fail(c, "conflicting Content-Length headers");
			return;
		}

		c->res.length = v;
		c->res.has_length = true;
		c->n_lengths++;

	} else if (ci_prefix(line, "transfer-encoding") && !line[17]) {

		char buf[64];
		copy_value(buf, sizeof(buf), value);
		for (char *p = buf; *p; p++) *p = lc(*p);
		// "chunked" must be the LAST encoding applied, and it is the
		// only one this client can undo. Anything else -- gzip,
		// deflate, a stack of them -- is a body it cannot read, so it
		// fails here rather than handing compressed bytes to the HTML
		// parser and rendering line noise.
		if (strstr(buf, "chunked")) c->res.chunked = true;
		else if (buf[0]) fail(c, "unsupported Transfer-Encoding");

	} else if (ci_prefix(line, "location") && !line[8]) {

		copy_value(c->res.location, sizeof(c->res.location), value);
		c->res.has_location = c->res.location[0] != '\0';

	}

	*colon = ':';

}

// Hands body bytes to the caller, decompressing on the way when the
// server compressed them.
//
// Every path that produces body bytes -- length-delimited, chunked,
// and delimited-by-close -- goes through here, so gzip works the same
// on all three. Content-Length describes the COMPRESSED size, which
// is what the framing above counts; the caller sees only plaintext
// and never learns which it was.
static void emit_body(http_ctx_t *c, const char *data, uint32_t n) {

	if (!n) return;

	if (!c->inf_active) {
		if (c->on_body) c->on_body(c->user, data, n);
		return;
	}

	{
		const uint8_t *p = (const uint8_t *)data;
		uint32_t left = n;

		while (left) {

			uint8_t out[512];
			uint32_t il = left, ol = sizeof(out);
			int rv = z_inflate(&c->inf, p, &il, out, &ol);

			if (ol && c->on_body) c->on_body(c->user, (const char *)out, ol);

			p += il;
			left -= il;

			if (rv == Z_INFLATE_DONE) {
				// Trailing bytes after the stream end are the
				// server's business, not ours; the framing above
				// decides when the response is over.
				c->inf_active = false;
				return;
			}

			if (rv != Z_INFLATE_OK) {
				fail(c, z_inflate_strerror(rv));
				return;
			}

			// No progress with input left means the decoder wants
			// output room it already has -- impossible, but a loop
			// that could spin on it is not worth leaving.
			if (il == 0 && ol == 0) return;

		}
	}

}

// The header block ended; decide how the body is framed.
static void headers_done(http_ctx_t *c) {

	// RFC 7230 3.3.3: when both are present, Transfer-Encoding wins
	// and Content-Length MUST be ignored. Both being present at all
	// is how a smuggling attack is built, so the length is not merely
	// ignored, it is erased -- leaving it set would let a later
	// change accidentally consult it.
	if (c->res.chunked && c->res.has_length) {
		c->res.has_length = false;
		c->res.length = 0;
	}

	// A connection can only be reused if this response is
	// SELF-DELIMITING.
	//
	// Without a Content-Length or chunked framing, the body ends when
	// the connection does -- so there is nothing to reuse, and
	// pretending otherwise would leave the next request reading the
	// tail of this body as its status line. That is not a subtle
	// failure mode, it is response smuggling against oneself.
	//
	// A HEAD response and the bodiless statuses below are
	// self-delimiting by definition and keep whatever the server
	// said.
	if (!c->res.chunked && !c->res.has_length &&
		!c->head_request && c->res.status != 204 && c->res.status != 304)
		c->res.keep_alive = false;

	// Compression, decided before any body byte is handled.
	if (c->res.gzip) {
		if (c->enc_unknown) {
			fail(c, "the server used a content encoding this build "
				"cannot decode");
			return;
		}
		if (!c->inf_window) {
			// Only reachable if a server ignores our headers: we do
			// not advertise gzip without a window.
			fail(c, "the server sent a compressed body and this build "
				"has no decompressor");
			return;
		}
		// The limit is the caller's spool budget, not a guess about
		// the content: a small compressed body that expands without
		// bound is a decompression bomb, and there is no honest page
		// on the other side of one.
		z_inflate_init(&c->inf, c->inf_window, Z_INFLATE_GZIP,
			HTTP_MAX_BODY);
		c->inf_active = true;
	}

	if (c->on_headers) c->on_headers(c->user, &c->res);

	// Responses that never have a body, whatever their headers claim.
	// 204 and 304 in particular routinely carry a Content-Length
	// describing the body they are NOT sending, and waiting for it
	// hangs the connection until it times out.
	if (c->head_request || c->res.status == 204 || c->res.status == 304 ||
		(c->res.status >= 100 && c->res.status < 200)) {
		c->state = ST_DONE;
		return;
	}

	if (c->res.chunked) {
		c->state = ST_BODY_CHUNKED;
		c->chunk_state = CH_SIZE;
		c->line_len = 0;
		return;
	}

	if (c->res.has_length) {
		c->remaining = c->res.length;
		c->state = (c->remaining == 0) ? ST_DONE : ST_BODY_LEN;
		return;
	}

	// No length and no chunking: the body is everything until the
	// connection closes. This is legal, common on older servers, and
	// indistinguishable from a truncated transfer -- which is exactly
	// why `Connection: close` is sent and the length, when offered,
	// is preferred.
	c->state = ST_BODY_CLOSE;

}

// -- line assembly -------------------------------------------------
//
// Returns true when a complete line is in c->line (NUL terminated,
// without its terminator). Accepts a bare LF as well as CRLF: a
// client parsing a response is not at risk from the desynchronisation
// that makes bare LF dangerous on the request side, and there are
// still servers and proxies that emit it.
static bool feed_line(http_ctx_t *c, char ch) {

	if (ch == '\n') {
		if (c->line_len && c->line[c->line_len - 1] == '\r') c->line_len--;
		c->line[c->line_len] = '\0';
		return true;
	}

	if (c->line_len + 1 >= HTTP_LINE_MAX) {
		c->line_overflow = true;
		return false;
	}

	c->line[c->line_len++] = ch;
	return false;

}

// -- public --------------------------------------------------------

void http_set_inflate(http_ctx_t *c, uint8_t *window) {
	c->inf_window = window;
}

void http_init(http_ctx_t *c, bool head,
	http_headers_fn on_headers, http_body_fn on_body, void *user) {

	memset(c, 0, sizeof(*c));

	// AFTER the memset, obviously -- but it was written before it,
	// and every "this response is reusable" test failed as a result.
	//
	// HTTP/1.1 is persistent by default. Retracted by the server
	// saying `close`, and by headers_done() when the response turns
	// out not to be self-delimiting.
	c->res.keep_alive = true;

	c->on_headers = on_headers;
	c->on_body = on_body;
	c->user = user;
	c->head_request = head;
	c->state = ST_STATUS;
}

void http_feed(http_ctx_t *c, const char *data, uint32_t len) {

	uint32_t i = 0;

	while (i < len && c->state != ST_DONE && c->state != ST_ERROR) {

		switch (c->state) {

		case ST_STATUS:
		case ST_HEADERS:

			c->header_bytes++;
			if (c->header_bytes > HTTP_HEADERS_MAX) {
				fail(c, "header block too large");
				return;
			}

			if (c->skip_line) {
				// Reading past a header that is too long to keep and
				// not one we act on.
				if (data[i++] == '\n') {
					c->skip_line = false;
					c->line_len = 0;
					c->line_overflow = false;
				}
				break;
			}

			if (!feed_line(c, data[i++])) {
				if (c->line_overflow) {
					c->line[c->line_len] = '\0';
					if (header_matters(c->line)) {
						// Truncating one of these would change what
						// is fetched or how the body is framed.
						fail(c, "header line too long");
						return;
					}
					// Not a header this client reads. Skip the rest
					// of the line and carry on -- refusing the whole
					// response over a Content-Security-Policy would
					// be absurd.
					c->skip_line = true;
				}
				break;
			}

			if (c->state == ST_STATUS) {
				parse_status(c, c->line);
				c->line_len = 0;
				if (c->state != ST_ERROR) c->state = ST_HEADERS;
				break;
			}

			if (c->line_len == 0) {
				// Blank line: end of the header block.
				//
				// A 1xx response is informational and is followed by
				// ANOTHER complete response. "100 Continue" arrives
				// unsolicited from some servers; treating it as the
				// real response leaves the actual one unparsed.
				if (c->res.status >= 100 && c->res.status < 200) {
					uint16_t was = c->res.status;
					memset(&c->res, 0, sizeof(c->res));
					(void)was;
					c->state = ST_STATUS;
					c->line_len = 0;
					break;
				}
				headers_done(c);
				c->line_len = 0;
				break;
			}

			parse_header(c, c->line);
			c->line_len = 0;
			break;

		case ST_BODY_LEN: {
			uint32_t n = len - i;
			if (n > c->remaining) n = c->remaining;
			emit_body(c, data + i, n);
			i += n;
			c->remaining -= n;
			if (c->remaining == 0) c->state = ST_DONE;
			break;
		}

		case ST_BODY_CLOSE: {
			uint32_t n = len - i;
			emit_body(c, data + i, n);
			i += n;
			break;
		}

		case ST_BODY_CHUNKED:

			switch (c->chunk_state) {

			case CH_SIZE:
				if (!feed_line(c, data[i++])) {
					if (c->line_overflow) { fail(c, "chunk size line too long"); return; }
					break;
				}
				{
					uint32_t v = 0;
					const char *p = c->line;
					int digits = 0;

					// A blank line here is the CRLF left over from a
					// previous chunk on a server that framed it
					// slightly differently; skip it rather than
					// failing.
					if (c->line_len == 0) { c->line_len = 0; break; }

					for (; *p; p++) {
						char h = lc(*p);
						uint32_t d;
						if (h >= '0' && h <= '9') d = (uint32_t)(h - '0');
						else if (h >= 'a' && h <= 'f') d = (uint32_t)(h - 'a' + 10);
						else break;		// ';' begins chunk extensions
						if (v > (0xFFFFFFFFu - d) / 16) {
							fail(c, "chunk size too large");
							return;
						}
						v = v * 16 + d;
						digits++;
					}

					if (!digits) { fail(c, "malformed chunk size"); return; }

					c->line_len = 0;
					c->remaining = v;

					if (v == 0) {
						// Final chunk. Trailers may follow, ending at
						// a blank line.
						c->chunk_state = CH_TRAILER;
					} else {
						c->chunk_state = CH_DATA;
					}
				}
				break;

			case CH_DATA: {
				uint32_t n = len - i;
				if (n > c->remaining) n = c->remaining;
				emit_body(c, data + i, n);
				i += n;
				c->remaining -= n;
				if (c->remaining == 0) {
					c->chunk_state = CH_DATA_CRLF;
					c->line_len = 0;
				}
				break;
			}

			case CH_DATA_CRLF:
				if (feed_line(c, data[i++])) {
					c->line_len = 0;
					c->chunk_state = CH_SIZE;
				}
				c->line_overflow = false;
				break;

			case CH_TRAILER:
				if (feed_line(c, data[i++])) {
					bool blank = (c->line_len == 0);
					c->line_len = 0;
					if (blank) c->state = ST_DONE;
				}
				c->line_overflow = false;
				break;

			}
			break;

		default:
			i++;
			break;

		}

	}

}

void http_eof(http_ctx_t *c) {

	switch (c->state) {

	case ST_BODY_CLOSE:
		// This is how such a response is supposed to end.
		c->state = ST_DONE;
		break;

	case ST_DONE:
	case ST_ERROR:
		break;

	case ST_STATUS:
		fail(c, "connection closed before any response");
		break;

	case ST_BODY_LEN:
		// A short body is a truncation, and it has to be reported:
		// silently accepting one renders half a page as though it
		// were the whole page.
		fail(c, "connection closed with body incomplete");
		break;

	default:
		fail(c, "connection closed mid-response");
		break;

	}

}

bool http_done(const http_ctx_t *c) { return c->state == ST_DONE; }

const char *http_error(const http_ctx_t *c) {
	return (c->state == ST_ERROR) ? (c->err ? c->err : "http error") : NULL;
}
