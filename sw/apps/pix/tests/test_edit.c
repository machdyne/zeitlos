/*
 * pix -- the tools and undo, on the host.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../px_doc.h"
#include "../px_edit.h"

static int checks, failures;

#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; \
	printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int count(const px_doc_t *d, int i) {
	int x, y, n = 0;
	for (y = 0; y < d->cell[i].h; y++)
		for (x = 0; x < d->cell[i].w; x++) n += px_get(d, i, x, y);
	return n;
}

int main(void) {
	px_doc_t d;
	px_piece_t p = { 0, 0, NULL };
	int i, x, y, k;

	px_doc_init(&d, PX_ZBM);          /* depth 1: two colours */
	i = px_cell_insert(&d, -1, 20, 12, "a");

	/* lines: endpoints and pixel count of a diagonal, and the preview
	 * agrees with what is drawn */
	px_line(&d, i, 1, 1, 10, 7, 1);
	CHECK(px_get(&d, i, 1, 1) && px_get(&d, i, 10, 7), "line ends");
	CHECK(count(&d, i) == 10, "line has 10 pixels (%d)", count(&d, i));
	for (y = 0; y < 12; y++)
		for (x = 0; x < 20; x++)
			CHECK(px_get(&d, i, x, y) == (int)px_on_line(1, 1, 10, 7, x, y), "preview %d,%d", x, y);

	/* rectangle outline then fill inside it */
	memset(d.bits, 0, d.bits_len);
	px_rect(&d, i, 15, 10, 2, 2, 1, false);         /* corners in any order */
	CHECK(count(&d, i) == 2 * 14 + 2 * 7, "outline (%d)", count(&d, i));
	CHECK(px_flood(&d, i, 5, 5, 1), "flood inside");
	CHECK(count(&d, i) == 14 * 9, "filled (%d)", count(&d, i));
	CHECK(!px_flood(&d, i, 5, 5, 1), "flood of the same colour does nothing");
	CHECK(px_flood(&d, i, 0, 0, 1) && count(&d, i) == 240, "flood outside");

	/* pieces */
	memset(d.bits, 0, d.bits_len);
	px_set(&d, i, 3, 2, 1);                         /* an L in a 4x3 piece at 3,2 */
	px_set(&d, i, 3, 3, 1);
	px_set(&d, i, 3, 4, 1);
	px_set(&d, i, 4, 4, 1);
	CHECK(px_piece_take(&p, &d, i, 3, 2, 4, 3), "take");
	CHECK(px_piece_get(&p, 0, 0) && px_piece_get(&p, 1, 2) && !px_piece_get(&p, 1, 0), "taken");
	px_piece_flip_h(&p);
	CHECK(px_piece_get(&p, 3, 0) && px_piece_get(&p, 2, 2) && !px_piece_get(&p, 0, 0), "flip h");
	px_piece_flip_h(&p);
	px_piece_flip_v(&p);
	CHECK(px_piece_get(&p, 0, 2) && px_piece_get(&p, 1, 0), "flip v");
	px_piece_flip_v(&p);
	CHECK(px_piece_rotate(&p) && p.w == 3 && p.h == 4, "rotate: 3 x 4");
	/* (0,0) -> (2,0); (0,2) -> (0,0); (1,2) -> (0,1) */
	CHECK(px_piece_get(&p, 2, 0) && px_piece_get(&p, 0, 0) && px_piece_get(&p, 0, 1), "rotated");
	for (k = 0; k < 3; k++) px_piece_rotate(&p);
	CHECK(p.w == 4 && px_piece_get(&p, 0, 0) && px_piece_get(&p, 1, 2), "four turns");
	px_piece_shift(&p, -1, 1);
	CHECK(px_piece_get(&p, 3, 1) && px_piece_get(&p, 0, 0), "shift wraps");
	px_piece_shift(&p, 1, -1);
	memset(d.bits, 0, d.bits_len);
	px_piece_put(&p, &d, i, 18, 10, false);         /* half off the cell: clipped */
	CHECK(px_get(&d, i, 18, 10) && px_get(&d, i, 18, 11) && count(&d, i) == 2, "put clips");
	px_piece_free(&p);

	/* undo and redo, through a resize */
	px_undo_clear();
	memset(d.bits, 0, d.bits_len);
	px_undo_save(&d, i);
	px_set(&d, i, 7, 7, 1);
	px_undo_save(&d, i);
	CHECK(px_cell_resize(&d, i, 12, 20), "resize");
	CHECK(px_get(&d, i, 7, 7) == 1, "resize keeps what fits");
	CHECK(px_undo(&d, false) == i && d.cell[i].w == 20 && px_get(&d, i, 7, 7), "undo the resize");
	CHECK(px_undo(&d, false) == i && count(&d, i) == 0, "undo the pixel");
	CHECK(px_undo(&d, false) == -1, "nothing more");
	CHECK(px_undo(&d, true) == i && px_get(&d, i, 7, 7), "redo");
	CHECK(px_undo_depth(true) == 1 && px_undo_depth(false) == 1, "depths");
	px_undo_save(&d, i);                       /* an edit that changes nothing */
	px_undo_settle(&d);
	CHECK(px_undo_depth(true) == 1, "an edit that changed nothing leaves redo");
	px_undo_save(&d, i);
	px_set(&d, i, 2, 2, 1);
	px_undo_settle(&d);
	CHECK(px_undo_depth(true) == 0, "a new edit clears redo");
	for (k = 0; k < PX_UNDO_STEPS + 10; k++) { px_undo_save(&d, i); px_set(&d, i, k % 12, k / 12, 1); }
	px_undo_settle(&d);
	CHECK(px_undo_depth(false) == PX_UNDO_STEPS, "at most %d steps (%d)", PX_UNDO_STEPS, px_undo_depth(false));
	px_undo_save(&d, i);                                   /* a step that changes nothing */
	px_undo_settle(&d);
	CHECK(px_undo_depth(false) == PX_UNDO_STEPS, "a step that changed nothing is not kept");
	px_undo_clear();

	/* a big picture: a step keeps only what it changed */
	{
		px_doc_t b;
		int j, n;
		uint32_t before;
		px_doc_init(&b, PX_ZBM);
		j = px_cell_insert(&b, -1, 1024, 1024, "big");
		for (n = 0; n < 40; n++) {
			px_undo_save(&b, j);
			px_line(&b, j, 100 + n * 10, 100, 120 + n * 10, 300, 1);
		}
		px_undo_settle(&b);
		CHECK(px_undo_depth(false) == 40, "40 lines, 40 steps (%d)", px_undo_depth(false));
		CHECK(px_undo_held() < 40u * 201u * 4u, "about a column of bytes a line, not 128 KB (%u)", px_undo_held());
		before = (uint32_t)count(&b, j);
		CHECK(before > 40 * 200, "drawn (%u)", before);
		for (n = 0; n < 40; n++) CHECK(px_undo(&b, false) == j, "undo %d", n);
		CHECK(count(&b, j) == 0, "all undone (%d left)", count(&b, j));
		for (n = 0; n < 40; n++) px_undo(&b, true);
		CHECK((uint32_t)count(&b, j) == before, "and redone (%d of %u)", count(&b, j), before);
		/* the steps redo kept were parts too */
		CHECK(px_undo_held() < 40u * 201u * 4u, "redo's steps are parts too (%u)", px_undo_held());
		px_undo_clear();
		px_doc_free(&b);
	}

	/* resize with other cells around: their pixels stay put */
	{
		int j = px_cell_insert(&d, -1, 9, 9, "b");
		px_set(&d, j, 8, 8, 1);
		px_cell_resize(&d, i, 33, 3);
		CHECK(px_get(&d, j, 8, 8) == 1 && count(&d, j) == 1, "b untouched");
	}

	px_doc_free(&d);

	/* depth 16: colours and transparency */
	px_doc_init(&d, PX_ZSP);
	zsp_palette_init(&d.pal, 16);
	i = px_cell_insert(&d, -1, 10, 6, "s");
	CHECK(px_get(&d, i, 3, 3) == ZSP_T && px_get(&d, i, -1, 0) == ZSP_T, "a new sprite is transparent");
	px_rect(&d, i, 1, 1, 8, 4, 9, false);
	CHECK(px_flood(&d, i, 4, 2, 12) && px_get(&d, i, 4, 2) == 12 && px_get(&d, i, 1, 1) == 9 &&
		px_get(&d, i, 0, 0) == ZSP_T, "fill inside a colour 9 outline");
	CHECK(px_flood(&d, i, 0, 0, 3) && px_get(&d, i, 9, 5) == 3 && px_get(&d, i, 5, 2) == 12,
		"fill the transparent outside");
	px_set(&d, i, 0, 0, ZSP_T);
	CHECK(px_piece_take(&p, &d, i, 0, 0, 3, 3), "take");
	CHECK(px_piece_get(&p, 0, 0) == ZSP_T && px_piece_get(&p, 1, 1) == 9 && px_piece_get(&p, 2, 2) == 12, "taken");
	px_set(&d, i, 7, 3, 5);
	px_piece_put(&p, &d, i, 6, 2, true);                  /* the transparent corner keeps what is there */
	CHECK(px_get(&d, i, 6, 2) == 12 && px_get(&d, i, 7, 3) == 9, "a transparent pixel put down leaves the cell's");
	px_undo_clear();
	px_undo_save(&d, i);
	px_set(&d, i, 5, 5, 200);
	CHECK(px_get(&d, i, 5, 5) == 200, "any index");
	CHECK(px_undo(&d, false) == i && px_get(&d, i, 5, 5) == 3, "undone");
	CHECK(px_cell_resize(&d, i, 12, 7) && px_get(&d, i, 11, 6) == ZSP_T && px_get(&d, i, 1, 1) == 9,
		"resized: the new part transparent");
	px_piece_free(&p);
	px_undo_clear();
	px_doc_free(&d);

	printf("pix edit: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
