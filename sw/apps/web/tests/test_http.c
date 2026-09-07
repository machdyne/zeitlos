/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host tests for http.c.
 *
 *   cc --std=gnu99 -Wall -Wextra -o test_http tests/test_http.c \
 *      http.c url.c
 *
 * Most of what is checked here is malformed input. That is not
 * pessimism about servers, it is where the bugs are: a well-formed
 * 200 with a Content-Length works on the first try, and everything
 * that has ever gone wrong in an HTTP client is in the other cases --
 * a body that ends at connection close, a chunk boundary that lands
 * mid-header, a 204 that claims a length, two Content-Lengths that
 * disagree.
 *
 * The chunking invariant is the same one test_html.c uses and for the
 * same reason: every response here is fed at several chunk sizes, and
 * all of them must produce identical output. A parser that works on
 * whole responses and breaks on 1-byte feeds is a parser that works
 * in this file and fails on hardware, where the transport delivers
 * whatever a 536-byte segment happened to contain.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../http.h"
#include "../url.h"

static int fails, checks;

static void ck(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("FAIL: %s\n", what); }
}

// -- collecting a parse ---------------------------------------------

typedef struct {
	char			body[65536];
	uint32_t		blen;
	http_response_t	res;
	bool			got_headers;
	int				nheader_calls;
} sink_t;

static void on_headers(void *user, const http_response_t *r) {
	sink_t *s = (sink_t *)user;
	s->res = *r;
	s->got_headers = true;
	s->nheader_calls++;
}

static void on_body(void *user, const char *data, uint32_t len) {
	sink_t *s = (sink_t *)user;
	if (s->blen + len < sizeof(s->body)) {
		memcpy(s->body + s->blen, data, len);
		s->blen += len;
		s->body[s->blen] = '\0';
	}
}

// Feeds `resp` in chunks of `chunk` bytes, then signals EOF.
static const char *parse_at(const char *resp, uint32_t len, uint32_t chunk,
	bool head, sink_t *out) {

	http_ctx_t c;

	memset(out, 0, sizeof(*out));
	http_init(&c, head, on_headers, on_body, out);

	for (uint32_t i = 0; i < len; i += chunk) {
		uint32_t n = len - i;
		if (n > chunk) n = chunk;
		http_feed(&c, resp + i, n);
	}

	http_eof(&c);

	return http_error(&c);

}

// The whole-response parse, plus the invariant that every chunk size
// agrees with it.
// A gzip stream built here rather than linked against zlib: the test
// suite should not need a compression library to check that the
// decompressor works, and a STORED deflate block is a legal gzip
// stream that exercises the whole wrapper.
static uint32_t fake_gzip(uint8_t *out, uint32_t cap,
	const char *data, uint32_t n) {

	uint32_t o = 0;

	if (cap < n + 24) return 0;

	out[o++] = 0x1f; out[o++] = 0x8b; out[o++] = 8;
	out[o++] = 0;								// no flags
	out[o++] = 0; out[o++] = 0; out[o++] = 0; out[o++] = 0;	// mtime
	out[o++] = 0; out[o++] = 3;					// XFL, OS

	out[o++] = 0x01;							// final, stored
	out[o++] = (uint8_t)(n & 0xff);
	out[o++] = (uint8_t)(n >> 8);
	out[o++] = (uint8_t)(~n & 0xff);
	out[o++] = (uint8_t)((~n >> 8) & 0xff);
	memcpy(out + o, data, n); o += n;

	// CRC and size: consumed, not verified -- see docs/http.md.
	for (int i = 0; i < 8; i++) out[o++] = 0;

	return o;

}

// Like parse(), but with a decompression window attached.
static const char *parse_gz(const char *resp, uint32_t len,
	uint8_t *win, sink_t *out) {

	http_ctx_t c;

	memset(out, 0, sizeof(*out));
	http_init(&c, false, on_headers, on_body, out);
	http_set_inflate(&c, win);

	// Fed in small pieces, because that is how a socket delivers.
	for (uint32_t i = 0; i < len; ) {
		uint32_t n = len - i > 13 ? 13 : len - i;
		http_feed(&c, resp + i, n);
		i += n;
	}
	http_eof(&c);

	return http_error(&c);

}

