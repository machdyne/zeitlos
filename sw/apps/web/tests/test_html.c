/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host tests for html.c and uni.c.
 *
 *   cc --std=gnu99 -Wall -Wextra -o test_html tests/test_html.c \
 *      html.c uni.c
 *   ./test_html                    unit cases plus invariants
 *   ./test_html page.html ...      also run the invariants over real
 *                                  saved pages
 *
 * The second form is the one that finds things, exactly as
 * sw/apps/read's md_test.c found things by being pointed at the real
 * docs/ directory. Real HTML contains constructs nobody thinks to
 * write a case for.
 *
 * -- the three invariants --
 *
 * 1. CHUNK INDEPENDENCE. Feeding a document in chunks of 1, 7, 512
 *    and "all at once" must produce identical output. This is what
 *    catches state that lives in a local variable instead of the
 *    context, and it is not hypothetical: a 512-byte chunk boundary
 *    is what the real transport delivers
 *    (ZSTREAM_CHUNK_SIZE_DEFAULT).
 *
 * 2. RESUME. Parsing from any mark, with the state that mark
 *    reported, must produce exactly the tail of the full parse. This
 *    is the property the whole scrolling design rests on -- if it is
 *    false, scrolling backwards shows different text than scrolling
 *    forwards did, which is the worst kind of bug to debug on
 *    hardware.
 *
 * 3. TERMINATION AND BOUNDS. Every block is NUL terminated within
 *    HTML_LINE_MAX, every span lies inside its block's text, and
 *    every span's link index is inside the block's link table.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../html.h"
#include "../uni.h"

static int fails, checks;

static void ck(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("FAIL: %s\n", what); }
}

// -- collecting the emitted blocks into one comparable string ------

typedef struct {
	char	buf[512 * 1024];
	uint32_t len;
	int		n;
} sink_t;

static const char *kind_name(html_kind_t k) {
	switch (k) {
	case HTML_BLANK:   return "BLANK";
	case HTML_PARA:    return "PARA";
	case HTML_HEADING: return "HEAD";
	case HTML_PRE:     return "PRE";
	case HTML_QUOTE:   return "QUOTE";
	case HTML_LIST:    return "LIST";
	case HTML_RULE:    return "RULE";
	case HTML_TABLE:   return "TABLE";
	case HTML_IMAGE:   return "IMG";
	}
	return "?";
}

static void sink_emit(void *user, const html_line_t *l) {

	sink_t *s = (sink_t *)user;
	int n;

	// Bounds, checked on every block of every document rather than in
	// one place -- invariant 3.
	if (l->len >= HTML_LINE_MAX) { fails++; printf("FAIL: block len overruns\n"); }
	if (l->text[l->len] != '\0') { fails++; printf("FAIL: block not terminated\n"); }
	if (l->nspans > HTML_MAX_SPANS) { fails++; printf("FAIL: too many spans\n"); }
	if (l->nlinks > HTML_MAX_LINKS) { fails++; printf("FAIL: too many links\n"); }

	for (int i = 0; i < l->nspans; i++) {
		const html_span_t *sp = &l->spans[i];
		if ((uint32_t)sp->start + sp->len > l->len) {
			fails++;
			printf("FAIL: span %d runs past the block text\n", i);
		}
		if (sp->kind == HTML_SPAN_LINK && sp->link >= l->nlinks) {
			fails++;
			printf("FAIL: span %d links to entry %u of %u\n",
				i, (unsigned)sp->link, (unsigned)l->nlinks);
		}
	}

	if (s->len + HTML_LINE_MAX + 256 >= sizeof(s->buf)) return;

	n = snprintf(s->buf + s->len, sizeof(s->buf) - s->len,
		"%s/%u%s%s [%s]", kind_name(l->kind), (unsigned)l->level,
		l->marker[0] ? " " : "", l->marker, l->text);
	s->len += (uint32_t)n;

	for (int i = 0; i < l->nlinks; i++) {
		n = snprintf(s->buf + s->len, sizeof(s->buf) - s->len,
			" ->%s", l->links[i]);
		s->len += (uint32_t)n;
	}

	s->buf[s->len++] = '\n';
	s->buf[s->len] = '\0';
	s->n++;

}

