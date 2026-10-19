/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host tests for find-in-page: find.c on its own, then the same search
 * walked through a real page with page_scan() in small steps, the way
 * web.c's main loop does it, and layout_locate() for where a match is
 * drawn. docs/web_app.md, "Find in page".
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../../common/tests/zrender.h"

#include "../find.h"
#include "../page.h"
#include "../html.h"
#include "../layout.h"

static int fails, checks;

static void ck(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("FAIL: %s\n", what); }
}

// -- find.c against a list of strings ------------------------------

static const char *blocks[] = {
	"Alpha beta gamma",
	"nothing here",
	"BETA again, and beta",
	"",
	"the end, beta",
};
#define NB ((uint32_t)(sizeof(blocks) / sizeof(blocks[0])))

// Runs a search to completion the way the app does: feed from f->block,
// wrap at the end. `budget` blocks per "pass".
static find_result_t run_list(find_t *f, uint32_t budget) {
	int guard = 0;
	while (f->result == FIND_RUN && guard++ < 100) {
		uint32_t n = 0, b = f->block;
		bool done = false;
		while (b < NB && n < budget && !done) {
			done = find_feed(f, b, blocks[b], (uint32_t)strlen(blocks[b]));
			b++; n++;
		}
		if (!done && n < budget) find_wrap(f);
	}
	return f->result;
}

static void test_list(void) {

	find_t f;
	uint32_t seen_b[8], seen_o[8];
	int k;

	ck(find_text("Hello World", 11, 0, "world", 5) == 6, "case-insensitive");
	ck(find_text("Hello World", 11, 7, "world", 5) == -1, "from past the match");
	ck(find_text("abc", 3, 0, "abcd", 4) == -1, "needle longer than text");
	ck(find_text("abc", 3, 5, "a", 1) == -1, "from past the end");
	ck(find_text("aaab", 4, 0, "aab", 3) == 1, "restart after a partial match");
	ck(find_text("x", 1, 0, "", 0) == -1, "empty needle");

	ck(!find_begin(&f, "", 0, 0), "empty needle refused");
	ck(!find_begin(&f, NULL, 0, 0), "NULL needle refused");

	// Every match in order, then wrapping round to the first again.
	for (uint32_t budget = 1; budget <= 6; budget++) {
		char what[80];
		ck(find_begin(&f, "beta", 0, 0), "begin");
		for (k = 0; k < 5; k++) {
			if (k) find_again(&f);
			if (run_list(&f, budget) != FIND_FOUND) break;
			seen_b[k] = f.found_block;
			seen_o[k] = f.found_off;
		}
		snprintf(what, sizeof(what), "all matches in order, budget %u", (unsigned)budget);
		ck(k == 5 &&
			seen_b[0] == 0 && seen_o[0] == 6 &&
			seen_b[1] == 2 && seen_o[1] == 0 &&
			seen_b[2] == 2 && seen_o[2] == 16 &&
			seen_b[3] == 4 && seen_o[3] == 9 &&
			seen_b[4] == 0 && seen_o[4] == 6, what);
		ck(f.wrapped, "the fifth match came after a wrap");
	}

	// Started below the only match: found by wrapping.
	find_begin(&f, "gamma", 2, 0);
	ck(run_list(&f, 2) == FIND_FOUND && f.found_block == 0 && f.wrapped,
		"wrap finds a match above the start");

	// The only match, found again by F3: wraps onto itself.
	find_begin(&f, "nothing", 0, 0);
	ck(run_list(&f, 3) == FIND_FOUND && f.found_block == 1, "single match");
	ck(find_again(&f) && run_list(&f, 3) == FIND_FOUND &&
		f.found_block == 1 && f.found_off == 0 && f.wrapped,
		"single match again, after wrapping");

	// Not there at all: one full round, then FIND_NONE -- never a loop.
	for (uint32_t start = 0; start < NB; start++) {
		find_begin(&f, "zeitlos", start, 0);
		ck(run_list(&f, 2) == FIND_NONE, "absent text ends");
	}
	find_begin(&f, "zeitlos", 0, 0);
	ck(run_list(&f, 100) == FIND_NONE && !f.wrapped,
		"from the top there is nothing to wrap to");

	// Starting part way into a block: earlier text in it is found only
	// after wrapping, and later text first.
	find_begin(&f, "beta", 2, 5);
	ck(run_list(&f, 2) == FIND_FOUND && f.found_block == 2 && f.found_off == 16,
		"start inside a block skips what is before");
	find_begin(&f, "beta", 2, 17);
	ck(run_list(&f, 2) == FIND_FOUND && f.found_block == 4, "then the next block");
	find_begin(&f, "again", 2, 10);
	ck(run_list(&f, 2) == FIND_FOUND && f.found_block == 2 &&
		f.found_off == 5 && f.wrapped,
		"a match before the start of the start block, after the wrap");

	find_begin(&f, "zeitlos", 1, 0);
	run_list(&f, 2);
	ck(!find_again(&f), "find_again needs a previous match");

	// A needle longer than FIND_MAX is cut, not overrun.
	{
		char big[200];
		memset(big, 'q', sizeof(big) - 1);
		big[sizeof(big) - 1] = 0;
		ck(find_begin(&f, big, 0, 0) && f.nlen == FIND_MAX - 1 &&
			strlen(f.needle) == FIND_MAX - 1, "long needle cut");
	}

}

