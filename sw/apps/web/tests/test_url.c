/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host tests for url.c.
 *
 *   cc --std=gnu99 -Wall -Wextra -o test_url tests/test_url.c url.c
 *
 * The resolution cases come from RFC 3986 section 5.4, verbatim,
 * including the abnormal ones. That list is not a formality: it is
 * the set of cases every hand-written resolver gets wrong, and three
 * of them ("../../../g", "http:g", "?y") were wrong here first.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../url.h"

static int fails;
static int checks;

static void ck(bool cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("FAIL: %s\n", what); }
}

static void ck_resolve(const char *base, const char *ref, const char *want) {

	url_t b, out;
	char got[URL_MAX];
	char what[512];

	checks++;

	if (!url_parse(base, &b)) {
		fails++;
		printf("FAIL: base did not parse: %s\n", base);
		return;
	}

	if (!url_resolve_str(&b, ref, &out)) {
		fails++;
		printf("FAIL: resolve failed: %s + %s\n", base, ref);
		return;
	}

	url_format(&out, got, sizeof(got), true);

	if (strcmp(got, want)) {
		fails++;
		snprintf(what, sizeof(what), "%s + %s", base, ref);
		printf("FAIL: %s\n      got  %s\n      want %s\n", what, got, want);
	}

}

static void ck_roundtrip(const char *text) {

	url_t a, b;
	char once[URL_MAX], twice[URL_MAX];

	checks++;

	if (!url_parse(text, &a)) {
		fails++;
		printf("FAIL: did not parse: %s\n", text);
		return;
	}

	url_format(&a, once, sizeof(once), true);

	if (!url_parse(once, &b)) {
		fails++;
		printf("FAIL: formatted form did not re-parse: %s -> %s\n", text, once);
		return;
	}

	url_format(&b, twice, sizeof(twice), true);

	if (strcmp(once, twice)) {
		fails++;
		printf("FAIL: not idempotent: %s -> %s -> %s\n", text, once, twice);
	}

}

