#ifndef URL_H
#define URL_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * URL parsing and relative resolution. No networking, no files, no
 * Zeitlos types -- so it builds and runs on the host, which is the
 * only way to have any confidence in something that is almost
 * entirely edge cases (tests/test_url.c).
 *
 * -- scope --
 *
 * RFC 3986 section 5 resolution, restricted to the three schemes this
 * browser can act on: http, https and file. Everything else parses
 * cleanly and is reported as URL_SCHEME_OTHER, so the renderer can
 * still draw a `mailto:` link as a link and say why it will not
 * follow it, rather than mangling it into a relative path and
 * fetching something absurd.
 *
 * Deliberately NOT handled:
 *
 *   - userinfo (`http://user:pass@host/`). Parsed and DISCARDED
 *     rather than passed on. It is a phishing vector far more often
 *     than it is a credential, there is no authentication in this
 *     browser for it to feed, and silently keeping it in the host
 *     field would mean connecting somewhere other than what the URL
 *     bar shows.
 *   - Internationalised domain names. Punycode encoding is a table
 *     and an algorithm for a case that cannot be typed on this
 *     machine anyway -- the fonts are ASCII (sw/common/zfont_data.c:
 *     every font is 0x20..0x7f). A host with a non-ASCII byte in it
 *     is rejected.
 *   - Percent-DECODING. Paths are kept exactly as written and sent
 *     exactly as written. Decoding then re-encoding is how a `%2F` in
 *     a path turns into a `/` and changes which resource is being
 *     asked for.
 *
 * -- normalisation, and the one place it matters --
 *
 * url_resolve() removes dot segments (RFC 3986 5.2.4), because a
 * relative link of `../../foo` is extremely common and a server
 * asked for a literal `/a/b/../../foo` will often 404. It does NOT
 * lowercase the path, collapse duplicate slashes, or reorder query
 * parameters: all three change what is being requested.
 *
 * The host IS lowercased, and the default port IS elided, because
 * both of those are genuinely case- and form-insensitive per the
 * spec, and because url_same_origin() has to be able to compare two
 * of these.
 */

#include <stdint.h>
#include <stdbool.h>

// Longest URL handled anywhere in this browser.
//
// 1024 rather than the 2048 a desktop browser uses, and the reason is
// memory rather than taste: sw/apps/web keeps a history ring and a
// per-page link table, and every URL_MAX in a struct is multiplied by
// however many of those there are. Long URLs are truncated at parse
// time and reported as an error rather than silently cut, because a
// truncated URL is a DIFFERENT URL and fetching it is worse than
// refusing.
#define URL_MAX      1024
#define URL_HOST_MAX 256
#define URL_PATH_MAX 512

typedef enum {
	URL_SCHEME_NONE = 0,	// relative reference, no scheme
	URL_SCHEME_HTTP,
	URL_SCHEME_HTTPS,
	URL_SCHEME_FILE,
	URL_SCHEME_OTHER,		// mailto:, ftp:, javascript:, ...
} url_scheme_t;

typedef struct {

	url_scheme_t	scheme;

	// The scheme exactly as written, lowercased, without the ':'.
	// Kept even for URL_SCHEME_OTHER so an error message can name it
	// ("cannot follow a mailto: link") rather than being generic.
	char			scheme_text[16];

	// Lowercased, no userinfo, no brackets on an IPv6 literal.
	char			host[URL_HOST_MAX];

	// 0 means "the scheme's default" -- 80 for http, 443 for https.
	// url_port() resolves that; this field stays 0 so that
	// url_format() can leave the port out again.
	uint16_t		port;

	// Always begins with '/' for a hierarchical URL. A URL with an
	// empty path ("http://example.com") gets "/" here, since that is
	// what has to go on the wire.
	char			path[URL_PATH_MAX];

	// Without the '?' / '#'. A fragment is kept because it is what
	// an in-page anchor jump needs, and dropped from every request
	// (it is not sent to a server, ever).
	char			query[URL_PATH_MAX];
	char			fragment[256];

	bool			has_query;

} url_t;

// Parses an absolute or relative reference. Returns false on
// something that cannot be a URL at all -- longer than URL_MAX, a
// non-ASCII byte in the host, a port that is not a number or does not
// fit in 16 bits, a control character anywhere.
//
// A relative reference ("/wiki/Foo", "../x", "?q=1") parses
// successfully with scheme == URL_SCHEME_NONE and whatever components
// were present; url_resolve() is what turns that into something
// fetchable.
bool url_parse(const char *text, url_t *out);

// RFC 3986 section 5.2: resolve `ref` against `base`.
//
// `base` must be absolute (a scheme and, for http/https, a host);
// returns false if it is not. `ref` may be absolute, in which case
// this is very nearly a copy -- the one thing it still does is remove
// dot segments, which a hand-typed absolute URL can contain.
//
// out may alias base or ref.
bool url_resolve(const url_t *base, const url_t *ref, url_t *out);

// Convenience: parse `text` and resolve it against `base` in one
// step. This is what a link click calls.
bool url_resolve_str(const url_t *base, const char *text, url_t *out);

// Rebuilds the URL as a string, into a buffer of at least URL_MAX.
// Elides the port when it is the scheme default, and the fragment
// when `with_fragment` is false.
//
// Round-trips: url_parse(url_format(u)) == u, for anything url_parse
// accepted. tests/test_url.c checks that on every case, because a
// formatter that disagrees with the parser is how the URL bar ends up
// showing something other than what was fetched.
void url_format(const url_t *u, char *out, uint32_t cap, bool with_fragment);

// The request-target for an HTTP request line: path, plus '?' and the
// query if there is one. Never the fragment.
void url_request_target(const url_t *u, char *out, uint32_t cap);

// 80, 443, or the explicit port if one was given.
uint16_t url_port(const url_t *u);

// Same scheme, host and effective port. Used for the one security
// decision Phase 1 makes: a redirect that changes origin is followed,
// but a redirect from https to http is NOT (see http.h).
bool url_same_origin(const url_t *a, const url_t *b);

// true for http and https -- the schemes that need `net`.
bool url_is_network(const url_t *u);

#endif
