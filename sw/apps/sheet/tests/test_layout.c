/*
 * Layout assertions for sw/apps/sheet.
 *
 *   cc -std=gnu99 -Wall -no-pie -I sw/common -o /tmp/test_layout \
 *      sw/apps/sheet/tests/test_layout.c sw/apps/sheet/sheet_core.c \
 *      sw/common/zwin.c sw/common/zwidget.c sw/common/zfont_data.c \
 *      sw/common/zobj.c sw/common/zeitlos.c sw/common/zfix.c
 *   /tmp/test_layout
 *
 * This does NOT replace tests/render.c, and the render does not
 * replace this. A render catches the relationships nobody thought to
 * write down; this catches the ones that ARE known, across far more
 * window sizes and column widths than anyone will sit and look at.
 *
 * The one worth having above all others is the HIT-TEST ROUND TRIP.
 * Drawing a cell and clicking one are two separate walks over the same
 * variable-width column geometry, and the failure when they disagree
 * is not a crash -- it is a grid where clicking a cell selects its
 * neighbour, which gets lived with and blamed on the mouse rather than
 * reported. sw/apps/hex has the same test for the same reason.
 */

#include "../../../common/tests/zrender.h"
#include "../../../common/tests/ztramp.h"

#define main sheet_main_unused
#include "../sheet.c"
#undef main

// -- stubs -------------------------------------------------------

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

// -- harness -----------------------------------------------------

static int checks, fails;

static void ok(const char *what, bool cond) {

	checks++;

	if (!cond) {
		printf("  FAIL  %s\n", what);
		fails++;
	}

}

static void fail_at(const char *what, int w, int h, int a, int b) {
	printf("  FAIL  %s (win %dx%d): %d vs %d\n", what, w, h, a, b);
	fails++;
}

// Sets the window to a given size and relays out, the way a
// Z_WM_WINDOW_RESIZED would.
static void resize(int w, int h) {

	win.w = (uint32_t)w;
	win.h = (uint32_t)h;

	layout();

}

