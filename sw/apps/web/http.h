#ifndef HTTP_H
#define HTTP_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * An HTTP/1.1 client: build a request, feed in whatever bytes the
 * socket produced, get headers and body out.
 *
 * -- no transport in here --
 *
 * This file does not open a socket, does not know what TLS is, and
 * includes nothing from Zeitlos. Bytes go in through http_feed() and
 * come out through two callbacks. That is what lets tests/test_http.c
 * run the whole thing on the build machine against hand-written
 * responses -- including the malformed ones, which are the ones worth
 * testing and the ones a real server will not produce on demand.
 *
 * The layering matters for a second reason: the same parser sits
 * behind plain TCP and behind TLS. If it knew which, there would be
 * two paths through it and only one of them would get exercised.
 *
 * -- scope --
 *
 * HTTP/1.1 requests, GET and HEAD. Chunked transfer-encoding,
 * Content-Length, and read-until-close, which are the three ways a
 * response body can end. Redirects are PARSED here (Location is
 * reported) and FOLLOWED a layer up, because following one is a
 * policy decision -- how many, and whether https may become http --
 * and policy does not belong in a parser.
 *
 * Not here, deliberately:
 *
 *   - **No compression.** A response is not asked for gzip, so it
 *     does not arrive gzipped. That costs bandwidth on a link that
 *     has little to spare and it saves an inflate implementation plus
 *     a decompression buffer, which is the right trade until Phase 5.
 *   - **No cookies, no authentication, no caching headers.** Nothing
 *     here has a user to be logged in as.
 *   - **No connection reuse yet.** One request per connection, and
 *     `Connection: close` is sent. `net` has one TCB, so a kept-alive
 *     connection would hold the only socket in the system open doing
 *     nothing.
 *   - **No HTTP/2 or /3.** Both need TLS ALPN and a different framing
 *     layer entirely; every server that speaks them still speaks 1.1.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zinflate.h"

#include "url.h"

// Longest single header line KEPT.
//
// A line longer than this is not automatically an error. Whether it
// matters depends entirely on WHICH header it is: a truncated
// Location or Content-Length is a different header and acting on one
// is worse than failing, but a 4KB Content-Security-Policy is
// something this browser never reads at all.
//
// So an over-long line is skipped if its NAME is one that is ignored,
// and is a hard error only if it is one that gets acted on. The name
// arrives first, which is what makes that decidable in a streaming
// parser -- see http.c.
//
// Real headers are the reason: en.wikipedia.org sends a
// Content-Security-Policy well past 1024 bytes, and at 1024 with no
// skip this parser refused the page outright.
#define HTTP_LINE_MAX     2048

// Ceiling on a DECOMPRESSED body.
//
// The defence against a decompression bomb: a few hundred kilobytes
// of gzip can legitimately expand to gigabytes, and there is no
// honest page on the other side of that. 8MB is far above any real
// document and far below anything that could exhaust the card.
#define HTTP_MAX_BODY     (8u * 1024u * 1024u)

// Total header block accepted, across all lines. Bounded because the
// header block arrives before anything can be validated and a server
// -- or something between here and the server -- can send it forever.
#define HTTP_HEADERS_MAX  16384

#define HTTP_CTYPE_MAX    64
#define HTTP_CHARSET_MAX  32

typedef struct {

	// The three-digit status. 0 before the status line has arrived.
	uint16_t	status;

	// Lowercased, with any parameters stripped: "text/html", not
	// "text/html; charset=UTF-8". The charset is separated out below
	// because it is the one parameter this browser acts on.
	char		content_type[HTTP_CTYPE_MAX];
	char		charset[HTTP_CHARSET_MAX];

	// The Location header, verbatim and undecoded. May be relative;
	// resolving it against the request URL is the caller's job
	// (url_resolve()).
	char		location[URL_MAX];
	bool		has_location;

	// Body length, when the server said. `has_length` is false for a
	// chunked response and for one that ends at connection close --
	// in both cases the length is genuinely not known in advance and
	// a zero here means nothing.
	uint32_t	length;
	bool		has_length;

	bool		chunked;

	// Whether this connection may carry another request.
	//
	// True only when the response is SELF-DELIMITING -- a
	// Content-Length or chunked encoding -- and the server did not
	// say `Connection: close`. A response that ends at connection
	// close obviously cannot be followed by anything, and one whose
	// end cannot be found would leave the next request reading the
	// tail of this body as its status line.
	bool		keep_alive;

	// The server compressed the body. Decoded before it reaches
	// on_body, so a caller never sees a gzip stream -- see
	// http_set_inflate().
	bool		gzip;

} http_response_t;

