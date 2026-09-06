/*
 * Render sw/apps/sheet's grid to an image.
 *
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/render \
 *      sw/apps/sheet/tests/render.c sw/apps/sheet/sheet_core.c \
 *      sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
 *      sw/common/zobj.c sw/common/zeitlos.c sw/common/zfix.c
 *   /tmp/render /tmp/sheet.pbm [win_w win_h font]
 *
 * Needs -no-pie and vm.mmap_min_addr=0 -- see
 * sw/common/tests/ztramp.h.
 *
 * See sw/common/tests/zrender.h for what this does and what it cannot
 * catch. In short: sheet.c, sheet_core.c, zwin.c and zwidget.c are the
 * REAL sources; only the pixel plotting is software.
 *
 * The synthetic sheet below matters as much as the harness. An empty
 * grid would hide most of what is worth looking at -- a number running
 * into its column separator, a right-aligned value that isn't, a
 * column too narrow for its own contents, a selection whose inverted
 * cells swallow the separators around them -- so it has text and
 * numbers of several widths, a formula, an error, a deliberately
 * narrow column, and a live multi-cell selection.
 */

#include "../../../common/tests/zrender.h"
#include "../../../common/tests/ztramp.h"

#define main sheet_main_unused
#include "../sheet.c"
#undef main

// -- everything that would need a filesystem or a message ---------
//
// Do-nothing on purpose: this renders geometry, and anything that
// takes a round trip to wm or the SD card is not geometry.

int fs_open_read(const char *p) { (void)p; return -1; }
int fs_open_write(const char *p) { (void)p; return -1; }
int fs_read_chunk(int h, void *b, int n) { (void)h; (void)b; (void)n; return 0; }
int fs_write_chunk(int h, const void *b, int n) { (void)h; (void)b; return n; }
int fs_close_handle(int h) { (void)h; return 1; }
int fs_sync(int h) { (void)h; return 1; }

const char *z_ftype_ext(const char *path) { (void)path; return NULL; }

bool z_dialog_open(const z_dialog_ctx_t *c, const char *d, char *o, int n) {
	(void)c; (void)d; (void)o; (void)n; return false; }
bool z_dialog_save(const z_dialog_ctx_t *c, const char *d, const char *s,
	char *o, int n) { (void)c; (void)d; (void)s; (void)o; (void)n; return false; }
int z_dialog_confirm(const z_dialog_ctx_t *c, const char *t, const char *m,
	int b) { (void)c; (void)t; (void)m; (void)b; return 0; }
bool z_dialog_prompt(const z_dialog_ctx_t *c, const char *t, const char *m,
	const char *i, char *o, int n) {
	(void)c; (void)t; (void)m; (void)i; (void)o; (void)n; return false; }

static void fill_demo(void) {

	sheet_init(&sh);

	sheet_set(&sh, 0, 0, "Item");
	sheet_set(&sh, 0, 1, "Qty");
	sheet_set(&sh, 0, 2, "Price");
	sheet_set(&sh, 0, 3, "Total");

	static const char *names[] = {
		"widget", "gizmo", "doohickey", "sprocket", "flange", "grommet"
	};

	for (int i = 0; i < 6; i++) {

		char qty[8], price[16], total[24];

		snprintf(qty, sizeof(qty), "%d", (i + 1) * 3);
		snprintf(price, sizeof(price), "%d.%02d", 2 + i * 7, (i * 17) % 100);
		snprintf(total, sizeof(total), "=B%d*C%d", i + 2, i + 2);

		sheet_set(&sh, i + 1, 0, names[i]);
		sheet_set(&sh, i + 1, 1, qty);
		sheet_set(&sh, i + 1, 2, price);
		sheet_set(&sh, i + 1, 3, total);

	}

	sheet_set(&sh, 7, 0, "'total");
	sheet_set(&sh, 7, 1, "=SUM(B2:B7)");
	sheet_set(&sh, 7, 3, "=SUM(D2:D7)");

	// A number far too wide for its column, to see the '#' fill, and
	// an error, to see that it right-aligns like a number.
	sheet_set(&sh, 9, 0, "wide");
	sheet_set(&sh, 9, 1, "123456789.5");
	sheet_set(&sh, 9, 2, "=1/0");
	sheet_set(&sh, 9, 3, "=NOPE(1)");

	// A long label, to see truncation against the next column.
	sheet_set(&sh, 10, 0, "a very long label indeed");

	sheet_set_colw(&sh, 0, 11);
	sheet_set_colw(&sh, 1, 5);

}

int main(int argc, char **argv) {

	const char *out = argc > 1 ? argv[1] : "/tmp/sheet.pbm";
	int w = argc > 2 ? atoi(argv[2]) : WIN_W;
	int h = argc > 3 ? atoi(argv[3]) : WIN_H;
	int big = argc > 4 ? atoi(argv[4]) : 0;

	// Render the EDIT state instead of the browsing one. The edit bar
	// is where all typing happens, so a render that never shows it
	// leaves the most-used part of the window unlooked at -- including
	// the two things about it that can only be judged by eye: the
	// caret's position between characters, and what a formula longer
	// than the field does when it scrolls.
	int edit_mode = argc > 5 ? atoi(argv[5]) : 0;

	if (!z_render_open(&win, w, h)) {
		printf("render: skipped (cannot map the VRAM address)\n");
		return 77;
	}

	// repaint() sends nothing, but update_title() does, and it runs
	// from the same paths a real session would take.
	if (!z_tramp_install()) {
		printf("render: skipped\n");
		return 77;
	}

	if (big) cur_font = &z_font_6x12;

	z_scrollbar_init(&vsb, &win, Z_SB_VERT);
	z_scrollbar_init(&hsb, &win, Z_SB_HORZ);

	fill_demo();

	layout();

	// A selection, parked over a block of numbers rather than at the
	// origin: an inverted cell in the corner proves nothing about how
	// it reads against its neighbours' separators.
	cur_row = 4;
	cur_col = 2;
	sel_row = 2;
	sel_col = 1;

	if (edit_mode) {

		// A formula comfortably longer than the bar, with the caret
		// parked mid-string rather than at either end.
		cur_row = 7;
		cur_col = 3;
		sel_row = cur_row;
		sel_col = cur_col;

		begin_edit(false);

		const char *f = "=SUM(D2:D7)+MAX(B2:B7)*2-ROUND(C4,2)";
		for (const char *q = f; *q; q++) edit_insert(*q);

		edit_caret = 18;

	}

	scroll_to_cursor();
	repaint();

	z_render_write(out, &win, 2);

	printf("render: %dx%d window, %d rows, %d cols visible, font %dx%d\n",
		w, h, vis_rows, vis_cols(), cur_font->w, cur_font->h);

	return 0;

}
