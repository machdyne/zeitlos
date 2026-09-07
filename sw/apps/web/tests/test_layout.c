/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host tests for layout.c: the relationships that CAN be written
 * down. tests/render.c covers the ones that cannot -- see
 * sw/common/tests/zrender.h on why both are needed and why neither
 * replaces the other.
 *
 * The invariant that matters most here is REVERSIBILITY: the sum of
 * layout_count() over a range of blocks must equal the number of
 * lines layout_draw() will actually produce for that range, at every
 * width. If those two disagree the scrollbar drifts, scrolling down
 * N lines and back up N lands somewhere else, and the symptom on
 * hardware is "scrolling is slightly wrong" -- which is close to
 * undebuggable. sw/apps/read's own render_test.c checks the same
 * property for the same reason.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../../common/tests/zrender.h"

#include "../html.h"
#include "../layout.h"

static int fails, checks;

static void ck(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("FAIL: %s\n", what); }
}

#define MAX_BLOCKS 4096
static html_line_t blocks[MAX_BLOCKS];
static int nblocks;

static void collect(void *user, const html_line_t *l) {
	(void)user;
	if (nblocks < MAX_BLOCKS) blocks[nblocks++] = *l;
}

static void parse(const char *doc) {
	html_ctx_t ctx;
	nblocks = 0;
	html_init(&ctx, collect, NULL);
	html_feed(&ctx, doc, (uint32_t)strlen(doc));
	html_finish(&ctx);
}

static z_win_t win;
static z_clip_t content;

static void cfg_for(layout_cfg_t *cfg, int width) {
	memset(cfg, 0, sizeof(*cfg));
	cfg->x = content.x0 + 2;
	cfg->width = width;
	cfg->body = &z_font_5x8;
	cfg->head = &z_font_6x12;
	cfg->links_live = true;
}

// -- the wrap covers the whole block, exactly once ------------------
//
// Every character of the block must appear in exactly one display
// line, in order, apart from the single space each break consumes.
// Written as a reconstruction rather than as an arithmetic check
// because an off-by-one at a break duplicates or eats a character,
// and only rebuilding the string catches which.
static void ck_wrap_covers(const html_line_t *l, const layout_cfg_t *cfg,
	const char *what) {

	int n = layout_count(l, cfg);
	uint16_t prev_end = 0;
	char rebuilt[HTML_LINE_MAX + LAYOUT_MAX_LINES + 1];
	uint32_t rl = 0;

	checks++;

	if (l->kind == HTML_RULE || l->kind == HTML_BLANK) return;

	for (int i = 0; i < n; i++) {

		uint16_t s, e;

		if (!layout_line_range(l, cfg, i, &s, &e)) {
			fails++;
			printf("FAIL: %s: line %d of %d has no range\n", what, i, n);
			return;
		}

		if (s < prev_end) {
			fails++;
			printf("FAIL: %s: line %d starts at %u, before previous end %u\n",
				what, i, (unsigned)s, (unsigned)prev_end);
			return;
		}

		if (e < s || e > l->len) {
			fails++;
			printf("FAIL: %s: line %d range %u..%u out of bounds (len %u)\n",
				what, i, (unsigned)s, (unsigned)e, (unsigned)l->len);
			return;
		}

		// The gap between lines may only be spaces -- anything else
		// means a character was dropped.
		for (uint16_t k = prev_end; k < s; k++) {
			if (l->text[k] != ' ') {
				fails++;
				printf("FAIL: %s: dropped '%c' at %u between lines\n",
					what, l->text[k], (unsigned)k);
				return;
			}
		}

		// A space is re-inserted only where one was CONSUMED by the
		// break. A hard break inside a long word consumes nothing and
		// must rejoin with no space -- getting this wrong here was a
		// bug in this test, not in the wrapper, and it is worth
		// keeping the distinction explicit because it is exactly the
		// distinction the wrapper itself has to make.
		if (rl && s > prev_end) rebuilt[rl++] = ' ';
		memcpy(rebuilt + rl, l->text + s, (uint32_t)(e - s));
		rl += (uint32_t)(e - s);
		prev_end = e;

	}

	rebuilt[rl] = '\0';

	// The reconstruction differs from the original only in runs of
	// spaces, which html.c has already collapsed to single ones -- so
	// for a collapsed block they must match exactly.
	if (l->kind != HTML_PRE && strcmp(rebuilt, l->text)) {
		fails++;
		printf("FAIL: %s: wrap does not reconstruct the block\n", what);
		printf("  got:  %.120s\n  want: %.120s\n", rebuilt, l->text);
	}

}