int main(void) {

	if (!z_render_open(&win, 400, 300)) {
		printf("test_layout: skipped (cannot map the VRAM address)\n");
		return 77;
	}

	if (!z_tramp_install()) {
		printf("test_layout: skipped\n");
		return 77;
	}

	z_scrollbar_init(&vsb, &win, Z_SB_VERT);
	z_scrollbar_init(&hsb, &win, Z_SB_HORZ);

	sheet_init(&sh);

	static const int sizes[][2] = {
		{ WIN_W, WIN_H }, { 400, 300 }, { 512, 384 }, { 640, 480 },
		{ 640, 200 }, { 200, 400 },
		{ Z_WM_MIN_WIDTH, Z_WM_MIN_HEIGHT + 40 },
	};

	static const int fonts[] = { 0, 1 };

	printf("geometry:\n");

	for (unsigned f = 0; f < sizeof(fonts) / sizeof(fonts[0]); f++) {

		cur_font = fonts[f] ? &z_font_6x12 : &z_font_5x8;

		for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {

			int w = sizes[i][0], h = sizes[i][1];

			// Non-default, non-uniform column widths -- the whole point
			// is that the geometry is not a multiplication.
			for (int c = 0; c < SHEET_COLS; c++)
				sheet_set_colw(&sh, c, SHEET_COLW_MIN + (c * 5) % 14);

			resize(w, h);

			int cw = z_win_content_w(&win);
			int ch = z_win_content_h(&win);

			// An empty grid is allowed -- at the smallest sizes the
			// bar, the headers and the scrollbars use everything
			// there is. What is NOT allowed is a grid that extends
			// past the content area, because then it draws over the
			// scrollbar and cell_at() answers for coordinates that
			// are not its own.
			checks++;
			if (grid_w && grid_x + grid_w + Z_SB_THICK > cw)
				fail_at("grid runs under the vertical scrollbar", w, h,
					grid_x + grid_w + Z_SB_THICK, cw);

			checks++;
			if (grid_h && grid_y + grid_h + Z_SB_THICK > ch)
				fail_at("grid runs under the horizontal scrollbar", w, h,
					grid_y + grid_h + Z_SB_THICK, ch);

			// A scrollbar that covers the resize grip makes the window
			// permanently unresizable -- see Z_WIN_GRIP_INSET.
			checks++;
			if (vsb.y + vsb.len > ch - Z_WIN_GRIP_INSET)
				fail_at("vertical scrollbar covers the grip", w, h,
					vsb.y + vsb.len, ch - Z_WIN_GRIP_INSET);

			checks++;
			if (hsb.x + hsb.len > cw - Z_WIN_GRIP_INSET)
				fail_at("horizontal scrollbar covers the grip", w, h,
					hsb.x + hsb.len, cw - Z_WIN_GRIP_INSET);

			checks++;
			if (vis_rows < 0) fail_at("negative visible rows", w, h,
				vis_rows, 0);

			// At any size wm will actually allow, there must be
			// something to look at.
			if (w >= WIN_W && h >= WIN_H) {
				checks++;
				if (vis_rows < 1)
					fail_at("no visible rows at a legal size", w, h,
						vis_rows, 1);
			}

			// -- the hit-test round trip --
			//
			// Every visible cell, probed at each corner of its own
			// rectangle, must come back as itself.
			for (int rr = 0; rr < vis_rows; rr++) {

				int r = top_row + rr;
				int nc = vis_cols();

				for (int cc = 0; cc < nc; cc++) {

					int c = left_col + cc;

					int x = col_x(c);
					int y = row_y(r);

					if (x < 0 || y < 0) continue;

					int cellw = COL_PX(c);
					if (x + cellw > grid_x + grid_w) continue;

					int probes[][2] = {
						{ x, y },
						{ x + cellw - 1, y },
						{ x, y + LINE_H - 1 },
						{ x + cellw - 1, y + LINE_H - 1 },
						{ x + cellw / 2, y + LINE_H / 2 },
					};

					for (unsigned p = 0; p < 5; p++) {

						int gr, gc;

						checks++;

						if (!cell_at(probes[p][0], probes[p][1], &gr, &gc)) {
							printf("  FAIL  no cell at (%d,%d) for %c%d "
								"(win %dx%d)\n", probes[p][0], probes[p][1],
								'A' + c, r + 1, w, h);
							fails++;
							continue;
						}

						if (gr != r || gc != c) {
							printf("  FAIL  click at (%d,%d) hit %c%d, "
								"wanted %c%d (win %dx%d)\n",
								probes[p][0], probes[p][1],
								'A' + gc, gr + 1, 'A' + c, r + 1, w, h);
							fails++;
						}

					}

				}

			}

			// Points outside the grid must not resolve to a cell --
			// otherwise a click on the edit bar or a scrollbar would
			// also move the cursor.
			{
				int gr, gc;

				ok("edit bar is not a cell",
					!cell_at(grid_x + 1, 0, &gr, &gc));
				ok("column header is not a cell",
					!cell_at(grid_x + 1, BAR_H, &gr, &gc));
				ok("row header is not a cell",
					!cell_at(0, grid_y + 1, &gr, &gc));
				ok("right of the grid is not a cell",
					!cell_at(grid_x + grid_w, grid_y + 1, &gr, &gc));
				ok("below the grid is not a cell",
					!cell_at(grid_x + 1, grid_y + grid_h, &gr, &gc));
			}

		}

	}

	// -- scrolling -------------------------------------------------

	printf("scrolling:\n");

	cur_font = &z_font_5x8;

	for (int c = 0; c < SHEET_COLS; c++)
		sheet_set_colw(&sh, c, SHEET_COLW_DEF);

	resize(WIN_W, WIN_H);

	top_row = left_col = 0;

	// Walking the cursor to every cell in the grid must always leave
	// it visible afterwards, at every column width -- this is the
	// property that keyboard-only navigation depends on.
	for (int c = 0; c < SHEET_COLS; c++) {

		cur_col = c;
		cur_row = c * 37;

		scroll_to_cursor();

		checks++;
		if (col_x(cur_col) < 0)
			fail_at("cursor column not visible after scrolling",
				WIN_W, WIN_H, cur_col, left_col);

		checks++;
		if (row_y(cur_row) < 0)
			fail_at("cursor row not visible after scrolling",
				WIN_W, WIN_H, cur_row, top_row);

	}

	// And back again, right to left.
	for (int c = SHEET_COLS - 1; c >= 0; c--) {

		cur_col = c;
		cur_row = 0;

		scroll_to_cursor();

		checks++;
		if (col_x(cur_col) < 0)
			fail_at("cursor column not visible scrolling back",
				WIN_W, WIN_H, cur_col, left_col);

	}

	// A very wide column, wider than the window: it must still be
	// shown (partially) rather than skipped, or it could never be
	// narrowed again.
	sheet_set_colw(&sh, 3, SHEET_COLW_MAX);
	cur_col = 3;
	cur_row = 0;
	scroll_to_cursor();

	ok("an over-wide column is still reachable", col_x(cur_col) >= 0);

	printf("\n%d checks, %s\n", checks, fails ? "FAILURES" : "all passed");

	return fails ? 1 : 0;

}
