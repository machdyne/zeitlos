/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host tests for page.c: the index, and the scroll arithmetic on top
 * of it.
 *
 * This is the file that exists because sw/apps/read's scrolling took
 * the longest of anything in that app to get right, and because the
 * symptom of getting it wrong -- "scrolling is slightly off" -- is
 * close to undebuggable on hardware. Everything here runs against a
 * buffer in memory through the same read callback the real app fills
 * from the card.
 *
 * -- the invariants --
 *
 * 1. THE INDEX AGREES WITH A PARSE FROM THE START. Every block
 *    page_fetch() returns must be byte-identical to the block a
 *    straight html.c parse produced at that position. This is the one
 *    that catches a checkpoint recorded at the wrong block number,
 *    which is otherwise invisible until a specific paragraph is
 *    scrolled past.
 *
 * 2. SCROLLING IS REVERSIBLE. Down N lines and back up N lands in
 *    exactly the same position, at every width, from every starting
 *    point.
 *
 * 3. STREAMING CONVERGES. A document indexed in small increments as
 *    it "arrives" must end up with the same block count and the same
 *    content as one indexed in a single pass. That is what the
 *    frontier rollback is for, and it is the part most likely to be
 *    subtly wrong.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../../common/tests/zrender.h"

#include "../page.h"
#include "../html.h"
#include "../layout.h"

static int fails, checks;

static void ck(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("FAIL: %s\n", what); }
}

// -- a byte source backed by memory --------------------------------

typedef struct {
	const char	*data;
	uint32_t	len;
	uint32_t	reads;		// how many times the source was hit
} mem_src_t;

static uint32_t mem_read(void *user, uint32_t off, char *buf, uint32_t len) {

	mem_src_t *m = (mem_src_t *)user;

	m->reads++;

	if (off >= m->len) return 0;
	if (off + len > m->len) len = m->len - off;

	memcpy(buf, m->data + off, len);
	return len;

}

// -- the reference: a straight parse from the start ----------------

#define REF_MAX 8192
static html_line_t ref[REF_MAX];
static int ref_n;

static void ref_emit(void *user, const html_line_t *l) {
	(void)user;
	if (ref_n < REF_MAX) ref[ref_n++] = *l;
}

static void build_reference(const char *doc, uint32_t len) {
	html_ctx_t c;
	ref_n = 0;
	html_init(&c, ref_emit, NULL);
	html_feed(&c, doc, len);
	html_finish(&c);
}

