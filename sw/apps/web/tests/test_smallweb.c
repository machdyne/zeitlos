/*
 * Zeitlos -- host tests for smallweb.c (Gopher and Gemini).
 *
 *   make test          (part of the web suite)
 *
 * Requests byte for byte, the Gemini header, and the HTML each kind of
 * body becomes. Every conversion is also run with the input fed ONE
 * BYTE AT A TIME, because on the device a body arrives in whatever
 * pieces TCP and TLS produce, and a converter that only works on whole
 * lines works here and fails there.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../smallweb.h"

static int checks, fails;
#define CK(c, what) do { checks++; if (!(c)) { fails++; \
	printf("FAIL %d: %s\n", __LINE__, what); } } while (0)

static char outbuf[131072];
static uint32_t outlen;

static void sink(void *user, const char *d, uint32_t n) {
	(void)user;
	if (outlen + n < sizeof(outbuf)) { memcpy(outbuf + outlen, d, n); outlen += n; }
	outbuf[outlen] = 0;
}

// Converts `in` for `url`, whole and then byte by byte; the two must
// agree. Returns the HTML (in outbuf) and leaves the context in *c.
static const char *convert(const char *url, const char *in, sw_ctx_t *c) {
	static char whole[16384];
	url_t u;
	if (!url_parse(url, &u)) { printf("bad test url %s\n", url); exit(2); }

	outlen = 0; outbuf[0] = 0;
	sw_begin(c, &u, sink, NULL);
	sw_feed(c, in, (uint32_t)strlen(in));
	sw_end(c);
	strcpy(whole, outbuf);

	outlen = 0; outbuf[0] = 0;
	sw_begin(c, &u, sink, NULL);
	for (const char *p = in; *p; p++) sw_feed(c, p, 1);
	sw_end(c);
	CK(!strcmp(whole, outbuf), "byte-at-a-time output matches whole");

	return outbuf;
}

static bool has(const char *hay, const char *needle) {
	if (!strstr(hay, needle)) { printf("  missing: %s\n", needle); return false; }
	return true;
}

static void req(const char *url, const char *want) {
	url_t u;
	char out[1200];
	uint32_t n;
	CK(url_parse(url, &u), url);
	n = sw_request(&u, out, sizeof(out));
	out[n] = 0;
	checks++;
	if (strcmp(out, want)) {
		fails++;
		printf("FAIL request for %s: got [%s] want [%s]\n", url, out, want);
	}
}

int main(void) {

	sw_ctx_t c;
	const char *h;
	url_t u;

	// -- URLs and ports --
	CK(url_parse("gopher://example.org/", &u) && u.scheme == URL_SCHEME_GOPHER &&
		url_port(&u) == 70 && url_is_network(&u), "gopher: port 70, network");
	CK(url_parse("gemini://example.org", &u) && u.scheme == URL_SCHEME_GEMINI &&
		url_port(&u) == 1965 && !strcmp(u.path, "/"), "gemini: port 1965, path /");
	{
		url_t base, out;
		char buf[URL_MAX];
		url_parse("gemini://example.org/a/b.gmi", &base);
		CK(url_resolve_str(&base, "c.gmi", &out), "gemini: relative link resolves");
		url_format(&out, buf, sizeof(buf), false);
		CK(!strcmp(buf, "gemini://example.org/a/c.gmi"), "gemini: resolved against the base");
		CK(url_resolve_str(&base, "/top.gmi", &out), "gemini: absolute path");
		url_format(&out, buf, sizeof(buf), false);
		CK(!strcmp(buf, "gemini://example.org/top.gmi"), "gemini: absolute path resolved");
	}

	// -- requests --
	req("gemini://example.org/", "gemini://example.org/\r\n");
	req("gemini://example.org:1966/x.gmi?q#frag", "gemini://example.org:1966/x.gmi?q\r\n");
	req("gemini://example.org/x?hello w\xc3\xb6rld", "gemini://example.org/x?hello%20w%C3%B6rld\r\n");
	req("gopher://example.org", "\r\n");
	req("gopher://example.org/", "\r\n");
	req("gopher://example.org/1/phlog", "/phlog\r\n");
	req("gopher://example.org/0/docs/read%20me.txt", "/docs/read me.txt\r\n");
	req("gopher://example.org/7/search?zeitlos%20fpga", "/search\tzeitlos fpga\r\n");
	CK(url_parse("gopher://h/0/x", &u) && sw_gopher_type(&u) == '0', "type from the path");
	CK(url_parse("gopher://h/", &u) && sw_gopher_type(&u) == '1', "root is a menu");
	CK(url_parse("http://h/", &u) && sw_request(&u, (char[8]){0}, 8) == 0, "not ours: 0");

	// -- the Gemini header --
	h = convert("gemini://h/", "20 text/gemini; lang=en\r\n# Hi\r\n", &c);
	CK(c.header_done && c.status == 20 && sw_status_class(&c) == 2, "status 20");
	CK(!strcmp(c.meta, "text/gemini; lang=en"), "meta kept whole");
	CK(c.body == SW_BODY_GEMTEXT, "text/gemini is gemtext");
	CK(has(h, "<h1>Hi</h1>"), "heading");

	h = convert("gemini://h/", "20\r\nplain", &c);
	CK(c.body == SW_BODY_GEMTEXT, "empty meta defaults to text/gemini");

	h = convert("gemini://h/", "31 gemini://h/new.gmi\r\n", &c);
	CK(sw_status_class(&c) == 3 && !strcmp(c.meta, "gemini://h/new.gmi"), "redirect + target");
	CK(c.body == SW_BODY_DISCARD && outlen == 0, "a redirect has no document");

	h = convert("gemini://h/", "10 Your name?\r\n", &c);
	CK(sw_status_class(&c) == 1 && !strcmp(c.meta, "Your name?"), "input prompt");

	h = convert("gemini://h/", "51 Not found\r\n", &c);
	CK(sw_status_class(&c) == 5, "not found");

	h = convert("gemini://h/", "20 text/plain\r\na < b\r\n", &c);
	CK(c.body == SW_BODY_TEXT && has(h, "<pre>a &lt; b\n</pre>"), "text/plain in <pre>, escaped");

	h = convert("gemini://h/", "20 image/png\r\n\x89PNG", &c);
	CK(c.body == SW_BODY_DISCARD && outlen == 0, "an image is not shown as text");

	h = convert("gemini://h/", "garbage\r\n", &c);
	CK(c.header_done && c.status == -1 && sw_status_class(&c) == 0, "a bad header is no status");

	// -- gemtext --
	h = convert("gemini://h/",
		"20 text/gemini\r\n"
		"# Title\r\n"
		"## Sub\r\n"
		"### Subsub\r\n"
		"First line\r\n"
		"second line of the same paragraph\r\n"
		"\r\n"
		"New paragraph with <tags> & \"quotes\"\r\n"
		"=> gemini://other/ Somewhere else\r\n"
		"=>   /local.gmi\r\n"
		"=>\r\n"
		"* one\r\n"
		"* two\r\n"
		"> quoted\r\n"
		"```ascii art\r\n"
		"  # not a heading\r\n"
		"=> not a link\r\n"
		"```\r\n"
		"after", &c);
	CK(has(h, "<h1>Title</h1>") && has(h, "<h2>Sub</h2>") && has(h, "<h3>Subsub</h3>"), "headings");
	CK(has(h, "<p>First line<br>second line of the same paragraph</p>"), "consecutive lines, one paragraph");
	CK(has(h, "<p>New paragraph with &lt;tags&gt; &amp; &quot;quotes&quot;</p>"), "escaped text");
	CK(has(h, "<a href=\"gemini://other/\">Somewhere else</a>"), "link with a label");
	CK(has(h, "<a href=\"/local.gmi\">/local.gmi</a>"), "link without a label shows the URL");
	CK(!strstr(h, "href=\"\""), "a bare => is not a link");
	CK(has(h, "<ul><li>one</li>\n<li>two</li>\n</ul>"), "one list for consecutive items");
	CK(has(h, "<blockquote>quoted</blockquote>"), "quote");
	CK(has(h, "<pre>  # not a heading\n=&gt; not a link\n</pre>"), "preformatted is literal");
	CK(has(h, "<p>after</p>"), "last line without a newline");
	CK(has(h, "</body></html>"), "document closed");

	// An unterminated ``` block and an open list are closed at the end.
	h = convert("gemini://h/", "20 text/gemini\r\n* a\r\n```\r\nx", &c);
	CK(has(h, "</ul>") && has(h, "<pre>x\n</pre>"), "open elements closed at end");

	// -- Gopher menus --
	h = convert("gopher://example.org/",
		"iWelcome to <this> hole\t\terror.host\t1\r\n"
		"1Phlog\t/phlog\texample.org\t70\r\n"
		"0Read me\t/docs/read me.txt\texample.org\t70\r\n"
		"7Search\t/search\tsearch.example\t7070\r\n"
		"hA web page\tURL:https://example.com/\texample.org\t70\r\n"
		"9A binary\t/x.bin\texample.org\t70\r\n"
		"3Something failed\t\terror.host\t1\r\n"
		"not a menu line at all\r\n"
		".\r\n"
		"1After the end\t/x\th\t70\r\n", &c);
	CK(has(h, "<pre>"), "menu in <pre>");
	CK(has(h, "      Welcome to &lt;this&gt; hole\n"), "info line, escaped, aligned");
	CK(has(h, "[dir] <a href=\"gopher://example.org/1/phlog\">Phlog</a>"), "menu link, default port elided");
	CK(has(h, "[txt] <a href=\"gopher://example.org/0/docs/read%20me.txt\">Read me</a>"), "selector percent-encoded");
	CK(has(h, "[ ? ] <a href=\"gopher://search.example:7070/7/search\">Search</a>"), "search, other port");
	CK(has(h, "[www] <a href=\"https://example.com/\">A web page</a>"), "URL: selector is a web link");
	CK(has(h, "[bin] A binary\n") && !strstr(h, "x.bin"), "binary shown, not linked");
	CK(has(h, "      Something failed\n"), "error line as text");
	CK(has(h, "      not a menu line at all\n"), "malformed line kept as text");
	CK(!strstr(h, "After the end"), "nothing after the '.' line");

	// And the link text survives a round trip: the href a menu wrote
	// parses back to a URL whose request is the original selector.
	req("gopher://example.org/0/docs/read%20me.txt", "/docs/read me.txt\r\n");

	// -- Gopher text --
	h = convert("gopher://h/0/f.txt", "line <1>\r\n..dotted\r\n.\r\nignored\r\n", &c);
	CK(has(h, "<pre>line &lt;1&gt;\n.dotted\n</pre>"), "text: escaped, .. unstuffed, ends at .");
	CK(!strstr(h, "ignored"), "text: nothing after the '.'");

	// -- Gopher HTML item passes through; an image is discarded --
	h = convert("gopher://h/h/page.html", "<p>hi</p>", &c);
	CK(!strcmp(h, "<p>hi</p>"), "type h passed through untouched");
	h = convert("gopher://h/I/pic.gif", "GIF89a", &c);
	CK(c.body == SW_BODY_DISCARD && outlen == 0, "type I discarded");

	// -- a blank line after a heading or a quote --
	//
	// The line buffer is reused, so on an empty line l[0] is what the
	// PREVIOUS line started with. The heading and quote branches used to
	// look at it before the blank-line check, then take `n - 1` of n = 0:
	// four billion bytes of whatever followed the buffer, into the page.
	// skyjake.fi found it; the cases above never had a blank line right
	// after a heading.
	{
		static const char *docs[] = {
			"20 text/gemini\r\n# Title\r\n\r\nText\r\n",
			"20 text/gemini\n## Sub\n\n### Subsub\n\nend\n",
			"20 text/gemini\r\n> quoted\r\n\r\nafter\r\n",
			"20 text/gemini\r\n# A\r\n\r\n\r\n> b\r\n\r\n",
		};
		for (unsigned i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
			h = convert("gemini://h/", docs[i], &c);
			CK(outlen < 64 * strlen(docs[i]), "a blank line after a heading or quote stays small");
		}
		h = convert("gemini://h/", "20 text/gemini\r\n# Title\r\n\r\nText\r\n", &c);
		CK(!strcmp(h, "<html><head><meta charset=\"utf-8\"></head><body>\n"
			"<h1>Title</h1>\n<p>Text</p>\n</body></html>\n"), "heading, blank, text: exactly");
	}

	// -- fuzz: every line type, blank lines, CR/LF, long lines, junk --
	//
	// Output is bounded by a small multiple of the input (the markup
	// around each line); a converter that runs away cannot stay inside
	// that. Whole and byte-at-a-time must agree. Under -fsanitize=address
	// any read past a line is an immediate failure, which is what would
	// have caught the blank-line bug on the first run.
	{
		static char doc[8192];
		static char whole[65536];
		static const char *starts[] = { "# ", "## ", "### ", "#", "> ", ">",
			"* ", "*", "=> ", "=>", "```", "", "text ", "i", "1", "0", "." };
		uint32_t seed = 12345;
		int bad_bound = 0, bad_split = 0;
		for (int round = 0; round < 3000; round++) {
			int n = 0;
			bool gemini = round & 1;
			if (gemini) n += sprintf(doc, "20 text/gemini\r\n");
			int lines = 1 + (int)((seed = seed * 1103515245u + 12345u) >> 16) % 40;
			for (int l = 0; l < lines && n < (int)sizeof(doc) - 1600; l++) {
				seed = seed * 1103515245u + 12345u;
				const char *st = starts[(seed >> 16) % (sizeof(starts) / sizeof(*starts))];
				n += sprintf(doc + n, "%s", st);
				seed = seed * 1103515245u + 12345u;
				int len = (int)((seed >> 16) % ((seed & 64) ? 1400 : 30));
				for (int k = 0; k < len; k++) {
					seed = seed * 1103515245u + 12345u;
					char ch = (char)(32 + (seed >> 16) % 95);
					if (((seed >> 8) & 31) == 0) ch = '\t';
					if (((seed >> 9) & 63) == 1) ch = (char)(0x80 + ((seed >> 16) & 0x7f));
					doc[n++] = ch;
				}
				doc[n++] = (seed & 1) ? '\n' : '\r';
				if (doc[n - 1] == '\r') doc[n++] = '\n';
				if (((seed >> 3) & 7) == 0) { doc[n++] = '\n'; }		// a blank line
			}
			doc[n] = 0;
			url_t u;
			url_parse(gemini ? "gemini://h/" : "gopher://h/", &u);
			outlen = 0; outbuf[0] = 0;
			sw_begin(&c, &u, sink, NULL);
			sw_feed(&c, doc, (uint32_t)n);
			sw_end(&c);
			if (outlen > 8u * (uint32_t)n + 256u) bad_bound++;
			memcpy(whole, outbuf, outlen + 1);
			uint32_t wl = outlen;
			outlen = 0; outbuf[0] = 0;
			sw_begin(&c, &u, sink, NULL);
			for (int k = 0; k < n; ) {
				seed = seed * 1103515245u + 12345u;
				int step = 1 + (int)((seed >> 16) % 700);
				if (step > n - k) step = n - k;
				sw_feed(&c, doc + k, (uint32_t)step);
				k += step;
			}
			sw_end(&c);
			if (outlen != wl || memcmp(whole, outbuf, wl)) bad_split++;
		}
		CK(bad_bound == 0, "fuzz: output stays within 8x the input");
		CK(bad_split == 0, "fuzz: split input converts the same as whole");
		if (bad_bound || bad_split)
			printf("  fuzz: %d over the bound, %d split mismatches\n", bad_bound, bad_split);
	}

	printf("test_smallweb: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;

}