// -- count agrees with draw ----------------------------------------

static void ck_count_matches_draw(const html_line_t *l,
	const layout_cfg_t *cfg, const char *what) {

	int n = layout_count(l, cfg);
	int drawn = layout_draw(l, cfg, content.y0, 0, LAYOUT_MAX_LINES,
		&content, 0, NULL, NULL, 0);

	checks++;

	if (n != drawn) {
		fails++;
		printf("FAIL: %s: layout_count says %d, layout_draw drew %d\n",
			what, n, drawn);
	}

	// And drawing it one line at a time must total the same, which is
	// what a partially-scrolled first block on screen actually does.
	{
		int total = 0;
		for (int i = 0; i < n; i++)
			total += layout_draw(l, cfg, content.y0, i, 1, &content,
				0, NULL, NULL, 0);
		checks++;
		if (total != n) {
			fails++;
			printf("FAIL: %s: line-at-a-time total %d != count %d\n",
				what, total, n);
		}
	}

}

// Stands in for the browser's img_overlay(): hands layout.c a bitmap
// for every block, or none.
static const uint32_t *test_img_bits;
static int test_img_wpl, test_img_w, test_img_h;

static void test_img_cb(uint32_t block, layout_img_t *o) {
	(void)block;
	memset(o, 0, sizeof(*o));
	o->bits = test_img_bits;
	o->wpl = test_img_wpl;
	o->w = test_img_w;
	o->h = test_img_h;
}