// Blocks compare on everything that reaches the screen. Comparing the
// whole struct would also compare padding, which is not defined.
static bool same_block(const html_line_t *a, const html_line_t *b) {
	if (a->kind != b->kind || a->level != b->level) return false;
	if (a->len != b->len || a->tight != b->tight) return false;
	if (strcmp(a->marker, b->marker)) return false;
	if (memcmp(a->text, b->text, a->len)) return false;
	if (a->nspans != b->nspans || a->nlinks != b->nlinks) return false;
	for (int i = 0; i < a->nlinks; i++)
		if (strcmp(a->links[i], b->links[i])) return false;
	for (int i = 0; i < a->nspans; i++)
		if (a->spans[i].start != b->spans[i].start ||
			a->spans[i].len != b->spans[i].len ||
			a->spans[i].kind != b->spans[i].kind ||
			a->spans[i].link != b->spans[i].link) return false;
	return true;
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

// -- invariant 1 ---------------------------------------------------

static void ck_index_matches(page_t *p, const char *what) {

	static html_line_t got[PAGE_FETCH_MAX];

	checks++;

	if ((int)page_blocks(p) != ref_n) {
		fails++;
		printf("FAIL: %s: index has %u blocks, a straight parse has %d\n",
			what, (unsigned)page_blocks(p), ref_n);
		return;
	}

	// Fetch in runs, including runs that deliberately straddle
	// checkpoint boundaries.
	for (uint32_t first = 0; first < page_blocks(p); first += 7) {

		uint32_t want = page_blocks(p) - first;
		uint32_t n;

		if (want > 11) want = 11;

		n = page_fetch(p, first, want, got);

		if (n != want) {
			fails++;
			printf("FAIL: %s: fetch(%u,%u) returned %u\n",
				what, (unsigned)first, (unsigned)want, (unsigned)n);
			return;
		}

		for (uint32_t i = 0; i < n; i++) {
			if (!same_block(&got[i], &ref[first + i])) {
				fails++;
				printf("FAIL: %s: block %u differs\n"
					"  index: [%.70s]\n  parse: [%.70s]\n",
					what, (unsigned)(first + i),
					got[i].text, ref[first + i].text);
				return;
			}
		}

	}

}

// The checkpoint table has to stay strictly increasing by block, and
// no denser than the stride implies.
//
// This is here because of a bug that no correctness check caught: a
// streaming index re-emits blocks after its last mark on every pass,
// and without a guard it recorded a duplicate checkpoint for the same
// block each time. Every block still came back correct -- the
// duplicates agreed -- so the whole suite passed while the table
// filled with copies, thinned repeatedly, and the stride climbed from
// 4 to 128. The only symptom was that each fetch replayed 32x more
// blocks than it needed to.
static void ck_ckpts_sane(page_t *p, const char *what) {

	checks++;

	for (int i = 1; i < p->nck; i++) {
		if (p->ck[i].block <= p->ck[i - 1].block) {
			fails++;
			printf("FAIL: %s: checkpoint %d block %u <= previous %u\n",
				what, i, (unsigned)p->ck[i].block,
				(unsigned)p->ck[i - 1].block);
			return;
		}
		if (p->ck[i].off < p->ck[i - 1].off) {
			fails++;
			printf("FAIL: %s: checkpoint %d offset goes backwards\n",
				what, i);
			return;
		}
	}

	checks++;
	// One checkpoint per `stride` blocks, give or take the first.
	if (p->nck > (int)(page_blocks(p) / p->stride) + 2) {
		fails++;
		printf("FAIL: %s: %d checkpoints for %u blocks at stride %u\n",
			what, p->nck, (unsigned)page_blocks(p), (unsigned)p->stride);
	}

}

// -- invariant 2 ---------------------------------------------------

static void ck_scroll_reversible(page_t *p, const layout_cfg_t *cfg,
	const char *what) {

	static const int32_t steps[] = { 1, 3, 17, 40, 200 };

	for (unsigned s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {

		for (uint32_t start = 0; start < page_blocks(p);
			start += (page_blocks(p) / 5) + 1) {

			page_pos_t pos = { start, 0 };
			page_pos_t saved = pos;
			int32_t down, up;

			checks++;

			down = page_advance(p, cfg, &pos, steps[s]);
			up = page_advance(p, cfg, &pos, -down);

			if (-up != down || pos.block != saved.block ||
				pos.sub != saved.sub) {
				fails++;
				printf("FAIL: %s: down %d then up %d from (%u,%u) "
					"landed at (%u,%u)\n", what, (int)down, (int)-up,
					(unsigned)saved.block, (unsigned)saved.sub,
					(unsigned)pos.block, (unsigned)pos.sub);
				return;
			}

		}

	}

}

// Moving down one line at a time must reach the same place as moving
// down N at once. A separate check because the two take different
// branches through page_advance().
static void ck_scroll_increments(page_t *p, const layout_cfg_t *cfg,
	const char *what) {

	page_pos_t a = { 0, 0 }, b = { 0, 0 };
	int32_t bulk, total = 0;

	checks++;

	bulk = page_advance(p, cfg, &a, 50);
	for (int i = 0; i < 50; i++) total += page_advance(p, cfg, &b, 1);

	if (bulk != total || a.block != b.block || a.sub != b.sub) {
		fails++;
		printf("FAIL: %s: 50 at once reached (%u,%u), "
			"50 singles reached (%u,%u)\n", what,
			(unsigned)a.block, (unsigned)a.sub,
			(unsigned)b.block, (unsigned)b.sub);
	}

}

// -- driving ---------------------------------------------------------

static page_t pg;
static mem_src_t src;

// Index a document all at once.
static void index_whole(const char *doc, uint32_t len) {
	src.data = doc;
	src.len = len;
	src.reads = 0;
	page_init(&pg, mem_read, &src);
	page_set_size(&pg, len, true);
	while (!page_indexed(&pg)) {
		if (page_index_more(&pg, 100000) == 0 && page_indexed(&pg)) break;
		if (!page_indexed(&pg) && page_index_more(&pg, 100000) == 0) break;
	}
}

// Index a document as though it were arriving `chunk` bytes at a
// time, with a small block budget per pass -- which is what the real
// app does so the window keeps repainting.
static void index_streaming(const char *doc, uint32_t len, uint32_t chunk,
	uint32_t budget) {

	uint32_t have = 0;

	src.data = doc;
	src.len = len;
	src.reads = 0;
	page_init(&pg, mem_read, &src);

	while (have < len) {
		have += chunk;
		if (have > len) have = len;
		page_set_size(&pg, have, have >= len);
		// Several passes per arrival, as the idle path would do.
		for (int i = 0; i < 400 && !page_indexed(&pg); i++)
			if (page_index_more(&pg, budget) == 0 && have < len) break;
	}

	page_set_size(&pg, len, true);
	for (int i = 0; i < 4000 && !page_indexed(&pg); i++)
		page_index_more(&pg, budget);

}

static const char *doc_small =
	"<html><head><title>Test</title></head><body>"
	"<h1>Heading</h1>"
	"<p>First paragraph with some words in it that will wrap at a "
	"narrow width but not at a wide one.</p>"
	"<ul><li>alpha</li><li>beta</li><li>gamma</li></ul>"
	"<p>Second paragraph, <a href=\"/x\">with a link</a> inside.</p>"
	"<pre>one\ntwo\nthree</pre>"
	"<blockquote><p>quoted text</p></blockquote>"
	"<table><tr><td>a</td><td>b</td></tr><tr><td>c</td><td>d</td></tr></table>"
	"<hr><p>last</p></body></html>";

// Big enough to force the checkpoint table to thin at least twice,
// which is the path that is otherwise never taken.
static char doc_big[400 * 1024];
static uint32_t doc_big_len;

static void build_big(void) {
	uint32_t n = 0;
	n += (uint32_t)sprintf(doc_big + n, "<html><body><h1>Long</h1>");
	for (int i = 0; i < 1500 && n < sizeof(doc_big) - 512; i++) {
		n += (uint32_t)sprintf(doc_big + n,
			"<h3>Section %d</h3><p>Paragraph %d with enough words in it "
			"to wrap more than once at any reasonable window width, and "
			"a <a href=\"/l%d\">link</a> too.</p><ul><li>item %d</li>"
			"<li>another</li></ul>", i, i, i, i);
	}
	n += (uint32_t)sprintf(doc_big + n, "</body></html>");
	doc_big_len = n;
}

static char filebuf[4 * 1024 * 1024];

static void run_doc(const char *doc, uint32_t len, const char *what) {

	layout_cfg_t cfg;
	static const int widths[] = { 120, 400, 596 };
	char label[128];

	build_reference(doc, len);

	// -- whole-document index --
	index_whole(doc, len);
	snprintf(label, sizeof(label), "%s whole", what);
	ck(page_indexed(&pg), "document reaches indexed");
	ck_index_matches(&pg, label);
	ck_ckpts_sane(&pg, label);

	for (unsigned w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
		cfg_for(&cfg, widths[w]);
		snprintf(label, sizeof(label), "%s whole w%d", what, widths[w]);
		ck_scroll_reversible(&pg, &cfg, label);
		ck_scroll_increments(&pg, &cfg, label);
	}

	// -- streamed in, which is how it really arrives --
	//
	// 512 is what the transport delivers (ZSTREAM_CHUNK_SIZE_DEFAULT);
	// a budget of 8 blocks a pass is what keeps the window repainting.
	{
		static const uint32_t chunks[] = { 137, 512, 4096 };

		for (unsigned c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
			index_streaming(doc, len, chunks[c], 8);
			snprintf(label, sizeof(label), "%s streamed/%u",
				what, (unsigned)chunks[c]);
			ck(page_indexed(&pg), "streamed document reaches indexed");
			ck_index_matches(&pg, label);
			ck_ckpts_sane(&pg, label);
		}
	}

}

int main(int argc, char **argv) {

	if (!z_render_open(&win, 600, 440)) {
		fprintf(stderr, "test_page: cannot map VRAM, skipping\n");
		return 77;
	}

	z_win_content_rect(&win, &content);

	run_doc(doc_small, (uint32_t)strlen(doc_small), "small");

	build_big();
	run_doc(doc_big, doc_big_len, "big");

	// The big document must actually have exercised the thinning
	// path, or the test above proved nothing about it.
	index_whole(doc_big, doc_big_len);
	ck(pg.stride > PAGE_STRIDE_MIN, "checkpoint table thinned at least once");
	ck(pg.nck <= PAGE_CKPT_MAX, "checkpoint table stayed bounded");

	// An empty document, and one that is nothing but a truncated tag.
	{
		index_whole("", 0);
		ck(page_blocks(&pg) == 0, "empty document has no blocks");
		{
			layout_cfg_t cfg;
			page_pos_t pos = { 0, 0 };
			cfg_for(&cfg, 400);
			ck(page_advance(&pg, &cfg, &pos, 10) == 0,
				"scrolling an empty document does nothing");
		}
		build_reference("<p>a</p><div class=\"x", 21);
		index_whole("<p>a</p><div class=\"x", 21);
		ck_index_matches(&pg, "truncated tag");
	}

	// Real pages, if any were given.
	for (int i = 1; i < argc; i++) {
		FILE *f = fopen(argv[i], "rb");
		uint32_t n;
		if (!f) { printf("skip: cannot open %s\n", argv[i]); continue; }
		n = (uint32_t)fread(filebuf, 1, sizeof(filebuf), f);
		fclose(f);
		printf("  %s (%u bytes)\n", argv[i], (unsigned)n);
		run_doc(filebuf, n, argv[i]);
	}

	printf("%s: %d checks, %d failures\n",
		fails ? "FAIL" : "ok", checks, fails);

	return fails ? 1 : 0;

}