static void parse_chunked(const char *doc, uint32_t len, uint32_t chunk,
	sink_t *out) {

	html_ctx_t ctx;

	memset(out, 0, sizeof(*out));
	html_init(&ctx, sink_emit, out);

	for (uint32_t i = 0; i < len; i += chunk) {
		uint32_t n = len - i;
		if (n > chunk) n = chunk;
		html_feed(&ctx, doc + i, n);
	}

	html_finish(&ctx);

}

// Captures raw blocks, for the fields render() does not print.
static html_line_t cap_blocks[16];
static int cap_n;

static void cap_emit(void *user, const html_line_t *l) {
	(void)user;
	if (cap_n < 16) cap_blocks[cap_n++] = *l;
}

static int parse_blocks(const char *doc, html_line_t *out, int max) {
	html_ctx_t ctx;
	int i, n;
	cap_n = 0;
	html_init(&ctx, cap_emit, NULL);
	html_feed(&ctx, doc, (uint32_t)strlen(doc));
	html_finish(&ctx);
	n = cap_n < max ? cap_n : max;
	for (i = 0; i < n; i++) out[i] = cap_blocks[i];
	return n;
}

static const char *render(const char *doc) {
	static sink_t s;
	parse_chunked(doc, (uint32_t)strlen(doc), (uint32_t)strlen(doc) + 1, &s);
	return s.buf;
}

static void ck_html(const char *doc, const char *want) {
	const char *got = render(doc);
	checks++;
	if (strcmp(got, want)) {
		fails++;
		printf("FAIL: %s\n  got:  %s  want: %s\n", doc, got, want);
	}
}

// -- invariant 1: chunk independence ------------------------------

static void ck_chunking(const char *doc, uint32_t len, const char *label) {

	static sink_t a, b;
	static const uint32_t sizes[] = { 1, 7, 512, 4096 };

	parse_chunked(doc, len, len ? len : 1, &a);

	for (unsigned k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
		checks++;
		parse_chunked(doc, len, sizes[k], &b);
		if (strcmp(a.buf, b.buf)) {
			fails++;
			printf("FAIL: %s: chunk size %u differs from whole-document\n",
				label, (unsigned)sizes[k]);
		}
	}

}

// -- invariant 2: resume from a mark ------------------------------
//
// Parse the whole document while recording every mark. Then, for a
// sample of those marks, restore and re-parse from the recorded offset
// and require the output to be a suffix of the full output.

typedef struct {
	sink_t		sink;
	html_ctx_t	*ctx;
	uint32_t	off[4096];
	html_state_t st[4096];
	uint32_t	line_at[4096];		// blocks emitted before this mark
	int			n;
} marker_t;

static marker_t mk;

static void mark_emit(void *user, const html_line_t *l) {

	sink_emit(&mk.sink, l);
	(void)user;

	// Record each DISTINCT mark the first time it is seen, along with
	// how many blocks had already been emitted before the block that
	// first carried it.
	//
	// Several consecutive blocks can share one mark -- a <pre> with
	// three lines in it produces three blocks and no intervening tag,
	// and so does a paragraph broken by <br>. The mark resumes at the
	// FIRST of them, which is exactly what an index wants to store.
	// See html.h.
	{
		uint32_t off = html_mark_offset(mk.ctx);
		if (mk.n == 0 || mk.off[mk.n - 1] != off) {
			if (mk.n < (int)(sizeof(mk.off) / sizeof(mk.off[0]))) {
				mk.off[mk.n] = off;
				mk.st[mk.n] = *html_mark_state(mk.ctx);
				mk.line_at[mk.n] = (uint32_t)mk.sink.n - 1;
				mk.n++;
			}
		}
	}

}