// -- through a real page -------------------------------------------

typedef struct { const char *data; uint32_t len; } mem_src_t;

static uint32_t mem_read(void *user, uint32_t off, char *buf, uint32_t len) {
	mem_src_t *m = (mem_src_t *)user;
	if (off >= m->len) return 0;
	if (off + len > m->len) len = m->len - off;
	memcpy(buf, m->data + off, len);
	return len;
}

static page_t pg;
static layout_cfg_t cfg;
static find_t fnd;
static uint32_t scanned;

static bool scan_cb(void *user, uint32_t block, const html_line_t *l) {
	(void)user;
	scanned++;
	return find_feed(&fnd, block, l->text, l->len);
}

static find_result_t run_page(uint32_t budget, uint32_t *passes) {
	*passes = 0;
	while (fnd.result == FIND_RUN && *passes < 100000) {
		uint32_t n = page_scan(&pg, fnd.block, budget, scan_cb, NULL);
		(*passes)++;
		if (fnd.result == FIND_RUN && n < budget) find_wrap(&fnd);
	}
	return fnd.result;
}

static char *doc;
static uint32_t doc_len;

static void build_doc(void) {
	uint32_t cap = 400000, n = 0;
	doc = malloc(cap);
	n += (uint32_t)snprintf(doc + n, cap - n, "<html><head><title>T</title></head><body>\n");
	for (int i = 0; i < 3000; i++) {
		if (i == 1234)
			n += (uint32_t)snprintf(doc + n, cap - n,
				"<p>Paragraph %d has the <b>Needle</b> &amp; more words after it "
				"so that it wraps onto several lines at a narrow width, which "
				"is where the highlight has to land on the right line.</p>\n", i);
		else if (i == 2900)
			n += (uint32_t)snprintf(doc + n, cap - n,
				"<ul><li>item %d: a needle in a list</li></ul>\n", i);
		else
			n += (uint32_t)snprintf(doc + n, cap - n,
				"<p>Paragraph %d is filler text about haystacks.</p>\n", i);
	}
	n += (uint32_t)snprintf(doc + n, cap - n, "</body></html>\n");
	doc_len = n;
}

