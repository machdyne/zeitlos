/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Draws a real web page on the build machine and writes it out as an
 * image, so the layout can be LOOKED AT before it reaches a screen.
 * See sw/common/tests/zrender.h for why this exists at all.
 *
 *   make render DOC=page.html
 *   make render DOC=page.html SCROLL=40 SCALE=2
 *
 * Linking is in the Makefile's `render` target. Note the absence of
 * zgfx.c: zrender.h supplies software pixel primitives, because
 * zgfx.c programs the GPU and on a build machine that writes to
 * unmapped MMIO and draws nothing.
 *
 * -- what this catches that an assertion cannot --
 *
 * Everything about the page at once. Whether a wrapped list item
 * lines up under its own text or under its bullet. Whether a link
 * underline collides with the line below. Whether a heading's rule
 * runs the full width or stops at the text. Whether an infobox table
 * is readable or a column of pipes. None of those are things anybody
 * writes an assertion for in advance, and all of them are obvious in
 * one look.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/tests/zrender.h"

#include "../html.h"
#include "../layout.h"

// Matched to web.c's default window on purpose. The whole value of
// this renderer is that what it draws is what the app draws; a
// different width here would quietly make it a different test.
#define WIN_W  540
#define WIN_H  400

// Blocks held for drawing. The real app streams from a file and keeps
// only what is on screen; this holds a window's worth plus slack so
// scrolling to an arbitrary offset needs one parse.
#define MAX_BLOCKS 20000

static html_line_t *blocks;
static int nblocks;

static void collect(void *user, const html_line_t *l) {
	(void)user;
	if (nblocks < MAX_BLOCKS) blocks[nblocks++] = *l;
}

static char filebuf[8 * 1024 * 1024];

int main(int argc, char **argv) {

	z_win_t win;
	z_clip_t content;
	layout_cfg_t cfg;
	html_ctx_t ctx;
	FILE *f;
	uint32_t flen;
	int scroll = 0, scale = 2;
	int y, i;
	layout_hit_t hits[LAYOUT_MAX_HITS];
	int nhits = 0;
	const char *path;
	const char *out = "/tmp/web.pbm";
	int prev_kind = -1;

	if (argc < 2) {
		fprintf(stderr, "usage: render <file.html> [scroll] [scale] [out]\n");
		return 2;
	}

	path = argv[1];
	if (argc > 2) scroll = atoi(argv[2]);
	if (argc > 3) scale = atoi(argv[3]);
	if (argc > 4) out = argv[4];

	f = fopen(path, "rb");
	if (!f) { perror(path); return 1; }
	flen = (uint32_t)fread(filebuf, 1, sizeof(filebuf), f);
	fclose(f);

	blocks = calloc(MAX_BLOCKS, sizeof(html_line_t));
	if (!blocks) { fprintf(stderr, "out of memory\n"); return 1; }

	html_init(&ctx, collect, NULL);
	html_feed(&ctx, filebuf, flen);
	html_finish(&ctx);

	fprintf(stderr, "render: %s -- %u bytes, %d blocks, title \"%s\"\n",
		path, (unsigned)flen, nblocks, html_title(&ctx));

	// MAP_FIXED_NOREPLACE at a low address is Linux/x86-64 only;
	// zrender.h asks callers to exit 77 elsewhere so CI skips rather
	// than fails.
	if (!z_render_open(&win, WIN_W, WIN_H)) {
		fprintf(stderr, "render: cannot map VRAM (not Linux/x86-64?)\n");
		return 77;
	}

	z_win_content_rect(&win, &content);

	memset(&cfg, 0, sizeof(cfg));
	cfg.x = content.x0 + 2;
	cfg.width = (content.x1 - content.x0 + 1) - 4;
	cfg.body = &z_font_5x8;
	cfg.head = &z_font_6x12;
	cfg.links_live = true;

	y = content.y0 + 2;

	for (i = scroll; i < nblocks && y < content.y1; i++) {

		const html_line_t *l = &blocks[i];
		int gap = layout_gap_before(l, &cfg);
		int n, avail;

		// Consecutive blocks of the same kind are one structure -- a
		// list, a table -- so the inter-block gap is suppressed
		// between them. Without this a ten-row table is spread over
		// two screens for no reason.
		if ((int)l->kind == prev_kind &&
			(l->kind == HTML_LIST || l->kind == HTML_TABLE ||
			 l->kind == HTML_PRE))
			gap = 0;

		if (i > scroll) y += gap;

		avail = (content.y1 - y + 1) / layout_line_height(l, &cfg);
		if (avail <= 0) break;

		n = layout_draw(l, &cfg, y, 0, avail, &content,
			(uint16_t)i, hits, &nhits, LAYOUT_MAX_HITS);

		y += n * layout_line_height(l, &cfg);
		prev_kind = (int)l->kind;

	}

	fprintf(stderr, "render: drew blocks %d..%d, %d link rects\n",
		scroll, i - 1, nhits);

	z_render_write(out, &win, scale);

	free(blocks);
	return 0;

}