static void ck_resume(const char *doc, uint32_t len, const char *label) {

	html_ctx_t ctx;
	static sink_t tail;
	int step;

	memset(&mk, 0, sizeof(mk));
	mk.ctx = &ctx;
	html_init(&ctx, mark_emit, NULL);
	html_feed(&ctx, doc, len);
	html_finish(&ctx);

	if (mk.n < 2) return;

	// Sample rather than test every mark: a large page has thousands
	// and each costs a full re-parse of its tail.
	step = mk.n / 24;
	if (step < 1) step = 1;

	for (int i = 0; i < mk.n; i += step) {

		html_ctx_t r;
		uint32_t off = mk.off[i];
		const char *full = mk.sink.buf;
		const char *suffix;
		uint32_t skip = mk.line_at[i];

		if (off == 0 || off > len) continue;

		checks++;

		memset(&tail, 0, sizeof(tail));
		html_init(&r, sink_emit, &tail);
		html_restore(&r, &mk.st[i], off);
		html_feed(&r, doc + off, len - off);
		html_finish(&r);

		// Skip the first `skip` lines of the full output; what remains
		// must be exactly what the resumed parse produced.
		suffix = full;
		for (uint32_t k = 0; k < skip && suffix; k++) {
			suffix = strchr(suffix, '\n');
			if (suffix) suffix++;
		}

		if (!suffix) suffix = "";

		if (strcmp(suffix, tail.buf)) {
			fails++;
			printf("FAIL: %s: resume at mark %d (offset %u) diverges\n",
				label, i, (unsigned)off);
			printf("  resumed head: %.160s\n", tail.buf);
			printf("  expected head: %.160s\n", suffix);
			return;			// one report is enough; they cascade
		}

	}

}

// -- unit cases ---------------------------------------------------