static void test_page(void) {

	static mem_src_t src;
	static html_line_t one;
	z_win_t win;
	z_clip_t content;
	uint32_t passes, first_found;

	if (!z_render_open(&win, 600, 440)) {
		printf("test_find: cannot map VRAM, skipping the page part\n");
		return;
	}
	z_win_content_rect(&win, &content);

	build_doc();
	src.data = doc;
	src.len = doc_len;

	page_init(&pg, mem_read, &src);
	page_set_size(&pg, doc_len, true);
	while (!page_indexed(&pg)) page_index_more(&pg, 24);
	ck(page_blocks(&pg) >= 3000, "page indexed");

	memset(&cfg, 0, sizeof(cfg));
	cfg.x = content.x0 + 2;
	cfg.width = 150;
	cfg.body = &z_font_5x8;
	cfg.head = &z_font_6x12;

	// From the top: the paragraph, then the list item, then round again.
	find_begin(&fnd, "NEEDLE", 0, 0);
	scanned = 0;
	ck(run_page(32, &passes) == FIND_FOUND, "found in the page");
	first_found = fnd.found_block;
	ck(page_fetch(&pg, fnd.found_block, 1, &one) == 1 &&
		!strncmp(one.text + fnd.found_off, "Needle", 6), "the right text");
	ck(passes > 10, "searched in many bounded steps");
	ck(scanned == fnd.found_block + 1, "each block looked at once");

	{
		layout_loc_t loc;
		uint16_t s, e;
		ck(layout_locate(&one, &cfg, (uint16_t)fnd.found_off, &loc),
			"locate the match");
		ck(layout_line_range(&one, &cfg, loc.line, &s, &e) &&
			s <= fnd.found_off && fnd.found_off < e && loc.end == e,
			"locate agrees with the line range");
		ck(loc.x == cfg.x + (int)(fnd.found_off - s) * z_font_5x8.w &&
			loc.cw == z_font_5x8.w && loc.ch == z_font_5x8.h,
			"locate's x is the character's cell");

		// Every character of a wrapped paragraph lands on a line that
		// contains it.
		for (uint16_t off = 0; off < one.len; off++) {
			if (!layout_locate(&one, &cfg, off, &loc) ||
				!layout_line_range(&one, &cfg, loc.line, &s, &e) ||
				off < s || off > e) {
				ck(0, "every offset on its own line");
				break;
			}
		}
		ck(layout_count(&one, &cfg) > 2, "the paragraph wraps");
	}

	ck(find_again(&fnd) && run_page(32, &passes) == FIND_FOUND &&
		fnd.found_block > first_found, "the next match is further down");
	ck(page_fetch(&pg, fnd.found_block, 1, &one) == 1 &&
		one.kind == HTML_LIST, "in the list");
	{
		layout_loc_t loc;
		ck(layout_locate(&one, &cfg, (uint16_t)fnd.found_off, &loc) &&
			loc.x > cfg.x, "a list item is indented");
	}

	ck(find_again(&fnd) && run_page(32, &passes) == FIND_FOUND &&
		fnd.found_block == first_found && fnd.wrapped, "and round to the first");

	// Budgets of every size agree.
	for (uint32_t b = 1; b <= 64; b *= 4) {
		find_begin(&fnd, "needle", 2000, 0);
		ck(run_page(b, &passes) == FIND_FOUND && fnd.found_block > 2000,
			"same answer at any budget");
	}

	find_begin(&fnd, "not on this page", 1500, 0);
	ck(run_page(32, &passes) == FIND_NONE, "absent: one round, then stop");

	// Entities are searched decoded: "&amp;" is "&" on the page.
	find_begin(&fnd, "needle & more", 0, 0);
	ck(run_page(32, &passes) == FIND_FOUND && fnd.found_block == first_found,
		"matches the text as displayed, not the markup");
	find_begin(&fnd, "<b>", 0, 0);
	ck(run_page(32, &passes) == FIND_NONE, "markup is not text");

	// page_scan stops when asked and never passes the index.
	{
		uint32_t n;
		scanned = 0;
		find_begin(&fnd, "x", page_blocks(&pg), 0);
		n = page_scan(&pg, page_blocks(&pg), 10, scan_cb, NULL);
		ck(n == 0 && scanned == 0, "scan past the end is empty");
		n = page_scan(&pg, page_blocks(&pg) - 3, 10, scan_cb, NULL);
		ck(n <= 3, "scan stops at the last block");
	}

	free(doc);

}

int main(void) {

	test_list();
	test_page();

	printf("%s: %d checks, %d failures\n",
		fails ? "FAIL" : "ok", checks, fails);

	return fails ? 1 : 0;

}