// Called once, when the header block is complete. `r` is valid for
// the duration of the call.
typedef void (*http_headers_fn)(void *user, const http_response_t *r);

// Called repeatedly with decoded body bytes -- de-chunked, so the
// caller never sees a chunk header. Same borrowed-buffer lifetime.
typedef void (*http_body_fn)(void *user, const char *data, uint32_t len);

typedef struct {

	uint8_t				state;
	uint8_t				chunk_state;

	http_headers_fn		on_headers;
	http_body_fn		on_body;
	void				*user;

	http_response_t		res;

	char				line[HTTP_LINE_MAX];
	uint32_t			line_len;
	bool				line_overflow;
	bool				skip_line;

	uint32_t			header_bytes;

	// Bytes of body still expected: the Content-Length remainder, or
	// the current chunk's remainder.
	uint32_t			remaining;

	// How many Content-Length headers were seen. More than one that
	// disagree is a hard error -- see http.c.
	uint8_t				n_lengths;

	bool				head_request;

	// gzip decoding, when the caller supplied a window.
	// An encoding we recognise as compressed but cannot decode --
	// brotli, zstd. Refused in headers_done() rather than passed
	// through as text.
	bool				enc_unknown;

	uint8_t				*inf_window;
	z_inflate_t			inf;
	bool				inf_active;

	const char			*err;

} http_ctx_t;

// Builds a request into `out`. Returns the number of bytes written,
// or 0 if it did not fit or the URL is not something that can be
// requested.
//
// `accept` may be NULL for a sensible default.
//
// The request-target never includes the fragment, and the Host header
// includes the port only when it is not the scheme default -- both of
// which are the difference between a request that works everywhere
// and one that works on most servers.
// `keep_alive` asks the server to leave the connection open. The
// caller must be prepared for a `Connection: close` answer anyway --
// that is always the server's right, and http_response_t.keep_alive
// is where the answer lands.
// `gzip` asks for a compressed body. Only pass true when
// http_set_inflate() has been given a window -- asking for gzip
// without a decompressor gets a body nothing can read.
uint32_t http_build_request(char *out, uint32_t cap, const char *method,
	const url_t *u, const char *accept, bool keep_alive, bool gzip);

// `head` must be true when the request method was HEAD, because a
// HEAD response carries a Content-Length describing a body that will
// never arrive. Without knowing, the parser waits for it forever.
// Enables `Content-Encoding: gzip`.
//
// `window` must be Z_INFLATE_WINDOW bytes and outlive the exchange.
// The buffer is the caller's because it is 32KB and not every caller
// wants to spend that -- see zinflate.h.
//
// Without this, http.c does not advertise gzip and a server that
// sends it anyway is a hard error rather than a page of binary. With
// it, en.wikipedia.org's 258KB front page arrives as about 60KB.
void http_set_inflate(http_ctx_t *c, uint8_t *window);

void http_init(http_ctx_t *c, bool head,
	http_headers_fn on_headers, http_body_fn on_body, void *user);

void http_feed(http_ctx_t *c, const char *data, uint32_t len);

// The peer closed the connection. For a response with no
// Content-Length and no chunking, this is what ends the body
// successfully; anywhere else it is a truncation and becomes an
// error.
void http_eof(http_ctx_t *c);

// True once the whole response has been delivered.
bool http_done(const http_ctx_t *c);

// NULL unless something went wrong, in which case a short reason
// suitable for showing to a person.
const char *http_error(const http_ctx_t *c);

#endif