static void unit_tests(void) {

	// Structure.
	ck_html("<p>hello world</p>", "PARA/0 [hello world]\n");
	ck_html("<h2>Title</h2>", "HEAD/2 [Title]\n");
	ck_html("<h1>A</h1><p>B</p>", "HEAD/1 [A]\nPARA/0 [B]\n");

	// Whitespace collapsing, including across tags. The second case is
	// the one that matters: markup between two words must not delete
	// the space between them.
	ck_html("<p>  a \n\t b  </p>", "PARA/0 [a b]\n");
	ck_html("<p>a <b>b</b> c</p>", "PARA/0 [a b c]\n");
	ck_html("<p>a<b> b </b>c</p>", "PARA/0 [a b c]\n");

	// Emphasis is structural only -- one weight per font (html.h).
	ck_html("<p><strong>x</strong><em>y</em></p>", "PARA/0 [xy]\n");

	// Non-text content is dropped, and its contents cannot be
	// mistaken for markup. The `a < b` inside the script is the case
	// that breaks a tokenizer without a raw-text state.
	ck_html("<p>a</p><script>if (a < b) { document.write('<p>no</p>'); }"
		"</script><p>b</p>", "PARA/0 [a]\nPARA/0 [b]\n");
	ck_html("<style>p { color: red }</style><p>x</p>", "PARA/0 [x]\n");

	// <noscript> content IS shown. This browser has no JavaScript, so
	// the fallback is addressed to us -- see docs/web_app.md.
	ck_html("<noscript><p>needs js</p></noscript>", "PARA/0 [needs js]\n");

	// Comments and doctypes.
	ck_html("<!DOCTYPE html><!-- gone --><p>x</p>", "PARA/0 [x]\n");
	ck_html("<!-- a -- b --><p>x</p>", "PARA/0 [x]\n");

	// Entities, named and numeric, including the cp1252 remap of a C1
	// numeric reference that HTML requires.
	ck_html("<p>a &amp; b &lt;c&gt; &quot;d&quot;</p>",
		"PARA/0 [a & b <c> \"d\"]\n");
	ck_html("<p>&mdash;&nbsp;&hellip;</p>", "PARA/0 [-- ...]\n");
	ck_html("<p>&#65;&#x42;&#8212;</p>", "PARA/0 [AB--]\n");
	ck_html("<p>&#151;</p>", "PARA/0 [--]\n");
	// A bare ampersand is content, not a broken entity.
	ck_html("<p>a & b</p>", "PARA/0 [a & b]\n");
	ck_html("<p>a &notanentity; b</p>", "PARA/0 [a &notanentity; b]\n");

	// UTF-8 folding (uni.c) all the way through.
	ck_html("<p>G\xc3\xb6" "del \xe2\x80\x94 caf\xc3\xa9</p>",
		"PARA/0 [Godel -- cafe]\n");
	// Something with no ASCII ancestor becomes '?' rather than
	// vanishing -- see uni.h.
	ck_html("<p>\xd0\x9c\xd0\xbe\xd1\x81</p>", "PARA/0 [???]\n");

	// Links: text, target, and the span that will be underlined.
	ck_html("<p>see <a href=\"/x\">here</a> now</p>",
		"PARA/0 [see here now] ->/x\n");
	ck_html("<p><a href='/a?x=1&amp;y=2'>q</a></p>",
		"PARA/0 [q] ->/a?x=1&y=2\n");
	// An anchor with no href is not a link.
	ck_html("<p><a name=\"top\">t</a></p>", "PARA/0 [t]\n");

	// Lists, including the ordered counter and nesting depth.
	ck_html("<ul><li>a</li><li>b</li></ul>",
		"LIST/0 * [a]\nLIST/0 * [b]\n");
	ck_html("<ol><li>a</li><li>b</li></ol>",
		"LIST/0 1. [a]\nLIST/0 2. [b]\n");
	ck_html("<ul><li>a<ul><li>b</li></ul></li></ul>",
		"LIST/0 * [a]\nLIST/1 - [b]\n");
	// Unclosed <li>, which is legal HTML and extremely common.
	ck_html("<ul><li>a<li>b</ul>", "LIST/0 * [a]\nLIST/0 * [b]\n");

	// Preformatted text keeps its newlines and does not collapse.
	ck_html("<pre>a  b\nc</pre>", "PRE/0 [a  b]\nPRE/0 [c]\n");

	// Blockquote depth.
	ck_html("<blockquote><p>q</p></blockquote>", "QUOTE/1 [q]\n");

	// Rules and breaks. The <br> case checks that a forced break does
	// not silently become a paragraph.
	ck_html("<p>a</p><hr><p>b</p>", "PARA/0 [a]\nRULE/0 []\nPARA/0 [b]\n");
	ck_html("<p>a<br>b</p>", "PARA/0 [a]\nPARA/0 [b]\n");

	// Tables become one block per row with cells joined, and no
	// leading separator.
	ck_html("<table><tr><td>a</td><td>b</td></tr></table>",
		"TABLE/0 [a | b]\n");

	// Images speak through their alt text; a decorative one says
	// nothing at all.
	// -- images --
	//
	// A block of its own, not inline text, because the layout engine
	// draws a box and a box cannot sit inside a wrapped line. The
	// cost is that an image inside a paragraph splits it.
	// The src rides along as link 0, so the renderer can put a hit
	// rectangle over the box and the browser can resolve it against
	// the page URL exactly as it does an anchor.
	ck_html("<p><img src=x alt=\"a cat\"></p>", "IMG/0 [a cat] ->x\n");

	// No alt: the filename, which tells a reader more than the word
	// "image" and is what they would need to go and look at it.
	ck_html("<p><img src=\"/pics/cat.png\"></p>",
		"IMG/0 [cat.png] ->/pics/cat.png\n");
	ck_html("<p><img src=\"a/b/c.png?v=2\"></p>",
		"IMG/0 [c.png] ->a/b/c.png?v=2\n");

	// An EMPTY alt is a decorative image saying so. Drawing a box for
	// every spacer GIF and tracking pixel would bury the content.
	ck_html("<p>x<img src=y alt=\"\">z</p>", "PARA/0 [xz]\n");

	// Splitting a paragraph, which is the accepted cost above.
	ck_html("<p>a<img src=x alt=\"pic\">b</p>",
		"PARA/0 [a]\nIMG/0 [pic] ->x\nPARA/0 [b]\n");

	// Malformed markup degrades rather than derailing.
	ck_html("<p>a</b>b</p>", "PARA/0 [ab]\n");
	ck_html("<p>a<unknown>b</unknown>c</p>", "PARA/0 [abc]\n");
	ck_html("<p>1 < 2 and 3 > 2</p>", "PARA/0 [1 < 2 and 3 > 2]\n");
	ck_html("<p title='a>b'>x</p>", "PARA/0 [x]\n");

	// Unquoted and empty attribute values.
	ck_html("<p><a href=/z>k</a></p>", "PARA/0 [k] ->/z\n");

	// Head content is not body text, but the title is captured.
	{
		html_ctx_t ctx;
		static sink_t s;
		const char *doc =
			"<html><head><title>  My  Page </title>"
			"<meta charset=\"utf-8\"></head><body><p>x</p></body></html>";
		memset(&s, 0, sizeof(s));
		html_init(&ctx, sink_emit, &s);
		html_feed(&ctx, doc, (uint32_t)strlen(doc));
		html_finish(&ctx);
		ck(!strcmp(s.buf, "PARA/0 [x]\n"), "head produces no body text");
		ck(!strcmp(html_title(&ctx), "My Page "), "title captured and collapsed");
	}

	// A <meta charset> naming Latin-1 switches decoding for the rest
	// of the document. 0x92 is a curly apostrophe in cp1252 and
	// nothing at all in strict Latin-1.
	{
		html_ctx_t ctx;
		static sink_t s;
		const char doc[] =
			"<meta http-equiv=Content-Type content=\"text/html; "
			"charset=iso-8859-1\"><p>it\x92s caf\xe9</p>";
		memset(&s, 0, sizeof(s));
		html_init(&ctx, sink_emit, &s);
		html_feed(&ctx, doc, (uint32_t)sizeof(doc) - 1);
		html_finish(&ctx);
		ck(!strcmp(s.buf, "PARA/0 [it's cafe]\n"), "meta charset cp1252");
	}

	// A transport charset wins over the document's own claim, because
	// it is the only one available before the first byte is decoded.
	{
		html_ctx_t ctx;
		static sink_t s;
		const char doc[] = "<meta charset=iso-8859-1><p>caf\xc3\xa9</p>";
		memset(&s, 0, sizeof(s));
		html_init(&ctx, sink_emit, &s);
		html_set_charset(&ctx, HTML_CS_UTF8);
		html_feed(&ctx, doc, (uint32_t)sizeof(doc) - 1);
		html_finish(&ctx);
		ck(!strcmp(s.buf, "PARA/0 [cafe]\n"), "transport charset wins");
	}

	// Deliberately pathological input: deeper than HTML_STACK_MAX.
	// The requirement is only that it terminates and stays in bounds
	// (sink_emit checks the rest).
	{
		static char deep[64 * 1024];
		uint32_t n = 0;
		for (int i = 0; i < 400; i++) n += (uint32_t)sprintf(deep + n, "<div>");
		n += (uint32_t)sprintf(deep + n, "text");
		for (int i = 0; i < 400; i++) n += (uint32_t)sprintf(deep + n, "</div>");
		ck_chunking(deep, n, "deep nesting");
		ck(strstr(render(deep), "[text]") != NULL,
			"text survives nesting past HTML_STACK_MAX");
	}

	// An unterminated tag, comment and entity at end of input.
	ck_html("<p>a</p><div class=\"x", "PARA/0 [a]\n");
	ck_html("<p>a</p><!-- unterminated", "PARA/0 [a]\n");
	ck_html("<p>a&amp", "PARA/0 [a&amp]\n");

	// -- uni.c directly --
	{
		char b[UNI_FOLD_MAX];
		ck(uni_fold('A', b) == 1 && b[0] == 'A', "ascii passes through");
		ck(uni_fold(0x2014, b) == 2 && !strcmp(b, "--"), "em dash");
		ck(uni_fold(0x00A0, b) == 1 && b[0] == ' ', "nbsp to space");
		ck(uni_fold(0x00AD, b) == 0, "soft hyphen dropped");
		ck(uni_fold(0x4E2D, b) == 1 && b[0] == '?', "unmappable to ?");
		ck(uni_fold(0x00DF, b) == 2 && !strcmp(b, "ss"), "eszett");

		uint32_t cp;
		ck(uni_utf8_next("\xc3\xa9", 2, &cp) == 2 && cp == 0xE9, "utf8 2-byte");
		ck(uni_utf8_next("\xe2\x80\x94", 3, &cp) == 3 && cp == 0x2014, "utf8 3-byte");
		// A truncated sequence consumes one byte, so the parser
		// resynchronises instead of eating real text after it.
		ck(uni_utf8_next("\xe2\x80", 2, &cp) == 1 && cp == 0xFFFD, "utf8 truncated");
		ck(uni_utf8_next("\xc0\xaf", 2, &cp) == 1 && cp == 0xFFFD, "utf8 overlong");
	}

	// -- invariants on the synthetic documents --
	{
		static const char *docs[] = {
			"<html><head><title>t</title></head><body>"
			"<h1>H</h1><p>Some <a href=\"/l\">linked</a> text with "
			"&amp; entities and \xc3\xa9 accents.</p>"
			"<ul><li>one</li><li>two <code>c</code></li></ul>"
			"<pre>line1\nline2</pre>"
			"<table><tr><th>a</th><th>b</th></tr>"
			"<tr><td>1</td><td>2</td></tr></table>"
			"<blockquote><p>quoted</p></blockquote>"
			"<script>var x = '<p>';</script><p>after</p></body></html>",

			"<div><div><div><p>deep</p></div></div></div>",
			"<p>a<br>b<br>c</p>",
			"<ol><li>x<ol><li>y</li></ol></li><li>z</li></ol>",
		};

		for (unsigned i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
			ck_chunking(docs[i], (uint32_t)strlen(docs[i]), "synthetic");
			ck_resume(docs[i], (uint32_t)strlen(docs[i]), "synthetic");
		}
	}

}

