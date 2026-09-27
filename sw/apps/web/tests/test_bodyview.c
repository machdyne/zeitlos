/*
 * Zeitlos -- tests for bodyview.c: scrolling by blit, pixel for pixel.
 *
 *   make -C sw/apps/web test
 *
 * For every starting position down a document and every scroll
 * distance a user can ask for (an arrow, the wheel's three lines, a
 * page): draw at the start, scroll FORWARD through bv_scroll_to() --
 * the render harness emulates the hardware blit, and refuses it by the
 * same rule the hardware does -- then draw from scratch at the same new
 * position, and require the body to be identical pixel for pixel, the
 * model of the glass identical line for line, and the link rectangles
 * identical. A scroll-by-blit that is one pixel out is torn text on
 * the screen; this is what says it is not.
 *
 * The documents mix what makes line positions irregular: headings in
 * the taller font, gaps between blocks, lists and <pre> that run on
 * without gaps, blocks longer than the view, and links.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../../common/tests/zrender.h"

#include "../page.h"
#include "../html.h"
#include "../layout.h"
#include "../bodyview.h"

static int checks, fails;
static void ck(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; if (fails < 30) printf("FAIL: %s\n", what); }
}

typedef struct { const char *data; uint32_t len; } mem_src_t;

static uint32_t mem_read(void *user, uint32_t off, char *buf, uint32_t len) {
	mem_src_t *m = (mem_src_t *)user;
	if (off >= m->len) return 0;
	if (off + len > m->len) len = m->len - off;
	memcpy(buf, m->data + off, len);
	return len;
}

static z_win_t win;
static z_clip_t content;
static page_t pg;
static mem_src_t src;
static layout_cfg_t cfg;
static layout_hit_t hits[LAYOUT_MAX_HITS];
static int nhits;
static bodyview_t v;

#define VIEW_Y   16
#define BODY_W   (z_win_content_w(&win) - 10)

static uint8_t shot_a[480][640], shot_b[480][640];

static void shoot(uint8_t shot[480][640]) {
	for (int y = content.y0 + VIEW_Y; y < content.y0 + VIEW_Y + v.view_h; y++)
		for (int x = content.x0; x < content.x0 + BODY_W; x++)
			shot[y][x] = (uint8_t)z_render_get(x, y);
}

static bool same_shots(void) {
	for (int y = content.y0 + VIEW_Y; y < content.y0 + VIEW_Y + v.view_h; y++)
		for (int x = content.x0; x < content.x0 + BODY_W; x++)
			if (shot_a[y][x] != shot_b[y][x]) {
				printf("  first differing pixel at (%d, %d)\n", x - content.x0, y - content.y0);
				return false;
			}
	return true;
}

static void setup(const char *doc, uint32_t len, int width) {

	src.data = doc;
	src.len = len;
	page_init(&pg, mem_read, &src);
	page_set_size(&pg, len, true);
	while (!page_indexed(&pg)) page_index_more(&pg, 1000);

	memset(&cfg, 0, sizeof(cfg));
	cfg.x = content.x0 + 2;
	cfg.width = width;
	cfg.body = &z_font_5x8;
	cfg.head = &z_font_6x12;
	cfg.links_live = true;

	memset(&v, 0, sizeof(v));
	v.pg = &pg;
	v.cfg = &cfg;
	v.win = &win;
	v.crect = &content;
	v.view_y = VIEW_Y;
	v.view_h = z_win_content_h(&win) - VIEW_Y;
	v.body_w = BODY_W;
	v.hits = hits;
	v.nhits = &nhits;
	v.max_hits = LAYOUT_MAX_HITS;

}

static int run_doc(const char *doc, uint32_t len, const char *what, int width) {

	static const int dist[] = { 1, 2, 3, 5, 12, 20 };
	page_pos_t start = { 0, 0 };
	int fast = 0, fell_back = 0;
	char label[160];

	setup(doc, len, width);

	for (int step = 0; step < 400; step++) {

		for (unsigned d = 0; d < sizeof(dist) / sizeof(dist[0]); d++) {

			page_pos_t to = start;
			layout_hit_t hits_a[LAYOUT_MAX_HITS];
			int nhits_a, nvis_a;
			uint32_t vblock_a[BV_VIS_MAX];
			uint16_t vsub_a[BV_VIS_MAX];
			int16_t vy_a[BV_VIS_MAX];

			if (page_advance(&pg, &cfg, &to, dist[d]) == 0) continue;

			// (a) draw at the start, then scroll forward by blit
			z_render_clear();
			bv_draw(&v, start);
			if (!bv_scroll_to(&v, to)) { fell_back++; continue; }
			fast++;
			shoot(shot_a);
			nhits_a = nhits;
			memcpy(hits_a, hits, sizeof(hits));
			nvis_a = v.nvis;
			memcpy(vblock_a, v.vblock, sizeof(vblock_a));
			memcpy(vsub_a, v.vsub, sizeof(vsub_a));
			memcpy(vy_a, v.vy, sizeof(vy_a));

			// (b) draw from scratch at the same position
			z_render_clear();
			bv_draw(&v, to);
			shoot(shot_b);

			snprintf(label, sizeof(label), "%s w%d: from block %u line %u, down %d",
				what, width, (unsigned)start.block, (unsigned)start.sub, dist[d]);

			checks++;
			if (!same_shots()) { fails++; printf("FAIL: pixels differ: %s\n", label); }
			if (nvis_a != v.nvis ||
				memcmp(vblock_a, v.vblock, (size_t)nvis_a * sizeof(vblock_a[0])) ||
				memcmp(vsub_a, v.vsub, (size_t)nvis_a * sizeof(vsub_a[0])) ||
				memcmp(vy_a, v.vy, (size_t)nvis_a * sizeof(vy_a[0]))) {
				ck(0, label);
				printf("  model differs: %d lines vs %d\n", nvis_a, v.nvis);
			}
			bool hits_same = (nhits_a == nhits);
			for (int i = 0; hits_same && i < nhits; i++)
				hits_same = hits_a[i].x == hits[i].x && hits_a[i].y == hits[i].y &&
					hits_a[i].w == hits[i].w && hits_a[i].h == hits[i].h &&
					hits_a[i].link == hits[i].link && hits_a[i].block == hits[i].block;
			if (!hits_same) { ck(0, label); printf("  link rects differ: %d vs %d\n", nhits_a, nhits); }

		}

		// walk the start down the document a line at a time
		if (page_advance(&pg, &cfg, &start, 1) == 0) break;

	}

	printf("  %s w%d: %d scrolls by blit, %d fell back to a full draw\n",
		what, width, fast, fell_back);
	return fast;

}

int main(void) {

	static char doc[65536];
	int n = 0, fast;

	if (!z_render_open(&win, 420, 300)) {
		fprintf(stderr, "test_bodyview: cannot map VRAM, skipping\n");
		return 77;
	}
	z_win_content_rect(&win, &content);

	// A long, irregular page.
	n += sprintf(doc + n, "<html><head><title>T</title></head><body>");
	for (int s = 0; s < 6; s++) {
		n += sprintf(doc + n, "<h1>Section %d</h1>", s);
		n += sprintf(doc + n, "<p>Opening paragraph of section %d, long enough to wrap "
			"across several lines at this width, with <a href=\"/a%d\">a link</a> in the "
			"middle and <a href=\"/b%d\">another link that wraps</a> near the end.</p>", s, s, s);
		n += sprintf(doc + n, "<h2>Sub %d</h2><ul>", s);
		for (int i = 0; i < 4; i++)
			n += sprintf(doc + n, "<li>item %d.%d, with a few words</li>", s, i);
		n += sprintf(doc + n, "</ul><pre>");
		for (int i = 0; i < 5; i++) n += sprintf(doc + n, "  code line %d.%d\n", s, i);
		n += sprintf(doc + n, "</pre><blockquote><p>quoted %d</p></blockquote>"
			"<table><tr><td>a</td><td>b</td></tr><tr><td>c</td><td>d</td></tr></table>"
			"<hr><p>short</p>", s);
	}
	n += sprintf(doc + n, "<p>The end.</p></body></html>");

	fast = run_doc(doc, (uint32_t)n, "mixed", 300);
	ck(fast > 100, "the fast path is actually taken");
	fast = run_doc(doc, (uint32_t)n, "mixed", 150);
	ck(fast > 100, "the fast path is taken at a narrow width too");

	// One block far longer than the view: every line of it is mid-block.
	n = sprintf(doc, "<html><body><p>");
	for (int i = 0; i < 300; i++) n += sprintf(doc + n, "word%d ", i);
	n += sprintf(doc + n, "</p></body></html>");
	run_doc(doc, (uint32_t)n, "one-block", 300);

	// A Gopher menu as smallweb.c writes one: a single long <pre>.
	n = sprintf(doc, "<html><head><meta charset=\"utf-8\"></head><body>\n<pre>");
	for (int i = 0; i < 80; i++)
		n += sprintf(doc + n, "[dir] <a href=\"gopher://h/1/%d\">Menu item %d</a>\n", i, i);
	n += sprintf(doc + n, "</pre></body></html>\n");
	run_doc(doc, (uint32_t)n, "gopher-menu", 300);

	// Refusals: a move of a whole screen, and no model.
	{
		page_pos_t a = { 0, 0 }, b = a;
		setup(doc, (uint32_t)n, 300);
		z_render_clear();
		bv_draw(&v, a);
		page_advance(&pg, &cfg, &b, 200);
		ck(!bv_scroll_to(&v, b), "a move past the screen is a full draw");
		bv_invalidate(&v);
		b = a;
		page_advance(&pg, &cfg, &b, 1);
		ck(!bv_scroll_to(&v, b), "no model, no scroll");
	}

	printf("%s: %d checks, %d failures\n", fails ? "FAIL" : "ok", checks, fails);
	return fails ? 1 : 0;

}