int main(void) {

	url_t u, v;
	char buf[URL_MAX];

	// -- parsing --

	ck(url_parse("http://example.com/", &u), "plain http");
	ck(u.scheme == URL_SCHEME_HTTP, "scheme http");
	ck(!strcmp(u.host, "example.com"), "host");
	ck(!strcmp(u.path, "/"), "path");
	ck(url_port(&u) == 80, "default port 80");

	ck(url_parse("HTTPS://Example.COM:8443/A/b?Q=1#F", &u), "mixed case");
	ck(u.scheme == URL_SCHEME_HTTPS, "scheme https");
	ck(!strcmp(u.host, "example.com"), "host lowercased");
	ck(!strcmp(u.path, "/A/b"), "path case preserved");
	ck(!strcmp(u.query, "Q=1"), "query case preserved");
	ck(!strcmp(u.fragment, "F"), "fragment");
	ck(u.port == 8443, "explicit port");

	// No path at all -- what actually goes on the wire is "/".
	ck(url_parse("http://example.com", &u), "no path");
	ck(!strcmp(u.path, "/"), "empty path becomes /");

	// userinfo is discarded, not carried into host. See url.h.
	ck(url_parse("http://user:pw@example.com/x", &u), "userinfo parses");
	ck(!strcmp(u.host, "example.com"), "userinfo discarded");

	// A colon inside an IPv6 literal is not a port separator.
	ck(url_parse("http://[2001:db8::1]:8080/x", &u), "ipv6 with port");
	ck(!strcmp(u.host, "2001:db8::1"), "ipv6 host unbracketed");
	ck(u.port == 8080, "ipv6 port");
	ck(url_parse("http://[::1]/x", &u), "ipv6 no port");
	ck(!strcmp(u.host, "::1"), "ipv6 host");
	ck(url_port(&u) == 80, "ipv6 default port");

	// Things that must be refused.
	ck(!url_parse("http://example.com/\r\nX: y", &u), "reject CRLF in url");
	ck(!url_parse("http://example.com:99999/", &u), "reject port > 65535");
	ck(!url_parse("http://example.com:80x/", &u), "reject non-numeric port");
	ck(!url_parse("http:///path", &u), "reject empty host for http");
	// The empty reference is legal (RFC 3986 5.4.1) and means "this
	// document" -- it must parse, or a bare "#frag" link cannot resolve.
	ck(url_parse("", &u), "empty reference parses");
	ck(u.scheme == URL_SCHEME_NONE, "empty reference is relative");

	// A scheme we cannot follow parses, keeps its text, and is not
	// mistaken for a relative path.
	ck(url_parse("mailto:someone@example.com", &u), "mailto parses");
	ck(u.scheme == URL_SCHEME_OTHER, "mailto is OTHER");
	ck(!strcmp(u.scheme_text, "mailto"), "mailto scheme text");
	ck(url_parse("javascript:void(0)", &u), "javascript: parses");
	ck(u.scheme == URL_SCHEME_OTHER, "javascript is OTHER");

	// A relative reference is not a scheme just because it has a colon
	// later on.
	ck(url_parse("/wiki/C:_(programming_language)", &u), "path with colon");
	ck(u.scheme == URL_SCHEME_NONE, "path with colon has no scheme");

	ck(url_parse("file:/docs/welcome.md", &u), "file url");
	ck(u.scheme == URL_SCHEME_FILE, "file scheme");

	// -- request target: never the fragment --

	ck(url_parse("http://example.com/a/b?c=d#frag", &u), "target base");
	url_request_target(&u, buf, sizeof(buf));
	ck(!strcmp(buf, "/a/b?c=d"), "request target excludes fragment");

	// -- origin --

	ck(url_parse("https://a.example/x", &u), "origin a");
	ck(url_parse("https://a.example:443/y", &v), "origin b");
	ck(url_same_origin(&u, &v), "explicit default port is same origin");
	ck(url_parse("http://a.example/x", &v), "origin c");
	ck(!url_same_origin(&u, &v), "scheme change is a different origin");

	// -- RFC 3986 5.4.1, normal examples --

	{
		const char *base = "http://a/b/c/d;p?q";

		ck_resolve(base, "g:h",     "g:h");
		ck_resolve(base, "g",       "http://a/b/c/g");
		ck_resolve(base, "./g",     "http://a/b/c/g");
		ck_resolve(base, "g/",      "http://a/b/c/g/");
		ck_resolve(base, "/g",      "http://a/g");
		ck_resolve(base, "//g",     "http://g/");
		ck_resolve(base, "?y",      "http://a/b/c/d;p?y");
		ck_resolve(base, "g?y",     "http://a/b/c/g?y");
		ck_resolve(base, "#s",      "http://a/b/c/d;p?q#s");
		ck_resolve(base, "g#s",     "http://a/b/c/g#s");
		ck_resolve(base, "g?y#s",   "http://a/b/c/g?y#s");
		ck_resolve(base, ";x",      "http://a/b/c/;x");
		ck_resolve(base, "g;x",     "http://a/b/c/g;x");
		ck_resolve(base, "g;x?y#s", "http://a/b/c/g;x?y#s");
		ck_resolve(base, "",        "http://a/b/c/d;p?q");
		ck_resolve(base, ".",       "http://a/b/c/");
		ck_resolve(base, "./",      "http://a/b/c/");
		ck_resolve(base, "..",      "http://a/b/");
		ck_resolve(base, "../",     "http://a/b/");
		ck_resolve(base, "../g",    "http://a/b/g");
		ck_resolve(base, "../..",   "http://a/");
		ck_resolve(base, "../../",  "http://a/");
		ck_resolve(base, "../../g", "http://a/g");

		// -- 5.4.2, abnormal examples --
		//
		// The ".." cases that would climb above the root are the ones
		// that matter in practice: a page deep in a site whose CSS
		// link is "../../../../style.css" is not unusual.
		ck_resolve(base, "../../../g",    "http://a/g");
		ck_resolve(base, "../../../../g", "http://a/g");
		ck_resolve(base, "/./g",          "http://a/g");
		ck_resolve(base, "/../g",         "http://a/g");
		ck_resolve(base, "g.",            "http://a/b/c/g.");
		ck_resolve(base, ".g",            "http://a/b/c/.g");
		ck_resolve(base, "g..",           "http://a/b/c/g..");
		ck_resolve(base, "..g",           "http://a/b/c/..g");
		ck_resolve(base, "./../g",        "http://a/b/g");
		ck_resolve(base, "./g/.",         "http://a/b/c/g/");
		ck_resolve(base, "g/./h",         "http://a/b/c/g/h");
		ck_resolve(base, "g/../h",        "http://a/b/c/h");
		ck_resolve(base, "g;x=1/./y",     "http://a/b/c/g;x=1/y");
		ck_resolve(base, "g;x=1/../y",    "http://a/b/c/y");
	}

	// -- resolution against a real-world base --

	ck_resolve("https://en.wikipedia.org/wiki/Fpga", "/wiki/Verilog",
		"https://en.wikipedia.org/wiki/Verilog");
	ck_resolve("https://en.wikipedia.org/wiki/Fpga", "#History",
		"https://en.wikipedia.org/wiki/Fpga#History");
	ck_resolve("https://en.wikipedia.org/w/index.php?title=X&action=raw",
		"?title=Y", "https://en.wikipedia.org/w/index.php?title=Y");

	// A non-default port survives resolution of a relative link. This
	// is the case a lab server on :8080 hits on its first link.
	ck_resolve("http://board.local:8080/a/b", "c",
		"http://board.local:8080/a/c");

	// -- format round-trips --

	ck_roundtrip("http://example.com/");
	ck_roundtrip("https://example.com:8443/a/b?c=d#e");
	ck_roundtrip("http://example.com/a%2Fb");
	ck_roundtrip("https://en.wikipedia.org/wiki/Special:Search?search=fpga");
	ck_roundtrip("mailto:a@b.c");
	ck_roundtrip("file:/docs/welcome.md");

	// Percent-encoding is preserved byte for byte -- decoding and
	// re-encoding would change which resource is requested.
	ck(url_parse("http://x/a%2Fb?q=%20", &u), "percent parse");
	ck(!strcmp(u.path, "/a%2Fb"), "percent path untouched");
	ck(!strcmp(u.query, "q=%20"), "percent query untouched");

	printf("%s: %d checks, %d failures\n",
		fails ? "FAIL" : "ok", checks, fails);

	return fails ? 1 : 0;

}