static const char *parse(const char *resp, sink_t *out) {

	static sink_t alt;
	static const uint32_t sizes[] = { 1, 3, 17, 512 };
	uint32_t len = (uint32_t)strlen(resp);
	const char *err = parse_at(resp, len, len ? len : 1, false, out);

	for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
		const char *e2 = parse_at(resp, len, sizes[k], false, &alt);
		checks++;
		if (alt.blen != out->blen || memcmp(alt.body, out->body, alt.blen) ||
			alt.res.status != out->res.status ||
			((err == NULL) != (e2 == NULL))) {
			fails++;
			printf("FAIL: chunk size %u differs from whole-response\n",
				(unsigned)sizes[k]);
		}
	}

	return err;

}

static void ck_body(const sink_t *s, const char *want, const char *what) {
	checks++;
	if (s->blen != strlen(want) || memcmp(s->body, want, s->blen)) {
		fails++;
		printf("FAIL: %s: body is \"%.60s\" (%u bytes), wanted \"%.60s\"\n",
			what, s->body, (unsigned)s->blen, want);
	}
}

int main(void) {

	sink_t s;
	const char *err;

	// -- request building --

	{
		url_t u;
		char req[2048];
		uint32_t n;

		ck(url_parse("http://example.com/a/b?c=d#frag", &u), "req url parses");
		n = http_build_request(req, sizeof(req), "GET", &u, NULL, false, false);
		ck(n > 0, "request built");
		ck(strstr(req, "GET /a/b?c=d HTTP/1.1\r\n") == req,
			"request line, query included");
		// The fragment is never sent. It is a client-side concept and
		// a server has no business seeing it.
		ck(!strstr(req, "frag"), "fragment not sent");
		ck(strstr(req, "Host: example.com\r\n") != NULL,
			"default port elided from Host");
		ck(strstr(req, "Connection: close\r\n") != NULL, "connection close");

		ck(url_parse("https://example.com:8443/x", &u), "req url with port");
		n = http_build_request(req, sizeof(req), "GET", &u, NULL, false, false);
		ck(n > 0 && strstr(req, "Host: example.com:8443\r\n") != NULL,
			"non-default port kept in Host");

		ck(url_parse("https://example.com/x", &u), "https default port");
		http_build_request(req, sizeof(req), "GET", &u, NULL, false, false);
		ck(strstr(req, "Host: example.com\r\n") != NULL,
			"443 elided for https");

		// A URL with no path still asks for "/".
		ck(url_parse("http://example.com", &u), "bare host");
		http_build_request(req, sizeof(req), "GET", &u, NULL, false, false);
		ck(strstr(req, "GET / HTTP/1.1") == req, "empty path becomes /");

		// Nothing that is not http/https can be requested.
		ck(url_parse("mailto:a@b.c", &u), "mailto parses");
		ck(http_build_request(req, sizeof(req), "GET", &u, NULL, false, false) == 0,
			"mailto is not requestable");

		// A buffer too small fails rather than truncating -- a
		// truncated request is a different request.
		ck(url_parse("http://example.com/", &u), "small buf url");
		ck(http_build_request(req, 20, "GET", &u, NULL, false, false) == 0,
			"too-small buffer refuses");
	}

	// -- a plain response --

	err = parse(
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/html; charset=UTF-8\r\n"
		"Content-Length: 11\r\n"
		"\r\n"
		"hello world", &s);
	ck(!err, "plain 200 parses");
	ck(s.res.status == 200, "status 200");
	ck(!strcmp(s.res.content_type, "text/html"), "content type, params stripped");
	ck(!strcmp(s.res.charset, "utf-8"), "charset extracted and lowercased");
	ck(s.res.has_length && s.res.length == 11, "content length");
	ck_body(&s, "hello world", "plain 200");

	// Header names are case-insensitive, and bare LF is tolerated --
	// there are still proxies that emit it.
	err = parse("HTTP/1.0 200 OK\ncOnTeNt-LeNgTh: 3\n\nabc", &s);
	ck(!err, "bare LF and mixed case parse");
	ck(s.res.length == 3, "case-insensitive header name");
	ck_body(&s, "abc", "bare LF");

	// -- body that ends at connection close --

	err = parse("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nsome text", &s);
	ck(!err, "close-delimited body is not an error");
	ck(!s.res.has_length, "no length reported when there is none");
	ck_body(&s, "some text", "close-delimited");

	// -- a truncated Content-Length body IS an error --
	//
	// Silently accepting one renders half a page as though it were
	// the whole page, which is the failure a reader cannot detect.
	err = parse("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort", &s);
	ck(err != NULL, "short body is reported");
	ck_body(&s, "short", "short body still delivered");

	// -- chunked --

	err = parse(
		"HTTP/1.1 200 OK\r\n"
		"Transfer-Encoding: chunked\r\n"
		"\r\n"
		"5\r\nhello\r\n"
		"1\r\n \r\n"
		"5\r\nworld\r\n"
		"0\r\n\r\n", &s);
	ck(!err, "chunked parses");
	ck(s.res.chunked, "chunked flagged");
	ck_body(&s, "hello world", "chunked body reassembled");

	// Chunk extensions are ignored, and hex is case-insensitive.
	err = parse(
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
		"A;name=value\r\n0123456789\r\n"
		"b\r\nabcdefghijk\r\n"
		"0\r\n\r\n", &s);
	ck(!err, "chunk extensions and uppercase hex");
	ck_body(&s, "0123456789abcdefghijk", "chunk extension body");

	// Trailers after the final chunk.
	err = parse(
		"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
		"3\r\nabc\r\n"
		"0\r\n"
		"X-Checksum: deadbeef\r\n"
		"\r\n", &s);
	ck(!err, "trailers after final chunk");
	ck_body(&s, "abc", "trailer body");

	// A chunked response cut off before its terminator is a
	// truncation.
	err = parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
		"5\r\nhello\r\n", &s);
	ck(err != NULL, "unterminated chunked body reported");

	// -- Transfer-Encoding wins over Content-Length --
	//
	// RFC 7230 3.3.3. Both being present is also how request
	// smuggling is built, so the length is erased rather than merely
	// deprioritised.
	err = parse(
		"HTTP/1.1 200 OK\r\n"
		"Content-Length: 3\r\n"
		"Transfer-Encoding: chunked\r\n"
		"\r\n"
		"5\r\nhello\r\n0\r\n\r\n", &s);
	ck(!err, "TE + CL parses");
	ck(!s.res.has_length, "content length erased when chunked");
	ck_body(&s, "hello", "chunked wins over content-length");

	// Two Content-Lengths that agree are fine; two that disagree are
	// not something to guess about.
	err = parse("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n"
		"Content-Length: 3\r\n\r\nabc", &s);
	ck(!err, "duplicate identical Content-Length accepted");
	err = parse("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n"
		"Content-Length: 9\r\n\r\nabc", &s);
	ck(err != NULL, "conflicting Content-Length rejected");

	// A compressed body cannot be read, so it fails loudly rather
	// than handing line noise to the HTML parser.
	err = parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nxx", &s);
	ck(err != NULL, "unsupported transfer-encoding rejected");

	// -- responses with no body, whatever they claim --

	err = parse("HTTP/1.1 204 No Content\r\nContent-Length: 42\r\n\r\n", &s);
	ck(!err, "204 with a bogus length does not hang");
	ck(s.res.status == 204 && s.blen == 0, "204 has no body");

	err = parse("HTTP/1.1 304 Not Modified\r\nContent-Length: 99\r\n\r\n", &s);
	ck(!err && s.blen == 0, "304 has no body");

	// HEAD is the case the parser cannot infer -- the Content-Length
	// is real and describes a body that will never be sent.
	{
		const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n";
		err = parse_at(resp, (uint32_t)strlen(resp), 512, true, &s);
		ck(!err, "HEAD response does not wait for a body");
		ck(s.res.length == 1234 && s.blen == 0, "HEAD reports length, no body");
	}

	// A 1xx is informational and is followed by the real response.
	err = parse("HTTP/1.1 100 Continue\r\n\r\n"
		"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", &s);
	ck(!err, "100 Continue then the real response");
	ck(s.res.status == 200, "final status is the real one");
	ck_body(&s, "hi", "body after 100 Continue");

	// -- redirects: parsed here, followed elsewhere --

	err = parse("HTTP/1.1 301 Moved\r\nLocation: /new/place\r\n"
		"Content-Length: 0\r\n\r\n", &s);
	ck(!err, "redirect parses");
	ck(s.res.status == 301, "redirect status");
	ck(s.res.has_location && !strcmp(s.res.location, "/new/place"),
		"location captured");

	// A Location with trailing whitespace becomes a URL with a space
	// in it if the value is not trimmed.
	err = parse("HTTP/1.1 302 Found\r\nLocation: http://x/y  \r\n"
		"Content-Length: 0\r\n\r\n", &s);
	ck(!strcmp(s.res.location, "http://x/y"), "location trimmed");

	// And it resolves against the request URL, which is the whole
	// reason it is reported verbatim.
	{
		url_t base, dest;
		ck(url_parse("http://example.com/a/b/c", &base), "redirect base");
		ck(url_resolve_str(&base, "/new/place", &dest), "redirect resolves");
		{
			char buf[URL_MAX];
			url_format(&dest, buf, sizeof(buf), false);
			ck(!strcmp(buf, "http://example.com/new/place"),
				"relative redirect resolved");
		}
	}

	// -- malformed --

	err = parse("not http at all\r\n\r\n", &s);
	ck(err != NULL, "non-HTTP response rejected");

	err = parse("HTTP/1.1 2x0 OK\r\n\r\n", &s);
	ck(err != NULL, "non-numeric status rejected");

	err = parse("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n", &s);
	ck(err != NULL, "non-numeric Content-Length rejected");

	// A length that does not fit in 32 bits is not a large file, it
	// is a lie -- and wrapping it produces a small number that looks
	// entirely reasonable.
	err = parse("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999\r\n\r\n", &s);
	ck(err != NULL, "overflowing Content-Length rejected");

	err = parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
		"zz\r\nabc\r\n", &s);
	ck(err != NULL, "malformed chunk size rejected");

	// An empty response, and one that stops mid-headers.
	err = parse("", &s);
	ck(err != NULL, "empty response reported");
	err = parse("HTTP/1.1 200 OK\r\nContent-Ty", &s);
	ck(err != NULL, "truncated header block reported");

	// An over-long header this client never reads is SKIPPED, not
	// refused. en.wikipedia.org sends a Content-Security-Policy well
	// past 1024 bytes, and refusing the whole page over it -- which
	// is what the first version did -- is absurd.
	{
		static char big[HTTP_LINE_MAX * 3];
		int n = sprintf(big, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
			"Content-Security-Policy: ");
		for (uint32_t k = 0; k < HTTP_LINE_MAX + 200; k++) big[n++] = 'a';
		n += sprintf(big + n, "\r\nContent-Type: text/html\r\n\r\nhi");
		big[n] = '\0';
		err = parse(big, &s);
		ck(!err, "over-long ignorable header is skipped");
		ck(s.res.status == 200, "status survives a skipped header");
		ck(!strcmp(s.res.content_type, "text/html"),
			"headers AFTER a skipped one are still parsed");
		ck_body(&s, "hi", "body after a skipped header");
	}

	// An over-long header that IS acted on stays a hard error: a
	// truncated Location is a different Location.
	{
		static char big[HTTP_LINE_MAX * 3];
		int n = sprintf(big, "HTTP/1.1 302 Found\r\nLocation: http://x/");
		for (uint32_t k = 0; k < HTTP_LINE_MAX + 200; k++) big[n++] = 'a';
		n += sprintf(big + n, "\r\n\r\n");
		big[n] = '\0';
		err = parse(big, &s);
		ck(err != NULL, "over-long Location is refused, not truncated");
	}

	// Headers that never end.
	{
		static char big[HTTP_HEADERS_MAX + 2048];
		int n = sprintf(big, "HTTP/1.1 200 OK\r\n");
		while (n < HTTP_HEADERS_MAX + 1000) n += sprintf(big + n, "X-Pad: y\r\n");
		big[n] = '\0';
		err = parse(big, &s);
		ck(err != NULL, "unbounded header block refused");
	}

	// Headers arriving one byte at a time, which is what a bad
	// network actually looks like.
	{
		const char *r = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
		err = parse_at(r, (uint32_t)strlen(r), 1, false, &s);
		ck(!err && s.blen == 5, "one byte at a time");
	}

	// -- connection reuse --
	//
	// keep_alive is what web.c uses to decide whether the socket can
	// carry another request. Getting it wrong in the permissive
	// direction is the dangerous one: the next request would read
	// the tail of this body as its status line, which is response
	// smuggling against oneself.
	{
		err = parse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi", &s);
		ck(!err && s.res.keep_alive,
			"length-delimited response is reusable");

		err = parse("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
			"Connection: close\r\n\r\nhi", &s);
		ck(!err && !s.res.keep_alive, "server saying close is honoured");

		err = parse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
			"\r\n2\r\nhi\r\n0\r\n\r\n", &s);
		ck(!err && s.res.keep_alive, "chunked response is reusable");

		// The one that matters: no framing at all. The body ends
		// when the connection does, so there is nothing to reuse.
		err = parse("HTTP/1.1 200 OK\r\n\r\nhi", &s);
		ck(!s.res.keep_alive,
			"response delimited by close is NOT reusable");

		err = parse("HTTP/1.1 204 No Content\r\n\r\n", &s);
		ck(!err && s.res.keep_alive, "204 with no body is reusable");
	}

	// -- gzip --
	//
	// The whole point of the exercise: a compressed body must reach
	// on_body as plaintext, identically to an uncompressed one, on
	// every framing path. Content-Length describes the COMPRESSED
	// size, so getting this wrong truncates pages rather than
	// failing.
	{
		static uint8_t win[Z_INFLATE_WINDOW];
		static char resp[8192];
		const char *body = "<html><body><p>hello hello hello</p></body></html>";
		uint8_t gz[512];
		uint32_t gzn;

		gzn = fake_gzip(gz, sizeof(gz), body, (uint32_t)strlen(body));

		// Length-delimited.
		{
			int n = snprintf(resp, sizeof(resp),
				"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
				"Content-Encoding: gzip\r\nContent-Length: %u\r\n\r\n",
				(unsigned)gzn);
			memcpy(resp + n, gz, gzn);
			err = parse_gz(resp, (uint32_t)n + gzn, win, &s);
			ck(!err, "a gzip body decodes");
			ck_body(&s, body, "and matches the original exactly");
		}

		// Chunked, in awkward pieces: de-chunking happens first, so
		// the decoder sees the compressed stream split at boundaries
		// that have nothing to do with its own structure.
		{
			int n = snprintf(resp, sizeof(resp),
				"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
				"Transfer-Encoding: chunked\r\n\r\n");
			uint32_t off = 0;
			while (off < gzn) {
				uint32_t take = gzn - off > 7 ? 7 : gzn - off;
				n += snprintf(resp + n, sizeof(resp) - n, "%x\r\n", (unsigned)take);
				memcpy(resp + n, gz + off, take); n += (int)take;
				n += snprintf(resp + n, sizeof(resp) - n, "\r\n");
				off += take;
			}
			n += snprintf(resp + n, sizeof(resp) - n, "0\r\n\r\n");
			err = parse_gz(resp, (uint32_t)n, win, &s);
			ck(!err, "a chunked gzip body decodes");
			ck_body(&s, body, "chunked and compressed still matches");
		}

		// An encoding we cannot decode must FAIL, not be handed to
		// the renderer as if it were text.
		{
			int n = snprintf(resp, sizeof(resp),
				"HTTP/1.1 200 OK\r\nContent-Encoding: br\r\n"
				"Content-Length: 2\r\n\r\nhi");
			err = parse_gz(resp, (uint32_t)n, win, &s);
			ck(err != NULL, "an unknown content encoding is refused");
		}

		// identity is not compression.
		{
			int n = snprintf(resp, sizeof(resp),
				"HTTP/1.1 200 OK\r\nContent-Encoding: identity\r\n"
				"Content-Length: 2\r\n\r\nhi");
			err = parse_gz(resp, (uint32_t)n, win, &s);
			ck(!err, "identity is accepted");
			ck_body(&s, "hi", "identity passes through unchanged");
		}

		// Corrupt compressed data must fail rather than emit garbage.
		//
		// The byte flipped is LEN in the stored-block header, not a
		// payload byte: LEN/~LEN is the ONE integrity check DEFLATE
		// gives for free, and the trailing CRC is consumed rather
		// than verified (docs/http.md). Flipping payload would change
		// the text and be caught by nothing, which is a property of
		// the format rather than a bug in the decoder -- and a test
		// that pretended otherwise would be asserting something
		// untrue.
		{
			int n;
			gz[11] ^= 0xff;			// LEN, low byte
			n = snprintf(resp, sizeof(resp),
				"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n"
				"Content-Length: %u\r\n\r\n", (unsigned)gzn);
			memcpy(resp + n, gz, gzn);
			err = parse_gz(resp, (uint32_t)n + gzn, win, &s);
			ck(err != NULL, "a corrupt gzip body is refused");
		}
	}

	printf("%s: %d checks, %d failures\n",
		fails ? "FAIL" : "ok", checks, fails);

	return fails ? 1 : 0;

}