int main(void) {

	layout_cfg_t cfg;
	static const int widths[] = { 60, 120, 240, 400, 596 };

	if (!z_render_open(&win, 600, 440)) {
		fprintf(stderr, "test_layout: cannot map VRAM, skipping\n");
		return 77;
	}

	z_win_content_rect(&win, &content);

	// -- basic geometry --

	cfg_for(&cfg, 400);

	parse("<p>hello</p>");
	ck(nblocks == 1, "one block");
	ck(layout_count(&blocks[0], &cfg) == 1, "short paragraph is one line");
	ck(layout_line_height(&blocks[0], &cfg) == z_font_5x8.h + 1,
		"body line height");

	parse("<h1>Title</h1>");
	ck(layout_line_height(&blocks[0], &cfg) == z_font_6x12.h + 1,
		"heading line height");

	// A <br>-induced block gets no gap above it; a real paragraph does.
	parse("<p>a<br>b</p>");
	ck(nblocks == 2, "br makes two blocks");
	ck(layout_gap_before(&blocks[1], &cfg) == 0, "tight block has no gap");
	parse("<p>a</p><p>b</p>");
	ck(layout_gap_before(&blocks[1], &cfg) > 0, "paragraph has a gap");

	// HTML_BLANK and HTML_RULE occupy exactly one line, so a position
	// can never land inside a block with nowhere to be.
	parse("<p>a</p><hr><p>b</p>");
	for (int i = 0; i < nblocks; i++) {
		ck(layout_count(&blocks[i], &cfg) >= 1, "every block is >= 1 line");
	}

	// -- wrapping --

	{
		// A word longer than the line must be hard-broken, not
		// dropped and not allowed to run past the edge.
		static char doc[512];
		snprintf(doc, sizeof(doc), "<p>%.*s</p>", 300,
			"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
			"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
			"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
		parse(doc);
		cfg_for(&cfg, 120);
		ck(layout_count(&blocks[0], &cfg) > 1, "long word is broken");
		ck_wrap_covers(&blocks[0], &cfg, "long word");
	}

	// A narrow window must not divide by zero or loop forever.
	parse("<p>some ordinary words here</p>");
	cfg_for(&cfg, 1);
	ck(layout_count(&blocks[0], &cfg) >= 1, "1px width still lays out");
	ck_wrap_covers(&blocks[0], &cfg, "1px width");

	// -- the real document, at every width --

	{
		static const char *doc =
			"<h1>Heading One</h1>"
			"<p>The quick brown fox jumps over the lazy dog, and then "
			"does it again with <a href=\"/l\">a link</a> and some "
			"<code>inline code</code> in the middle of it all.</p>"
			"<h3>Subsection</h3>"
			"<ul><li>first item, which is long enough that it will "
			"certainly have to wrap at any sensible window width</li>"
			"<li>second</li></ul>"
			"<ol><li>one</li><li>two</li></ol>"
			"<blockquote><p>A quoted passage that also runs on for a "
			"while so that it wraps.</p></blockquote>"
			"<pre>preformatted   spacing\nsecond line</pre>"
			"<table><tr><td>a</td><td>b</td></tr></table>"
			"<hr><p>after the rule</p>";

		parse(doc);
		ck(nblocks > 8, "real document produces blocks");

		for (unsigned wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
			cfg_for(&cfg, widths[wi]);
			for (int i = 0; i < nblocks; i++) {
				char what[64];
				snprintf(what, sizeof(what), "block %d at width %d",
					i, widths[wi]);
				ck_wrap_covers(&blocks[i], &cfg, what);
				ck_count_matches_draw(&blocks[i], &cfg, what);
			}
		}
	}

	// -- hit testing --
	//
	// A link that wraps must produce a rectangle on EVERY line it
	// covers, or half of it is not clickable.
	{
		layout_hit_t hits[LAYOUT_MAX_HITS];
		int nhits = 0;
		int lines_with_link = 0;

		parse("<p><a href=\"/x\">a link whose text is long enough that it "
			"must wrap across more than one display line</a></p>");
		cfg_for(&cfg, 120);

		layout_draw(&blocks[0], &cfg, content.y0, 0, LAYOUT_MAX_LINES,
			&content, 0, hits, &nhits, LAYOUT_MAX_HITS);

		ck(layout_count(&blocks[0], &cfg) > 1, "link block wraps");
		ck(nhits >= 2, "a wrapped link is clickable on every line");

		for (int i = 0; i < nhits; i++) {
			ck(hits[i].w > 0 && hits[i].h > 0, "hit rect is non-empty");
			ck(hits[i].link < blocks[0].nlinks, "hit names a real link");
			ck(hits[i].x >= cfg.x, "hit is inside the content area");
			ck(hits[i].x + hits[i].w <= cfg.x + cfg.width + z_font_5x8.w,
				"hit does not run past the right edge");
		}

		// One rectangle per display line, no more.
		for (int i = 0; i < nhits; i++)
			for (int j = i + 1; j < nhits; j++)
				if (hits[i].y == hits[j].y && hits[i].link == hits[j].link)
					lines_with_link++;
		ck(lines_with_link == 0, "no duplicate rect for one link on one line");
	}

	// Underlines are suppressed while a page is still loading, so a
	// half-parsed document does not look interactive before it is.
	{
		layout_hit_t hits[LAYOUT_MAX_HITS];
		int nhits = 0;
		parse("<p><a href=\"/x\">link</a></p>");
		cfg_for(&cfg, 400);
		cfg.links_live = false;
		layout_draw(&blocks[0], &cfg, content.y0, 0, LAYOUT_MAX_LINES,
			&content, 0, hits, &nhits, LAYOUT_MAX_HITS);
		ck(nhits == 0, "no hit rects while links are not live");
	}

	// -- image placeholders --
	//
	// The box is the point of the feature: a reader should be able to
	// tell a thumbnail from a banner without loading either. So the
	// declared size has to survive to layout, and the clamps have to
	// be clamps rather than the usual answer.
	{
		html_line_t im;
		layout_cfg_t c;
		int tall, wide, deflt;

		cfg_for(&c, 400);

		memset(&im, 0, sizeof(im));
		im.kind = HTML_IMAGE;
		snprintf(im.text, sizeof(im.text), "a picture");
		im.len = (uint16_t)strlen(im.text);

		// No declared size: the renderer picks one, and it is the
		// common case because the modern web sizes images in CSS.
		deflt = layout_count(&im, &c);
		ck(deflt > 1, "an image with no declared size still has a box");

		// A taller image occupies more lines than a shorter one.
		im.img_w = 100; im.img_h = 40;
		tall = layout_count(&im, &c);
		im.img_h = 200;
		ck(layout_count(&im, &c) > tall,
			"a taller image occupies more lines");

		// Wider than the content: scaled down, keeping the aspect
		// ratio, rather than running off the edge.
		im.img_w = (uint16_t)(c.width * 4); im.img_h = 400;
		wide = layout_count(&im, &c);
		ck(wide > 0 && wide < 40,
			"an oversized image is scaled, not drawn off the edge");

		// Absurd height is capped, so one bad attribute cannot push
		// the rest of the page off the bottom.
		im.img_w = 50; im.img_h = 4000;
		ck(layout_count(&im, &c) <= 24,
			"an absurd height is capped");

		// An image has no selectable text rows -- the caption is part
		// of the box, not wrapped content.
		{
			uint16_t s0, e0;
			im.img_w = 100; im.img_h = 100;
			ck(!layout_line_range(&im, &c, 0, &s0, &e0),
				"an image exposes no text rows");
		}
	}

	// -- an image drawn in place --
	//
	// When the browser supplies a decoded bitmap for a block, the
	// picture must REPLACE the placeholder box, not be drawn inside
	// or over it. A leftover frame around a loaded image looks like a
	// rendering fault, and drawing both is the easiest mistake here.
	{
		static uint32_t bits[8 * 64];
		html_line_t im;
		layout_cfg_t c;
		int rows;

		cfg_for(&c, 400);

		memset(&im, 0, sizeof(im));
		im.kind = HTML_IMAGE;
		snprintf(im.text, sizeof(im.text), "pic");
		im.len = 3;
		// The box is larger than the picture, which is the normal
		// case: zimg downscales by powers of two only, so a 300-pixel
		// image in a 200-pixel box is decoded at 1/2 and drawn 150
		// wide. The leftover is left blank rather than stretched.
		im.img_w = 200; im.img_h = 100;

		// A solid source, so every pixel of the drawn area is set.
		for (unsigned i = 0; i < sizeof(bits)/sizeof(bits[0]); i++)
			bits[i] = 0xffffffffu;

		test_img_bits = bits;
		test_img_wpl = 2;			// words; layout.c converts to bytes
		test_img_w = 64;
		test_img_h = 32;
		c.img = test_img_cb;

		z_render_clear();
		rows = layout_draw(&im, &c, 0, 0, 100, NULL, 0, NULL, NULL, 0);
		ck(rows > 0, "an in-place image occupies lines");

		// The box's own frame would put a pixel at the top-left
		// corner of the block and a matching one at the box's full
		// width. A 64-wide image fills only its own area.
		{
			int box_w, box_h;
			layout_image_box(&im, &c, &box_w, &box_h);
			ck(box_w > 64, "the box is larger than the decoded picture");
			ck(z_render_get(c.x, 0) == 1, "the image is drawn");
			ck(z_render_get(c.x + 63, 31) == 1, "including its far corner");
			ck(z_render_get(c.x + box_w - 1, 0) == 0,
				"and the placeholder frame is NOT drawn around it");
		}

		// -- the stride is in BYTES --
		//
		// A striped source, so a wrong stride is visible as wrong
		// pixels rather than merely a different-looking picture. The
		// real bug was passing words where the hardware wants bytes,
		// which scrambles every row by a factor of four and reads as
		// "the dither is broken" rather than as a stride error.
		//
		// A solid bitmap cannot catch this -- every row looks the
		// same however they are indexed -- which is exactly why the
		// first version of these tests passed while the image on
		// screen was unrecognisable.
		{
			int y;

			for (y = 0; y < 32; y++) {
				bits[y * 2 + 0] = (y & 1) ? 0xffffffffu : 0;
				bits[y * 2 + 1] = (y & 1) ? 0xffffffffu : 0;
			}

			test_img_bits = bits;
			z_render_clear();
			layout_draw(&im, &c, 0, 0, 100, NULL, 0, NULL, NULL, 0);

			ck(z_render_get(c.x, 0) == 0, "row 0 of a striped source is clear");
			ck(z_render_get(c.x, 1) == 1, "row 1 is set");
			ck(z_render_get(c.x, 2) == 0, "row 2 is clear");
			ck(z_render_get(c.x, 3) == 1, "and row 3 is set");
		}

		// Restore the solid source for the check below.
		for (unsigned i = 0; i < sizeof(bits)/sizeof(bits[0]); i++)
			bits[i] = 0xffffffffu;

		// -- a loaded image sizes its own box --
		//
		// The placeholder is sized from the markup, but the decoders
		// scale by powers of two, so a 250x224 picture in a 250-wide
		// box comes back 125x112. Without this the box keeps its
		// declared height and leaves a hundred-odd pixels of nothing
		// between the image and the next paragraph.
		{
			layout_cfg_t d;
			html_line_t big;
			int bw0, bh0, bw1, bh1;

			cfg_for(&d, 400);

			memset(&big, 0, sizeof(big));
			big.kind = HTML_IMAGE;
			snprintf(big.text, sizeof(big.text), "pic");
			big.len = 3;
			// Under the 24-line height cap, so the cap is not what is
			// being measured here.
			big.img_w = 250; big.img_h = 150;
			snprintf(big.links[0], sizeof(big.links[0]), "cat.jpg");
			big.nlinks = 1;

			layout_image_box(&big, &d, &bw0, &bh0);
			ck(bh0 == 150, "an unloaded box uses the declared height");

			d.img_src = "cat.jpg";
			d.img_dw = 125; d.img_dh = 75;
			layout_image_box(&big, &d, &bw1, &bh1);
			ck(bw1 == 125 && bh1 == 75,
				"a loaded image sizes the box to what was decoded");
			ck(layout_count(&big, &d) < layout_count(&big, &c),
				"so it occupies fewer lines than the placeholder did");

			// A DIFFERENT image must not be resized by it.
			snprintf(big.links[0], sizeof(big.links[0]), "dog.jpg");
			layout_image_box(&big, &d, &bw1, &bh1);
			ck(bh1 == 150, "another image keeps its own declared size");
		}

		// Without a bitmap the box comes back.
		test_img_bits = NULL;
		z_render_clear();
		layout_draw(&im, &c, 0, 0, 100, NULL, 0, NULL, NULL, 0);
		{
			int box_w, box_h;
			layout_image_box(&im, &c, &box_w, &box_h);
			ck(z_render_get(c.x + box_w - 1, 0) == 1,
				"with no image, the placeholder box is drawn");
		}
	}

	printf("%s: %d checks, %d failures\n",
		fails ? "FAIL" : "ok", checks, fails);

	return fails ? 1 : 0;

}