// -- real documents -----------------------------------------------

static char filebuf[4 * 1024 * 1024];

static void run_file(const char *path) {

	FILE *f = fopen(path, "rb");
	uint32_t n;

	if (!f) { printf("skip: cannot open %s\n", path); return; }

	n = (uint32_t)fread(filebuf, 1, sizeof(filebuf), f);
	fclose(f);

	printf("  %s (%u bytes)\n", path, (unsigned)n);
	ck_chunking(filebuf, n, path);
	ck_resume(filebuf, n, path);

}

int main(int argc, char **argv) {

	unit_tests();

	for (int i = 1; i < argc; i++) run_file(argv[i]);

	// -- declared size --
	//
	// Honoured when given, because a page of thumbnails and a page of
	// banners should not look the same. Most of the modern web sizes
	// images in CSS, so absent is the common case and 0 means "the
	// renderer decides".
	{
		html_line_t out[8];
		int n;

		n = parse_blocks("<img src=x alt=\"a\" width=\"320\" height=\"200\">",
			out, 8);
		ck(n == 1 && out[0].kind == HTML_IMAGE, "image block emitted");
		ck(n == 1 && out[0].img_w == 320 && out[0].img_h == 200,
			"width and height attributes are kept");

		// Not a pixel count. Both are legal HTML and neither is a
		// number, so both are treated as absent rather than guessed.
		n = parse_blocks("<img src=x alt=\"a\" width=\"50%\">", out, 8);
		ck(n == 1 && out[0].img_w == 0, "percentage width is ignored");
		n = parse_blocks("<img src=x alt=\"a\" width=\"auto\">", out, 8);
		ck(n == 1 && out[0].img_w == 0, "non-numeric width is ignored");

		// Attributes must not leak from one image to the next.
		n = parse_blocks("<img src=a alt=\"1\" width=\"64\">"
			"<img src=b alt=\"2\">", out, 8);
		ck(n == 2 && out[0].img_w == 64 && out[1].img_w == 0,
			"a declared size does not carry to the next image");
	}

	printf("%s: %d checks, %d failures\n",
		fails ? "FAIL" : "ok", checks, fails);

	return fails ? 1 : 0;

}
