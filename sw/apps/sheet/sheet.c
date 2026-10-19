/*
 * sheet -- a spreadsheet
 *
 * Cells, formulas, and a grid you can drive with or without a mouse.
 *
 *   > run wm
 *   > run sheet
 *
 * -- layout --
 *
 *   +-----------------------------------+
 *   | A1  =SUM(B1:B9)                   |   edit bar
 *   +---+-------+-------+-------+---+---+
 *   |   |A      |B      |C      |   |#|     column headers
 *   | 1 |Item   |  12.5 |       |   | |
 *   | 2 |Widget |   8.0 |       |   | |
 *   | 3 |Total  |  20.5 |       |   | |
 *   +---+-------+-------+-------+---+-+
 *   |  <horizontal scrollbar>       |     |
 *   +-----------------------------------+
 *
 * The window is resizable; everything is recomputed from its size in
 * layout(). Both scrollbars stop short of the resize grip
 * (Z_WIN_GRIP_INSET, zwm.h) -- a scrollbar that covers the corner
 * swallows it and the window can never be resized again.
 *
 * -- the edit bar --
 *
 * Editing happens in the bar at the top, not in the cell. That is how
 * every spreadsheet before the mid-eighties worked, and here it is the
 * right answer for a concrete reason rather than nostalgia: a formula
 * is routinely wider than the column it lives in, and an in-cell
 * editor would either have to overflow into its neighbours (repainting
 * cells that are not its own) or scroll a nine-character window over
 * the text being typed. The bar has the whole width of the window.
 *
 * -- what is in the model and what is here --
 *
 * Everything about cells, formulas, evaluation and files is in
 * sheet_core.c and is tested on the build machine (tests/test_core.c).
 * This file is the window: layout, drawing, input, dialogs. The split
 * is not tidiness -- arithmetic nobody re-checks is exactly what has
 * to be testable without hardware.
 *
 * -- drawing --
 *
 * Every glyph goes through the hardware glyph blitter, via
 * z_win_draw_text()/z_win_draw_text2() (-DZ_GFX_HW_BLIT, see the
 * Makefile). A cursor move repaints two cells and the edit bar, not
 * the grid; anything that can change a value anywhere (an edit, a
 * load, a scroll) repaints the grid, because a formula in any cell can
 * depend on any other.
 *
 * The panel can be drawn on the build machine and looked at without a
 * board -- see tests/render.c and sw/common/tests/zrender.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zpaths.h"
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"			// Z_TICK_HZ
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zgfx.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zwidget.h"
#include "../../common/zflist.h"
#include "../../common/zdialog.h"
#include "../../common/zfsapp.h"
#include "../../common/ztype.h"

#include "sheet_core.h"
#include "chart.h"

// -- the document --

static sheet_t sh;

static char filename[Z_FS_PATH_MAX];		// "" when never saved

// -- window --

// Minimum useful size, pinned as the floor by Z_WIN_FLAG_MIN_IS_CREATE
// -- below roughly this, the row header plus two columns plus a
// scrollbar stop fitting across, and the titlebar stops having room
// for its five icons.
#define WIN_W   360
#define WIN_H   240

static z_win_t win;

// -- fonts and metrics --
//
// Both fonts are resident in glyph memory, loaded once by wm (see
// glyph_layout[] in sw/common/zgfx.c), so both render through the
// hardware blitter. Everything below is derived from whichever is
// current, because a font switch changes the column count, the row
// height, and which cell a click lands on.
static const z_font_t *cur_font = &z_font_5x8;

#define CHAR_W    (cur_font->w)
#define LINE_H    (cur_font->h + 2)

// Edit bar height, and the width of the reference box at its left.
#define BAR_H     (cur_font->h + 5)
#define REF_W     (6 * CHAR_W + 4)

// Row header: three digits plus a little air.
#define ROWHDR_W  (3 * CHAR_W + 6)

// Pixels a column of `w` characters occupies, including its 1px
// separator line. One function, used by drawing and by hit testing --
// see tests/test_layout.c on why those two agreeing matters more than
// anything else here.
#define COL_PX(c) (sheet_colw(&sh, (c)) * CHAR_W + 4)

// -- viewport --

static int top_row, left_col;		// first visible row/column
static int cur_row, cur_col;		// the cursor
static int sel_row, sel_col;		// the other corner of the selection

static int grid_x, grid_y;			// content-relative origin of the cells
static int grid_w, grid_h;			// pixels available to them
static int vis_rows;				// whole rows that fit

static z_scrollbar_t vsb, hsb;

// -- editing --

static bool editing;
static char edit[SHEET_SRC_MAX];
static int edit_len, edit_caret;

// Modifier state from the last key event, so shift+click can be told
// from a plain one -- Z_WM_MOUSE carries buttons but no modifiers.
static uint8_t mods_now;

static uint8_t last_buttons;

// Column-width drag, started on a column header's right edge.
static int width_drag_col = -1;
static int width_drag_x0, width_drag_w0;

// Cell-selection drag.
static bool selecting;

static void repaint(void);
static void repaint_view(void);
static void draw_bar(void);
static void draw_cell(int r, int c);
static void draw_headers(void);

// ---------------------------------------------------------------
// layout
// ---------------------------------------------------------------

static void layout(void) {

	int cw = z_win_content_w(&win);
	int ch = z_win_content_h(&win);

	grid_x = ROWHDR_W;
	grid_y = BAR_H + LINE_H;

	grid_w = cw - grid_x - Z_SB_THICK;
	grid_h = ch - grid_y - Z_SB_THICK;

	// CLAMPED TO ZERO, NOT TO ONE CELL.
	//
	// The obvious floor -- "at least one row high, at least one
	// character wide" -- is wrong, and tests/test_layout.c caught it
	// at the smallest window wm will allow: when the edit bar, the
	// header row and the scrollbars already use more than the window
	// has, a minimum makes the grid EXTEND PAST the content area
	// instead of shrinking, so it draws over the scrollbar and
	// cell_at() answers for coordinates that are not the grid's.
	//
	// Zero is the honest answer at that size: nothing is drawn,
	// nothing is clickable, and the numbers stay sound. The window
	// cannot actually get there (Z_WIN_FLAG_MIN_IS_CREATE pins the
	// floor at WIN_W x WIN_H), which is exactly why it is worth
	// getting right -- an unreachable size that underflows is a bug
	// waiting for the day the floor changes.
	if (grid_w < 0) grid_w = 0;
	if (grid_h < 0) grid_h = 0;

	vis_rows = grid_h / LINE_H;

	// Both bars stop short of the resize grip. See Z_WIN_GRIP_INSET.
	int vlen = ch - grid_y - Z_WIN_GRIP_INSET;
	int hlen = cw - grid_x - Z_WIN_GRIP_INSET;

	if (vlen < 0) vlen = 0;
	if (hlen < 0) hlen = 0;

	z_scrollbar_set_geom(&vsb, cw - Z_SB_THICK, grid_y, vlen);
	z_scrollbar_set_geom(&hsb, grid_x, ch - Z_SB_THICK, hlen);

}

// How many columns are fully visible starting at left_col. At least
// one, always -- a column wider than the window shows partially rather
// than not at all, which is what makes it possible to see the column
// you are about to narrow.
static int vis_cols(void) {

	int x = 0, n = 0;

	for (int c = left_col; c < SHEET_COLS; c++) {
		int w = COL_PX(c);
		if (x + w > grid_w) break;
		x += w;
		n++;
	}

	// One, so a column wider than the window is shown partially rather
	// than not at all -- otherwise it could never be narrowed again.
	// None at all only when there is no grid to show it in.
	if (!n) return grid_w > 0 ? 1 : 0;

	return n;

}

// Content-relative x of column `c`, or -1 if it is not on screen.
static int col_x(int c) {

	if (c < left_col) return -1;

	int x = grid_x;

	for (int i = left_col; i < c; i++) {
		x += COL_PX(i);
		if (x >= grid_x + grid_w) return -1;
	}

	return x < grid_x + grid_w ? x : -1;

}

static int row_y(int r) {

	if (r < top_row || r >= top_row + vis_rows) return -1;
	return grid_y + (r - top_row) * LINE_H;

}

// Which cell is at (cx, cy), content-relative. Returns false for a
// point outside the grid. The exact inverse of col_x()/row_y(), and
// tests/test_layout.c checks that on every column width.
static bool cell_at(int cx, int cy, int *row, int *col) {

	if (cx < grid_x || cy < grid_y) return false;
	if (cx >= grid_x + grid_w || cy >= grid_y + grid_h) return false;

	int r = top_row + (cy - grid_y) / LINE_H;
	if (r >= SHEET_ROWS) return false;

	int x = grid_x;
	int c = left_col;

	for (; c < SHEET_COLS; c++) {
		int w = COL_PX(c);
		if (cx < x + w) break;
		x += w;
	}

	if (c >= SHEET_COLS) return false;

	*row = r;
	*col = c;

	return true;

}

// ---------------------------------------------------------------
// scrolling
// ---------------------------------------------------------------

// The scrollable extent: the used area plus a margin, so there is
// always somewhere to go past the last thing you typed. Never less
// than a page, or the scrollbar would draw a thumb longer than its
// own track.
static void update_scroll_range(void) {

	int rows, cols;
	sheet_extent(&sh, &rows, &cols);

	if (cur_row + 1 > rows) rows = cur_row + 1;
	if (cur_col + 1 > cols) cols = cur_col + 1;

	rows += 8;
	if (rows > SHEET_ROWS) rows = SHEET_ROWS;

	// Columns scroll over the WHOLE grid, rows only over what is used
	// plus a margin. The asymmetry is deliberate: 26 columns divided
	// by the handful on screen still leaves a thumb big enough to
	// grab, so a pointer can reach column Z; 999 rows would leave a
	// thumb about two percent of the track, which is unusable for the
	// twenty-row sheet that is the common case. A keyboard reaches
	// every row regardless -- the cursor scrolls the view with it.
	cols = SHEET_COLS;

	int page_c = vis_cols();

	if (rows < vis_rows) rows = vis_rows;
	if (cols < page_c) cols = page_c;
	if (rows < 1) rows = 1;

	z_scrollbar_set_range(&vsb, rows, vis_rows);
	z_scrollbar_set_range(&hsb, cols, page_c);

	z_scrollbar_set_value(&vsb, top_row);
	z_scrollbar_set_value(&hsb, left_col);

}

// Brings the cursor into view. Returns true if anything moved, so the
// caller knows whether a full grid repaint is needed rather than the
// two-cell one.
static bool scroll_to_cursor(void) {

	int old_top = top_row, old_left = left_col;

	// A page of at least one row, even when none is visible: with a
	// literal zero here the cursor would scroll the view one row PAST
	// itself on every move.
	int page_r = vis_rows > 0 ? vis_rows : 1;

	if (cur_row < top_row) top_row = cur_row;
	if (cur_row >= top_row + page_r) top_row = cur_row - page_r + 1;

	if (top_row < 0) top_row = 0;

	if (cur_col < left_col) left_col = cur_col;

	// Widen rightwards one column at a time: with variable widths
	// there is no arithmetic for "how far left must I start", and the
	// loop is at most SHEET_COLS steps.
	while (cur_col >= left_col + vis_cols() && left_col < cur_col)
		left_col++;

	update_scroll_range();

	return top_row != old_top || left_col != old_left;

}

// ---------------------------------------------------------------
// title
// ---------------------------------------------------------------

static void build_title(char *t, int cap, const char *path, bool star) {
	z_win_doc_title(t, cap, path, star, "untitled");
}

// What was last sent, so an unchanged title costs no message. wm
// repairs the titlebar strip on every Z_WM_SET_TITLE and tells the
// windows underneath to repaint; doing that on every keystroke would
// be a visible flicker across the whole screen.
static char sent_title[40];

static void update_title(void) {

	char t[64];

	build_title(t, (int)sizeof(t), filename, sh.modified);

	if (!strcmp(t, sent_title)) return;

	strcpy(sent_title, t);
	z_win_set_title(&win, t);

}

// ---------------------------------------------------------------
// drawing
// ---------------------------------------------------------------

// Selection rectangle, normalised.
static void sel_rect(int *r0, int *c0, int *r1, int *c1) {

	*r0 = cur_row < sel_row ? cur_row : sel_row;
	*r1 = cur_row < sel_row ? sel_row : cur_row;
	*c0 = cur_col < sel_col ? cur_col : sel_col;
	*c1 = cur_col < sel_col ? sel_col : cur_col;

}

static bool in_sel(int r, int c) {

	int r0, c0, r1, c1;
	sel_rect(&r0, &c0, &r1, &c1);

	return r >= r0 && r <= r1 && c >= c0 && c <= c1;

}

static void draw_cell(int r, int c) {

	int x = col_x(c);
	int y = row_y(r);

	if (x < 0 || y < 0) return;

	int w = COL_PX(c);

	// Clip the last column to the grid rather than letting it run
	// under the scrollbar.
	if (x + w > grid_x + grid_w) w = grid_x + grid_w - x;
	if (w <= 0) return;

	// The cursor cell is drawn NORMAL inside a multi-cell selection,
	// so it reads as a hole in the highlight -- which is how every
	// spreadsheet marks the active cell of a range, and the only way
	// to see where typing would go without a second kind of marker.
	// A single-cell selection IS the cursor, so there it inverts.
	bool multi = (sel_row != cur_row || sel_col != cur_col);
	bool sel = in_sel(r, c) && !(multi && r == cur_row && c == cur_col);

	int bg = sel ? 1 : 0;
	int fg = sel ? 0 : 1;

	z_win_fill_rect(&win, x, y, w, LINE_H, bg);

	// The column separator, drawn in the cell's own background so a
	// selected cell doesn't get a stripe through it.
	z_win_fill_rect(&win, x + w - 1, y, 1, LINE_H, sel ? 0 : 1);

	char buf[SHEET_SRC_MAX];
	bool right = false;

	int chars = (w - 4) / CHAR_W;
	if (chars < 1) return;

	int n = sheet_display(&sh, r, c, buf, sizeof(buf), chars, &right);
	if (!n) return;

	int tx = right ? x + w - 3 - n * CHAR_W : x + 2;

	z_win_draw_text2(&win, tx, y + 1, buf, fg, bg, cur_font);

}

static void draw_headers(void) {

	int cw = z_win_content_w(&win);

	// The corner, above the row numbers and left of the column
	// letters. Background, not ink: a solid block there is the
	// heaviest thing on the screen and it labels nothing.
	z_win_fill_rect(&win, 0, BAR_H, grid_x, LINE_H, 0);
	z_win_fill_rect(&win, grid_x - 1, BAR_H, 1, LINE_H, 1);

	// Column headers. The cursor's own column is drawn inverted, which
	// is most of what makes the grid readable without a mouse pointer
	// to look at.
	int nc = vis_cols();

	for (int i = 0; i < nc; i++) {

		int c = left_col + i;
		int x = col_x(c);
		if (x < 0) break;

		int w = COL_PX(c);
		if (x + w > grid_x + grid_w) w = grid_x + grid_w - x;
		if (w <= 0) break;

		bool hot = (c == cur_col);

		z_win_fill_rect(&win, x, BAR_H, w, LINE_H, hot ? 1 : 0);
		z_win_fill_rect(&win, x + w - 1, BAR_H, 1, LINE_H, hot ? 0 : 1);

		char nm[2];
		nm[0] = (char)('A' + c);
		nm[1] = 0;

		z_win_draw_text2(&win, x + (w - CHAR_W) / 2, BAR_H + 1, nm,
			hot ? 0 : 1, hot ? 1 : 0, cur_font);

	}

	// Fill any slack right of the last column, so a resize doesn't
	// leave the previous header row behind.
	{
		int x = grid_x;
		for (int i = 0; i < nc; i++) x += COL_PX(left_col + i);
		if (x < grid_x + grid_w)
			z_win_fill_rect(&win, x, BAR_H, grid_x + grid_w - x, LINE_H, 0);
	}

	// The line under the whole header strip.
	z_win_fill_rect(&win, 0, BAR_H + LINE_H - 1, cw, 1, 1);

	// Row headers.
	for (int i = 0; i < vis_rows; i++) {

		int r = top_row + i;
		if (r >= SHEET_ROWS) break;

		int y = grid_y + i * LINE_H;
		bool hot = (r == cur_row);

		z_win_fill_rect(&win, 0, y, grid_x, LINE_H, hot ? 1 : 0);
		z_win_fill_rect(&win, grid_x - 1, y, 1, LINE_H, hot ? 0 : 1);

		char num[6];
		int t = 0, v = r + 1;
		char tmp[4];
		int tn = 0;

		while (v && tn < 4) { tmp[tn++] = (char)('0' + v % 10); v /= 10; }
		while (tn) num[t++] = tmp[--tn];
		num[t] = 0;

		z_win_draw_text2(&win, grid_x - 3 - t * CHAR_W, y + 1, num,
			hot ? 0 : 1, hot ? 1 : 0, cur_font);

	}

}

static void draw_menu(void);
static int menu;
static bool chart_mode;

static void draw_bar(void) {

	if (menu) { draw_menu(); return; }
	if (chart_mode) return;

	int cw = z_win_content_w(&win);

	z_win_fill_rect(&win, 0, 0, cw, BAR_H, 0);

	// The reference box: the cursor, or the selection if there is one.
	char ref[16];
	int n = sheet_ref_name(cur_row, cur_col, ref, sizeof(ref));

	if (!editing && (sel_row != cur_row || sel_col != cur_col)) {

		int r0, c0, r1, c1;
		sel_rect(&r0, &c0, &r1, &c1);

		n = sheet_ref_name(r0, c0, ref, sizeof(ref));
		if (n < (int)sizeof(ref) - 6) {
			ref[n++] = ':';
			n += sheet_ref_name(r1, c1, ref + n, (int)sizeof(ref) - n);
		}

	}

	z_win_draw_text(&win, 2, 2, ref, 1, cur_font);
	z_win_fill_rect(&win, REF_W - 2, 0, 1, BAR_H, 1);

	// The content: what is being typed, or the cell's own source.
	const char *text = editing ? edit : sheet_src(&sh, cur_row, cur_col);

	// A selection shows what is in it, as a line of arithmetic nobody
	// has to type: sum, average and count of its numbers.
	char stats[64];
	if (!editing && (sel_row != cur_row || sel_col != cur_col)) {
		int r0, c0, r1, c1, count = 0;
		z_fix_t sum = 0;
		sel_rect(&r0, &c0, &r1, &c1);
		int n2 = 0;
		if (sheet_range_stats(&sh, r0, c0, r1, c1, &sum, &count) && count) {
			const char *parts[] = { "Sum ", NULL, "  Avg ", NULL, "  Count ", NULL };
			char a[24], b[24], c[12];
			z_fix_format(sum, SHEET_DP, Z_FIX_STRIP, a, sizeof a);
			z_fix_t avg = sum / count;
			z_fix_format(avg, SHEET_DP, 2, b, sizeof b);
			int cn = 0, cv = count;
			char t[12];
			int tn = 0;
			while (cv && tn < 11) { t[tn++] = (char)('0' + cv % 10); cv /= 10; }
			while (tn) c[cn++] = t[--tn];
			c[cn] = 0;
			parts[1] = a; parts[3] = b; parts[5] = c;
			for (int i = 0; i < 6; i++)
				for (const char *q = parts[i]; *q && n2 < (int)sizeof(stats) - 1; q++) stats[n2++] = *q;
		}
		stats[n2] = 0;
		text = stats;
	}

	int avail = (cw - REF_W - 4) / CHAR_W;
	if (avail < 1) avail = 1;

	// Scroll the field so the caret stays in it. Only ever needed
	// while editing; a stored source is shown from the start.
	int from = 0;

	if (editing && edit_caret > avail - 1) from = edit_caret - avail + 1;

	char shown[SHEET_SRC_MAX];
	int sn = 0;

	for (int i = from; text[i] && sn < avail && sn < (int)sizeof(shown) - 1; i++)
		shown[sn++] = text[i];

	shown[sn] = 0;

	// The text starts one pixel right of the caret column, so the two
	// never share a pixel.
	//
	// The glyph blitter paints a SOLID CELL -- a character owns every
	// column of its own width, and the blank spacing column belongs to
	// the character on its left. So a caret drawn at the start of
	// glyph N lands on top of glyph N's first column, which for most
	// letters is ink: the render showed the caret sitting inside the
	// ':' of a range rather than beside it. Offsetting the text by one
	// puts the caret in the previous cell's trailing blank instead,
	// where it reads as being between two characters.
	z_win_draw_text(&win, REF_W + 1, 2, shown, 1, cur_font);

	if (editing) {
		int cx = REF_W + (edit_caret - from) * CHAR_W;
		z_win_fill_rect(&win, cx, 1, 1, cur_font->h + 2, 1);
	}

	z_win_fill_rect(&win, 0, BAR_H - 1, cw, 1, 1);

}

static void draw_grid(void) {

	int nc = vis_cols();

	for (int i = 0; i < vis_rows; i++) {

		int r = top_row + i;
		if (r >= SHEET_ROWS) break;

		for (int j = 0; j < nc; j++)
			draw_cell(r, left_col + j);

	}

	// Slack to the right of the last full column, and below the last
	// row. Cleared explicitly rather than left alone: after a resize
	// or a scroll this is where the previous contents would still be.
	int x = grid_x;
	for (int j = 0; j < nc; j++) x += COL_PX(left_col + j);

	if (x < grid_x + grid_w)
		z_win_fill_rect(&win, x, grid_y, grid_x + grid_w - x, grid_h, 0);

	int y = grid_y + vis_rows * LINE_H;
	if (y < grid_y + grid_h)
		z_win_fill_rect(&win, grid_x, y, grid_w, grid_y + grid_h - y, 0);

}

// Everything in the window, WITHOUT clearing first.
//
// Every function called here fills its own background -- draw_bar()
// its strip, draw_headers() the header row and the row-header column,
// draw_grid() the cells and the slack past the last of them, and
// z_scrollbar_draw() its own track -- so nothing stale can survive as
// long as the GEOMETRY has not changed.
//
// That distinction is the whole reason this is a separate function
// from repaint() below. A clear followed by a redraw is a visible
// FLASH on this display, and the paths that run at pointer rates -- a
// drag that extends a selection, a scrollbar being dragged -- would
// flash on every sample. Committing a cell entry is the same, and it
// is the single most common thing anyone does here.
// -- the chart view (F5) --
//
// The chart takes the window, as 1-2-3's graph took the screen: the
// grid is not drawn behind it. A key changes its kind; Esc goes back.

static void chart_repaint(void) {

	int cw = z_win_content_w(&win), ch = z_win_content_h(&win);
	z_win_fill_rect(&win, 0, 0, cw, BAR_H, 0);
	static const char *const kinds[] = { "Bar", "Stacked", "Line", "Pie" };
	char t[80];
	int n = 0;
	const char *lead = "Chart: ";
	for (const char *q = lead; *q; q++) t[n++] = *q;
	for (int k = 0; k < SHEET_CHART_KINDS; k++) {
		const char *q = kinds[k];
		if (k == sh.chart.kind) t[n++] = '[';
		while (*q) t[n++] = *q++;
		if (k == sh.chart.kind) t[n++] = ']';
		t[n++] = ' ';
	}
	for (const char *q = "  Esc: back"; *q; q++) t[n++] = *q;
	t[n] = 0;
	z_win_draw_text(&win, 2, 2, t, 1, cur_font);
	z_win_fill_rect(&win, 0, BAR_H - 1, cw, 1, 1);
	chart_draw(&win, &sh, &sh.chart, 0, BAR_H, cw, ch - BAR_H - Z_WIN_GRIP_INSET, cur_font);

}

static void repaint_view(void) {

	if (chart_mode) { chart_repaint(); return; }

	update_scroll_range();

	draw_bar();
	draw_headers();
	draw_grid();

	z_scrollbar_draw(&vsb, false);
	z_scrollbar_draw(&hsb, false);

}

// The structural version: clears first, and forces the scrollbars.
//
// For the cases where the geometry itself changed (a resize, a font
// switch) or where something outside this app owned the pixels a
// moment ago (a Z_WM_REDRAW, a dialog that has just closed over us).
static void repaint(void) {

	if (chart_mode) { z_win_clear(&win); chart_repaint(); return; }

	z_win_clear(&win);

	update_scroll_range();

	draw_bar();
	draw_headers();
	draw_grid();

	z_scrollbar_draw(&vsb, true);
	z_scrollbar_draw(&hsb, true);

}

// A cursor move repaints only what changed: the two cells, their row
// and column headers, and the edit bar.
static void move_repaint(int old_row, int old_col) {

	if (chart_mode) return;

	if (scroll_to_cursor()) {
		repaint_view();
		return;
	}

	draw_cell(old_row, old_col);
	draw_cell(cur_row, cur_col);
	draw_headers();
	draw_bar();

	z_scrollbar_draw(&vsb, false);
	z_scrollbar_draw(&hsb, false);

}

// ---------------------------------------------------------------
// files
// ---------------------------------------------------------------

static void forward_msg(z_msg_t *msg, void *user);
static void history_clear(void);

static z_dialog_ctx_t dlg_ctx;

static char last_dir[Z_FLIST_PATH_MAX] = Z_DIR_HOME;

static void remember_dir(const char *path) {

	int cut = 0;

	for (int i = 0; path[i]; i++)
		if (path[i] == '/') cut = i;

	if (!cut) { last_dir[0] = '/'; last_dir[1] = 0; return; }

	int n = 0;
	while (n < cut && n < Z_FLIST_PATH_MAX - 1) { last_dir[n] = path[n]; n++; }
	last_dir[n] = 0;

}

static bool is_csv(const char *path) {

	const char *e = z_ftype_ext(path);
	if (!e) return false;

	return (e[0] == 'c' || e[0] == 'C') &&
		(e[1] == 's' || e[1] == 'S') &&
		(e[2] == 'v' || e[2] == 'V') && !e[3];

}

// The emit callback sheet_write() streams through. See sheet_core.h on
// why writing is a callback rather than a buffer: a whole sheet does
// not fit the heap, which shares 16KB with the stack.
static bool file_emit(void *ctx, const char *s, int len) {

	int h = *(int *)ctx;

	if (len < 0) len = (int)strlen(s);
	if (!len) return true;

	return fs_write_chunk(h, s, len) == len;

}

// Another system's table format -- .xlsx, .ods, .tsv. sheet reads and
// writes only its own and CSV; convert does the rest (docs/convert.md):
// saving to one of these writes the sheet to /tmp and converts it,
// with convert's own window for progress, Cancel and errors, and
// opening one hands it to convert, which opens the result here.
static bool is_foreign(const char *path) {

	const char *e = z_ftype_ext(path);
	if (!e) return false;

	char l[6];
	int n = 0;
	for (; e[n] && n < 5; n++) l[n] = (char)(e[n] | 0x20);
	l[n] = 0;

	return !strcmp(l, "xlsx") || !strcmp(l, "ods") || !strcmp(l, "tsv");

}

static bool do_export(const char *path) {

	static const char tmp[] = Z_DIR_TMP "/sheet-export.zss";
	static const char jobf[] = Z_DIR_TMP "/sheet-export.job";
	char job[Z_FS_PATH_MAX + 64];

	int h = fs_open_write(tmp);
	bool ok = h >= 0 && sheet_write(&sh, file_emit, &h);
	if (h >= 0) { fs_sync(h); fs_close_handle(h); }

	// no printf here: it is 50KB this app does not otherwise carry
	int n = 0;
	const char *parts[] = { "in=", tmp, "\nout=", path, "\nui=1\n" };
	for (unsigned i = 0; i < sizeof parts / sizeof parts[0]; i++)
		for (const char *p = parts[i]; *p && n < (int)sizeof(job) - 1; p++) job[n++] = *p;
	job[n] = 0;
	if (ok && n < (int)sizeof(job) - 1 && fs_write_file((char *)jobf, job, n) == n) {
		z_launch_arg_set("-j " Z_DIR_TMP "/sheet-export.job");
		if (z_proc_run("convert")) return true;
	}

	z_dialog_confirm(&dlg_ctx, "Export", ok ? "convert could not\nbe started." :
		"Could not write the\ncopy to convert.", Z_DIALOG_OK_CANCEL);
	return false;

}

static bool do_save_to(const char *path) {

	// an export is a copy: the sheet keeps its own name and state
	if (is_foreign(path)) return do_export(path);

	int h = fs_open_write(path);

	if (h < 0) {
		z_dialog_confirm(&dlg_ctx, "Save failed",
			"Could not open the file\nfor writing.", Z_DIALOG_YES_NO);
		return false;
	}

	bool ok = is_csv(path)
		? sheet_write_csv(&sh, file_emit, &h)
		: sheet_write(&sh, file_emit, &h);

	// Sync before close, so a card pulled a moment later still has a
	// directory entry with the right size in it (see fs_sync()).
	fs_sync(h);
	fs_close_handle(h);

	if (!ok) {
		z_dialog_confirm(&dlg_ctx, "Save failed",
			"The file could not be\nwritten completely.", Z_DIALOG_YES_NO);
		return false;
	}

	if (path != filename) {
		int n = 0;
		while (path[n] && n < (int)sizeof(filename) - 1) {
			filename[n] = path[n];
			n++;
		}
		filename[n] = 0;
	}

	sh.modified = false;
	remember_dir(path);
	update_title();

	return true;

}

static bool do_save_as(void) {

	char path[Z_FS_PATH_MAX];

	const char *suggest = filename[0] ? filename : "sheet.zss";

	if (!z_dialog_save(&dlg_ctx, last_dir, suggest, path, sizeof(path)))
		return false;

	bool ok = do_save_to(path);

	repaint();

	return ok;

}

static bool do_save(void) {

	if (!filename[0]) return do_save_as();

	// A sheet in /tmp is gone at the next boot -- most often one
	// convert made from an .xlsx or .ods. Its first save asks where.
	if (!strncmp(filename, Z_DIR_TMP "/", sizeof(Z_DIR_TMP))) {
		strcpy(last_dir, Z_DIR_HOME);
		return do_save_as();
	}

	bool ok = do_save_to(filename);

	repaint();

	return ok;

}

// One line buffer for both readers, in .bss rather than on the stack:
// a CSV row of 26 populated fields is far wider than anything the
// native format produces, and 1KB is more stack than an app with a
// 16KB allocation should spend on a transient.
static char line_buf[1024];

static bool load_path(const char *path) {

	if (is_foreign(path)) {
		// convert opens the result in a sheet of its own
		z_launch_arg_set(path);
		if (!z_proc_run("convert"))
			z_dialog_confirm(&dlg_ctx, "Open failed",
				"convert is not on\nthis card.", Z_DIALOG_OK_CANCEL);
		return false;
	}

	int h = fs_open_read(path);

	if (h < 0) {
		z_dialog_confirm(&dlg_ctx, "Open failed",
			"The file could not\nbe opened.", Z_DIALOG_YES_NO);
		return false;
	}

	bool csv = is_csv(path);

	if (csv) sheet_load_csv_begin(&sh);
	else sheet_load_begin(&sh);

	char chunk[256];
	int n = 0;
	int got;

	// Read in bounded chunks and split on newlines here. Nothing ever
	// holds more than one line -- see sheet_core.h.
	while ((got = fs_read_chunk(h, chunk, sizeof(chunk))) > 0) {

		for (int i = 0; i < got; i++) {

			char ch = chunk[i];

			if (ch == '\n') {
				line_buf[n] = 0;
				if (csv) sheet_load_csv_line(&sh, line_buf);
				else sheet_load_line(&sh, line_buf);
				n = 0;
				continue;
			}

			// An over-long line is truncated rather than wrapped into
			// a spurious second line, which would put half a cell's
			// text in the row below.
			if (n < (int)sizeof(line_buf) - 1) line_buf[n++] = ch;

		}

	}

	if (n) {
		line_buf[n] = 0;
		if (csv) sheet_load_csv_line(&sh, line_buf);
		else sheet_load_line(&sh, line_buf);
	}

	fs_close_handle(h);

	if (csv) sheet_load_csv_end(&sh);
	else sheet_load_end(&sh);
	history_clear();		// another document: no history to undo into

	int fn = 0;
	while (path[fn] && fn < (int)sizeof(filename) - 1) {
		filename[fn] = path[fn];
		fn++;
	}
	filename[fn] = 0;

	remember_dir(path);

	cur_row = cur_col = sel_row = sel_col = 0;
	top_row = left_col = 0;
	editing = false;

	return true;

}

static bool confirm_discard(void) {

	if (!sh.modified) return true;

	int r = z_dialog_confirm(&dlg_ctx, "Unsaved changes",
		"This sheet has unsaved\nchanges. Save first?",
		Z_DIALOG_YES_NO_CANCEL);

	if (r == Z_DIALOG_CANCEL) return false;
	if (r == Z_DIALOG_YES) return do_save();

	return true;

}

static void do_new(void) {

	if (!confirm_discard()) { repaint(); return; }

	sheet_init(&sh);
	history_clear();

	filename[0] = 0;
	cur_row = cur_col = sel_row = sel_col = 0;
	top_row = left_col = 0;
	editing = false;

	update_title();
	repaint();

}

static void do_open(void) {

	if (!confirm_discard()) { repaint(); return; }

	char path[Z_FS_PATH_MAX];

	if (z_dialog_open(&dlg_ctx, last_dir, path, sizeof(path)))
		load_path(path);

	update_title();
	layout();
	repaint();

}

static void do_font(void) {

	cur_font = (cur_font == &z_font_5x8) ? &z_font_6x12 : &z_font_5x8;

	layout();
	scroll_to_cursor();
	repaint();

}

static void do_close(void) {

	if (!confirm_discard()) { repaint(); return; }

	z_win_destroy(&win);
	exit(0);

}

// ---------------------------------------------------------------
// memory: undo, the clipboard, scratch
// ---------------------------------------------------------------
//
// One block, from the arena when the machine has one to give
// (docs/kernel.md, "Arenas"), else a small one of our own: undo and
// redo history, the cells last copied, and scratch for a sort. Taken
// once at start; nothing here is held as a pointer across a resize,
// because there is none.

#define MEM_WANT	(512u * 1024u)
#define MEM_LESS	(128u * 1024u)

static uint8_t mem_small[32768] __attribute__((aligned(8)));
static uint8_t *mem;
static uint32_t mem_size;

static uint8_t *clip_mem, *scratch;
static int clip_cap, scratch_cap;

// -- undo --
//
// Two stacks of records, undo and redo. A record is what a change
// overwrote: the cells of a rectangle (most edits), the widths and
// formats (a format or a width), or the whole document (inserting and
// deleting rows and columns, which move everything). Undoing one
// pushes what is there NOW onto the other stack and puts the record
// back, so undo and redo are one operation in two directions.
//
// When a stack is full the oldest records go: history gets shorter,
// it never refuses an edit.

enum { U_CELLS = 1, U_META, U_SNAP };

typedef struct {
	uint8_t		*b;
	int			cap, used;
} ustack_t;

// [len u32][type u8][0][r0 u16][c0 u16][h u16][w u16] payload [len u32]
#define UHDR	14
#define UTRL	4

typedef struct {
	int		type, r0, c0, h, w, len;
	uint8_t	*payload;
} urec_t;

static ustack_t ust, rst;

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr16(uint8_t *p, int v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static int rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }

static void mem_init(void) {

	static const uint32_t want[] = { MEM_WANT, MEM_LESS };
	for (unsigned i = 0; i < sizeof want / sizeof want[0] && !mem; i++) {
		void *base = NULL;
		uint32_t got = 0;
		if (z_arena_set(want[i], &base, &got) == Z_ARENA_OK && base && got) {
			mem = base;
			mem_size = got;
		}
	}
	if (!mem) { mem = mem_small; mem_size = sizeof(mem_small); }

	// undo half, redo a quarter, clipboard and scratch an eighth each
	uint32_t q = (mem_size / 8) & ~7u;
	ust.b = mem;            ust.cap = (int)(q * 4);
	rst.b = mem + q * 4;    rst.cap = (int)(q * 2);
	clip_mem = mem + q * 6; clip_cap = (int)q;
	scratch = mem + q * 7;  scratch_cap = (int)(mem_size - q * 7);
	ust.used = rst.used = 0;

}

// Room for `need` bytes on top of `s`, dropping the oldest records.
static bool u_room(ustack_t *s, int need) {

	if (need > s->cap) { s->used = 0; return false; }

	while (s->used + need > s->cap) {
		int first = UHDR + (int)rd32(s->b) + UTRL;
		memmove(s->b, s->b + first, (size_t)(s->used - first));
		s->used -= first;
	}
	return true;

}

static void u_head(uint8_t *p, int type, int r0, int c0, int h, int w, int len) {
	wr32(p, (uint32_t)len);
	p[4] = (uint8_t)type;
	p[5] = 0;
	wr16(p + 6, r0);
	wr16(p + 8, c0);
	wr16(p + 10, h);
	wr16(p + 12, w);
}

// Saves what the rectangle holds now, as a record on `s`.
static bool u_push_cells(ustack_t *s, int r0, int c0, int r1, int c1) {

	int len = sheet_range_size(&sh, r0, c0, r1, c1);
	if (!u_room(s, UHDR + len + UTRL)) return false;
	uint8_t *p = s->b + s->used;
	u_head(p, U_CELLS, r0, c0, r1 - r0 + 1, c1 - c0 + 1, len);
	sheet_range_save(&sh, r0, c0, r1, c1, p + UHDR, len);
	wr32(p + UHDR + len, (uint32_t)len);
	s->used += UHDR + len + UTRL;
	return true;

}

static int meta_size(void) {
	return SHEET_COLS + 1 + sh.nfmts * (int)sizeof(sheet_fmt_t);
}

static bool u_push_meta(ustack_t *s) {

	int len = meta_size();
	if (!u_room(s, UHDR + len + UTRL)) return false;
	uint8_t *p = s->b + s->used;
	u_head(p, U_META, cur_row, cur_col, 1, 1, len);
	memcpy(p + UHDR, sh.colw, SHEET_COLS);
	p[UHDR + SHEET_COLS] = (uint8_t)sh.nfmts;
	memcpy(p + UHDR + SHEET_COLS + 1, sh.fmts, (size_t)sh.nfmts * sizeof(sheet_fmt_t));
	wr32(p + UHDR + len, (uint32_t)len);
	s->used += UHDR + len + UTRL;
	return true;

}

static bool u_push_snap(ustack_t *s) {

	int len = sheet_snapshot_size(&sh);
	if (!u_room(s, UHDR + len + UTRL)) return false;
	uint8_t *p = s->b + s->used;
	u_head(p, U_SNAP, cur_row, cur_col, 1, 1, len);
	sheet_snapshot(&sh, p + UHDR, len);
	wr32(p + UHDR + len, (uint32_t)len);
	s->used += UHDR + len + UTRL;
	return true;

}

static bool u_top(ustack_t *s, urec_t *r) {

	if (!s->used) return false;
	int len = (int)rd32(s->b + s->used - UTRL);
	uint8_t *p = s->b + s->used - UTRL - len - UHDR;
	r->len = len;
	r->type = p[4];
	r->r0 = rd16(p + 6);
	r->c0 = rd16(p + 8);
	r->h = rd16(p + 10);
	r->w = rd16(p + 12);
	r->payload = p + UHDR;
	return true;

}

// Before a change to the cells of a rectangle: what is there goes on
// the undo stack, and the redo stack is no longer a future.
static void will_change(int r0, int c0, int r1, int c1) {
	if (r0 > r1) { int t = r0; r0 = r1; r1 = t; }
	if (c0 > c1) { int t = c0; c0 = c1; c1 = t; }
	u_push_cells(&ust, r0, c0, r1, c1);
	rst.used = 0;
}

static void will_change_meta(void) { u_push_meta(&ust); rst.used = 0; }
static void will_change_all(void) { u_push_snap(&ust); rst.used = 0; }

static void update_title(void);

// Undo (from = the undo stack) or redo.
static void u_apply(ustack_t *from, ustack_t *to) {

	urec_t r;
	if (!u_top(from, &r)) return;

	// The inverse goes on the other stack, which is other memory, so
	// the record stays where it is until it has been put back.
	int size = UHDR + r.len + UTRL;
	uint8_t *copy = r.payload;

	switch (r.type) {
		case U_CELLS:
			u_push_cells(to, r.r0, r.c0, r.r0 + r.h - 1, r.c0 + r.w - 1);
			sheet_range_load(&sh, r.r0, r.c0, r.h, r.w, true, copy, r.len, false, 0, 0);
			cur_row = r.r0; cur_col = r.c0;
			sel_row = r.r0 + r.h - 1; sel_col = r.c0 + r.w - 1;
			break;
		case U_META:
			u_push_meta(to);
			memcpy(sh.colw, copy, SHEET_COLS);
			sh.nfmts = copy[SHEET_COLS];
			memcpy(sh.fmts, copy + SHEET_COLS + 1, (size_t)sh.nfmts * sizeof(sheet_fmt_t));
			break;
		case U_SNAP:
			u_push_snap(to);
			sheet_restore(&sh, copy, r.len);
			break;
	}

	from->used -= size;

	sh.modified = true;
	editing = false;
	if (cur_row >= SHEET_ROWS) cur_row = SHEET_ROWS - 1;
	if (cur_col >= SHEET_COLS) cur_col = SHEET_COLS - 1;
	if (sel_row >= SHEET_ROWS) sel_row = SHEET_ROWS - 1;
	if (sel_col >= SHEET_COLS) sel_col = SHEET_COLS - 1;
	update_title();
	scroll_to_cursor();
	repaint_view();

}

static void history_clear(void) { ust.used = rst.used = 0; }

static void do_undo(void) { u_apply(&ust, &rst); }
static void do_redo(void) { u_apply(&rst, &ust); }

// ---------------------------------------------------------------
// editing
// ---------------------------------------------------------------

static void begin_edit(bool keep) {

	editing = true;
	edit_len = 0;
	edit[0] = 0;

	if (keep) {
		const char *src = sheet_src(&sh, cur_row, cur_col);
		while (src[edit_len] && edit_len < SHEET_SRC_MAX - 1) {
			edit[edit_len] = src[edit_len];
			edit_len++;
		}
		edit[edit_len] = 0;
	}

	edit_caret = edit_len;

	// A selection makes no sense while typing into one cell.
	sel_row = cur_row;
	sel_col = cur_col;

}

static void cancel_edit(void) {

	editing = false;
	draw_cell(cur_row, cur_col);
	draw_bar();

}

// Commits the edit buffer into the cursor cell. Repaints the whole
// grid, because a formula anywhere can depend on the cell just
// changed -- there is no cheaper answer that is also correct.
static void commit_edit(void) {

	if (!editing) return;

	editing = false;

	if (!strcmp(edit, sheet_src(&sh, cur_row, cur_col))) { draw_cell(cur_row, cur_col); draw_bar(); return; }

	will_change(cur_row, cur_col, cur_row, cur_col);
	sheet_err_t e = sheet_set(&sh, cur_row, cur_col, edit);

	if (e == SHEET_ERR_FULL)
		z_dialog_confirm(&dlg_ctx, "Sheet full",
			"No room for another cell.\nThe change was not made.",
			Z_DIALOG_YES_NO);

	update_title();
	repaint_view();

}

static void edit_insert(char c) {

	if (edit_len >= SHEET_SRC_MAX - 1) return;

	for (int i = edit_len; i > edit_caret; i--) edit[i] = edit[i - 1];

	edit[edit_caret++] = c;
	edit_len++;
	edit[edit_len] = 0;

	draw_bar();

}

static void edit_backspace(void) {

	if (!edit_caret) return;

	for (int i = edit_caret - 1; i < edit_len - 1; i++) edit[i] = edit[i + 1];

	edit_caret--;
	edit_len--;
	edit[edit_len] = 0;

	draw_bar();

}

static void edit_delete(void) {

	if (edit_caret >= edit_len) return;

	for (int i = edit_caret; i < edit_len - 1; i++) edit[i] = edit[i + 1];

	edit_len--;
	edit[edit_len] = 0;

	draw_bar();

}

// ---------------------------------------------------------------
// clipboard
// ---------------------------------------------------------------
//
// A selection travels as TAB-SEPARATED text, one line per row -- the
// same thing every other spreadsheet puts on a clipboard, so a copy
// out of here pastes into `text` as something readable, and a table
// pasted in from anywhere arrives in the right cells.

static char clip_buf[Z_WM_CLIP_MAX];

// What was copied, as cells: so a paste here moves formulas the way a
// spreadsheet does (=A1 copied one row down is =A2), which the text on
// the system clipboard cannot say. Used when the clipboard still holds
// what this copy put there -- recognised by a checksum of it.
static int clip_len = -1, clip_h, clip_w, clip_r0, clip_c0;
static uint32_t clip_sum;

static uint32_t text_sum(const char *t, int n) {
	uint32_t h = 2166136261u;
	for (int i = 0; i < n; i++) h = (h ^ (uint8_t)t[i]) * 16777619u;
	return h;
}

static void do_copy(void) {

	int r0, c0, r1, c1;
	sel_rect(&r0, &c0, &r1, &c1);

	clip_len = sheet_range_save(&sh, r0, c0, r1, c1, clip_mem, clip_cap);
	clip_h = r1 - r0 + 1;
	clip_w = c1 - c0 + 1;
	clip_r0 = r0;
	clip_c0 = c0;

	int n = 0;

	for (int r = r0; r <= r1 && n < (int)sizeof(clip_buf) - 2; r++) {

		for (int c = c0; c <= c1; c++) {

			if (c > c0 && n < (int)sizeof(clip_buf) - 2) clip_buf[n++] = '\t';

			const char *src = sheet_src(&sh, r, c);

			// The apostrophe that forces text is a local convention,
			// not something to hand to another app.
			if (*src == '\'') src++;

			while (*src && n < (int)sizeof(clip_buf) - 2) clip_buf[n++] = *src++;

		}

		if (n < (int)sizeof(clip_buf) - 2) clip_buf[n++] = '\n';

	}

	clip_buf[n] = 0;

	clip_sum = text_sum(clip_buf, n);
	z_clip_set(clip_buf, n);

}

// Our own copy: cells, with formulas moved. A selection that is a
// multiple of what was copied is filled with copies of it.
static void paste_cells(void) {

	int r0, c0, r1, c1;
	sel_rect(&r0, &c0, &r1, &c1);
	int th = r1 - r0 + 1, tw = c1 - c0 + 1;
	int nr = (th % clip_h == 0) ? th / clip_h : 1, nc = (tw % clip_w == 0) ? tw / clip_w : 1;
	int er = r0 + nr * clip_h - 1, ec = c0 + nc * clip_w - 1;
	if (er >= SHEET_ROWS) er = SHEET_ROWS - 1;
	if (ec >= SHEET_COLS) ec = SHEET_COLS - 1;
	will_change(r0, c0, er, ec);
	sheet_err_t e = SHEET_OK;
	for (int i = 0; i < nr; i++)
		for (int j = 0; j < nc; j++) {
			sheet_err_t e2 = sheet_range_load(&sh, r0 + i * clip_h, c0 + j * clip_w, clip_h, clip_w,
				true, clip_mem, clip_len, true, clip_r0, clip_c0);
			if (e2 != SHEET_OK) e = e2;
		}
	if (e == SHEET_ERR_FULL)
		z_dialog_confirm(&dlg_ctx, "Sheet full", "No room for more cells.\nSome were not pasted.", Z_DIALOG_OK_CANCEL);
	update_title();
	repaint();

}

static void do_paste(void) {

	int got = z_clip_get(clip_buf, (int)sizeof(clip_buf));
	if (!got) return;

	if (clip_len >= 0 && text_sum(clip_buf, got) == clip_sum) {
		paste_cells();
		return;
	}
	// Text from elsewhere: tab-separated, a row a line. Its size first,
	// for the undo record.
	{
		int rows = 1, cols = 1, cc = 1;
		for (int i = 0; i < got; i++) {
			if (clip_buf[i] == '\t') { cc++; if (cc > cols) cols = cc; }
			else if (clip_buf[i] == '\n') { if (i + 1 < got) rows++; cc = 1; }
		}
		int er = cur_row + rows - 1, ec = cur_col + cols - 1;
		if (er >= SHEET_ROWS) er = SHEET_ROWS - 1;
		if (ec >= SHEET_COLS) ec = SHEET_COLS - 1;
		will_change(cur_row, cur_col, er, ec);
	}

	int r = cur_row, c = cur_col;
	char cell[SHEET_SRC_MAX];
	int n = 0;

	for (int i = 0; i <= got; i++) {

		char ch = clip_buf[i];

		if (ch == '\t' || ch == '\n' || ch == 0) {

			cell[n] = 0;

			if (n) sheet_set(&sh, r, c, cell);

			n = 0;

			if (ch == '\t') {
				c++;
				if (c >= SHEET_COLS) { c = cur_col; r++; }
			} else {
				c = cur_col;
				r++;
			}

			if (ch == 0 || r >= SHEET_ROWS) break;

			continue;

		}

		if (ch == '\r') continue;

		if (n < SHEET_SRC_MAX - 1) cell[n++] = ch;

	}

	update_title();
	repaint_view();

}

static void do_clear_selection(void) {

	int r0, c0, r1, c1;
	sel_rect(&r0, &c0, &r1, &c1);

	will_change(r0, c0, r1, c1);
	sheet_clear_range(&sh, r0, c0, r1, c1);

	update_title();
	repaint_view();

}

// ---------------------------------------------------------------
// commands
// ---------------------------------------------------------------

static void after_change(void) {
	update_title();
	repaint_view();
}

static void say(const char *title, const char *msg) {
	z_dialog_confirm(&dlg_ctx, title, msg, Z_DIALOG_OK_CANCEL);
	repaint();
}

// Fill down (Ctrl+D) and right (Ctrl+R): the selection's first row
// (column) copied into the rest of it, formulas moving as they go --
// the way a column of totals or a row of months is made.
static void do_fill(bool down) {

	int r0, c0, r1, c1;
	sel_rect(&r0, &c0, &r1, &c1);
	if (down ? r1 == r0 : c1 == c0) return;

	int len = down ? sheet_range_save(&sh, r0, c0, r0, c1, scratch, scratch_cap)
		: sheet_range_save(&sh, r0, c0, r1, c0, scratch, scratch_cap);
	if (len < 0) { say("Fill", "Too much to fill\nat once."); return; }

	if (down) will_change(r0 + 1, c0, r1, c1);
	else will_change(r0, c0 + 1, r1, c1);

	sheet_err_t e = SHEET_OK;
	if (down) {
		for (int r = r0 + 1; r <= r1 && e == SHEET_OK; r++)
			e = sheet_range_load(&sh, r, c0, 1, c1 - c0 + 1, true, scratch, len, true, r0, c0);
	} else {
		for (int c = c0 + 1; c <= c1 && e == SHEET_OK; c++)
			e = sheet_range_load(&sh, r0, c, r1 - r0 + 1, 1, true, scratch, len, true, r0, c0);
	}
	if (e == SHEET_ERR_FULL) say("Sheet full", "No room for more cells.\nSome were not filled.");
	after_change();

}

// Rows or columns, inserted before the cursor or deleted from it --
// as many as the selection spans.
static void do_structural(bool rows, bool ins) {

	int r0, c0, r1, c1;
	sel_rect(&r0, &c0, &r1, &c1);
	int at = rows ? r0 : c0, n = rows ? r1 - r0 + 1 : c1 - c0 + 1;

	will_change_all();
	sheet_err_t e = rows ? (ins ? sheet_insert_rows(&sh, at, n) : sheet_delete_rows(&sh, at, n))
		: (ins ? sheet_insert_cols(&sh, at, n) : sheet_delete_cols(&sh, at, n));

	if (e != SHEET_OK) {
		// nothing changed (inserting refuses before it starts); the
		// record just pushed would undo nothing
		if (ust.used) { urec_t r; if (u_top(&ust, &r)) ust.used -= UHDR + r.len + UTRL; }
		say(ins ? "Insert" : "Delete", e == SHEET_ERR_FULL && ins ?
			"That would push cells\npast the edge of the sheet." : "No room: the sheet\nis full.");
		return;
	}
	after_change();

}

// Ctrl+1, or /Format: a format for the selection.
static void do_format(void) {

	int r0, c0, r1, c1;
	sel_rect(&r0, &c0, &r1, &c1);
	int dec, kind = sheet_format_at(&sh, cur_row, cur_col, &dec);
	char cur[8], in[16];
	sheet_format_name(kind, dec, cur, sizeof cur);

	if (!z_dialog_prompt(&dlg_ctx, "Format",
		"G general  F2 fixed  ,2 1,000.00\n%1 percent  $2 money  (digit: places)",
		cur, in, sizeof in)) { repaint(); return; }

	int k, d;
	if (!sheet_parse_format(in, &k, &d)) {
		say("Format", "Not a format: G, F2, ,2,\n%1 or $2 (any 0-8 places).");
		return;
	}
	will_change_meta();
	if (!sheet_set_format(&sh, r0, c0, r1, c1, k, d))
		say("Format", "Too many formatted\nranges: 64 at most.");
	repaint();
	update_title();

}

// /Width: the current column's width, typed.
static void do_width(void) {

	char cur[8], in[8];
	int w = sheet_colw(&sh, cur_col), n = 0;
	char t[4];
	int tn = 0;
	while (w && tn < 3) { t[tn++] = (char)('0' + w % 10); w /= 10; }
	while (tn) cur[n++] = t[--tn];
	cur[n] = 0;

	if (!z_dialog_prompt(&dlg_ctx, "Width", "Column width, in characters:", cur, in, sizeof in)) {
		repaint();
		return;
	}
	int v = 0;
	for (const char *p = in; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
	if (v) {
		will_change_meta();
		int r0, c0, r1, c1;
		sel_rect(&r0, &c0, &r1, &c1);
		for (int c = c0; c <= c1; c++) sheet_set_colw(&sh, c, v);
	}
	update_title();
	scroll_to_cursor();
	repaint();

}

// F5, or /Chart: the chart of the selection -- or, with none, the
// sheet's chart as it was last made.
static void do_chart(void) {

	if (sel_row != cur_row || sel_col != cur_col) {
		int r0, c0, r1, c1;
		sel_rect(&r0, &c0, &r1, &c1);
		sheet_chart_t *g = &sh.chart;
		if (!g->set || g->r0 != r0 || g->r1 != r1 || g->c0 != c0 || g->c1 != c1) {
			if (!g->set) g->kind = SHEET_CHART_BAR;
			g->set = true;
			g->r0 = (uint16_t)r0; g->r1 = (uint16_t)r1;
			g->c0 = (uint8_t)c0; g->c1 = (uint8_t)c1;
			sh.modified = true;
			update_title();
		}
	} else if (!sh.chart.set) {
		say("Chart", "Select the table to chart\n(labels in the first row\nand column), then F5.");
		return;
	}
	chart_mode = true;
	repaint();

}

static void chart_key(uint32_t k) {

	int kind = -1;
	switch (k) {
		case 'b': case 'B': kind = SHEET_CHART_BAR; break;
		case 's': case 'S': kind = SHEET_CHART_STACK; break;
		case 'l': case 'L': kind = SHEET_CHART_LINE; break;
		case 'p': case 'P': kind = SHEET_CHART_PIE; break;
		case 0x1b: case Z_KEY_F5: case '\r': case '\n':
			chart_mode = false;
			repaint();
			return;
		default: return;
	}
	if (kind != sh.chart.kind) {
		sh.chart.kind = (uint8_t)kind;
		sh.modified = true;
		update_title();
	}
	repaint();

}

// /Sort: the selection's rows, by the cursor's column.
static void do_sort(bool desc) {

	int r0, c0, r1, c1;
	sel_rect(&r0, &c0, &r1, &c1);
	if (r1 == r0) {
		say("Sort", "Select the rows to sort\n(all their columns), then\nput the cursor in the key.");
		return;
	}
	will_change(r0, c0, r1, c1);
	if (!sheet_sort(&sh, r0, c0, r1, c1, cur_col, desc, scratch, scratch_cap)) {
		urec_t r;
		if (u_top(&ust, &r)) ust.used -= UHDR + r.len + UTRL;
		say("Sort", "Too large to sort here:\nsort fewer rows at a time.");
		return;
	}
	after_change();

}

// Ctrl+G: go to a cell.
static void do_goto(void) {

	char in[16];
	if (!z_dialog_prompt(&dlg_ctx, "Go to", "Cell (like B12):", "", in, sizeof in) || !in[0]) {
		repaint();
		return;
	}
	int r, c;
	const char *p = in;
	while (*p == ' ') p++;
	if (!sheet_parse_ref(p, &r, &c, NULL)) { say("Go to", "Not a cell: A1 to Z999."); return; }
	cur_row = sel_row = r;
	cur_col = sel_col = c;
	scroll_to_cursor();
	repaint();

}

// Ctrl+F: the next cell, after the cursor, whose text or value
// contains what is asked, in reading order; F3 again for the next.
static char find_text[SHEET_SRC_MAX];

static bool contains_ci(const char *h, const char *n) {
	int nl = (int)strlen(n);
	for (; *h; h++) {
		int i = 0;
		for (; i < nl; i++) {
			char a = h[i], b = n[i];
			if (!a) return false;
			if (a >= 'a' && a <= 'z') a = (char)(a - 32);
			if (b >= 'a' && b <= 'z') b = (char)(b - 32);
			if (a != b) break;
		}
		if (i == nl) return true;
	}
	return false;
}

static void do_find(void);

// F3 with nothing found yet asks what to find (docs/keys.md).
static void find_next(void) {

	if (!find_text[0]) { do_find(); return; }
	int start = cur_row * SHEET_COLS + cur_col;
	// the cells are kept in key order: one walk round from the cursor
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < sh.ncells; i++) {
			int key = sh.cells[i].key;
			if (pass == 0 ? key <= start : key > start) continue;
			int r = key / SHEET_COLS, c = key % SHEET_COLS;
			char shown[48];
			sheet_display(&sh, r, c, shown, sizeof shown, 40, NULL);
			if (contains_ci(sheet_src(&sh, r, c), find_text) || contains_ci(shown, find_text)) {
				cur_row = sel_row = r;
				cur_col = sel_col = c;
				scroll_to_cursor();
				repaint_view();
				return;
			}
		}
	}
	say("Find", "Not found.");

}

// F1 and /Help: the short version; "Read docs" is the manual.
static void show_help(void) {
	static const char help[] =
		"A spreadsheet: numbers, text and formulas in a grid.\n\n"
		"Type into the cell; =A1*2 is a formula, =SUM(B2:B9) a "
		"range. Enter moves down, Tab right, Shift+arrows select.\n\n"
		"/, F4\tCommands: insert, delete, number format, width, sort, "
		"extend, goto, find, chart\n"
		"F2\tEdit the cell\n"
		"Ctrl+Z/Y\tUndo, redo\n"
		"Ctrl+C/X/V\tCopy, cut, paste\n"
		"Ctrl+D/R\tFill down, right\n"
		"Ctrl+1\tNumber format\n"
		"Ctrl+G\tGo to a cell\n"
		"F3, Ctrl+F\tFind (again: the next match)\n"
		"F5\tChart the selection\n"
		"Alt+Left\tNarrow the column (Alt+Right widens)\n"
		"Ctrl+N/O/S\tNew, open, save\n\n"
		"Functions: SUM AVERAGE MIN MAX COUNT ABS INT ROUND MOD "
		"SQRT POWER IF AND OR NOT PMT PV FV NPV VLOOKUP INDEX.\n\n"
		"Select a range to see its sum, average and count.";
	z_dialog_help(&dlg_ctx, "Sheet help", help, "sheet_app");
}

static void do_find(void) {

	char in[SHEET_SRC_MAX];
	if (!z_dialog_prompt(&dlg_ctx, "Find", "Find (F3 finds the next):", find_text, in, sizeof in) || !in[0]) {
		repaint();
		return;
	}
	memcpy(find_text, in, sizeof find_text);
	repaint();
	find_next();

}

// ---------------------------------------------------------------
// the / menu
// ---------------------------------------------------------------
//
// "/" opens a line of commands in the edit bar, as it did in VisiCalc
// and 1-2-3: each starts with a different letter, and that letter (or
// the arrows and Enter) chooses it. Everything here is also on a key
// of its own; the menu is how those are found without a manual.

typedef struct {
	const char	*label;		// first letter chooses
	int			id;
} mitem_t;

enum {
	M_NONE = 0,
	M_TOP, M_INSERT, M_DELETE, M_SORT, M_FILL,	// menus
	C_INS_ROWS = 100, C_INS_COLS, C_DEL_ROWS, C_DEL_COLS, C_FORMAT, C_WIDTH,
	C_SORT_UP, C_SORT_DOWN, C_FILL_DOWN, C_FILL_RIGHT, C_UNDO, C_REDO,
	C_GOTO, C_FIND, C_HELP, C_CHART,
};

static const mitem_t m_top[] = {
	{ "Insert", M_INSERT }, { "Delete", M_DELETE }, { "Number", C_FORMAT },
	{ "Width", C_WIDTH }, { "Sort", M_SORT }, { "Extend", M_FILL },
	{ "Undo", C_UNDO }, { "Redo", C_REDO }, { "Goto", C_GOTO },
	{ "Find", C_FIND }, { "Chart", C_CHART }, { "Help", C_HELP }, { NULL, 0 },
};
static const mitem_t m_insert[] = { { "Rows", C_INS_ROWS }, { "Columns", C_INS_COLS }, { NULL, 0 } };
static const mitem_t m_delete[] = { { "Rows", C_DEL_ROWS }, { "Columns", C_DEL_COLS }, { NULL, 0 } };
static const mitem_t m_sort[] = { { "Ascending", C_SORT_UP }, { "Descending", C_SORT_DOWN }, { NULL, 0 } };
static const mitem_t m_fill[] = { { "Down", C_FILL_DOWN }, { "Right", C_FILL_RIGHT }, { NULL, 0 } };

static int menu_hot;			// menu: M_NONE when closed (declared above draw_bar)

static const mitem_t *menu_items(void) {
	return menu == M_INSERT ? m_insert : menu == M_DELETE ? m_delete :
		menu == M_SORT ? m_sort : menu == M_FILL ? m_fill : m_top;
}

static void draw_menu(void) {

	int cw = z_win_content_w(&win);
	z_win_fill_rect(&win, 0, 0, cw, BAR_H, 0);
	const char *title = menu == M_INSERT ? "Insert:" : menu == M_DELETE ? "Delete:" :
		menu == M_SORT ? "Sort:" : menu == M_FILL ? "Extend:" : "/";
	z_win_draw_text(&win, 2, 2, title, 1, cur_font);
	int x = 2 + ((int)strlen(title) + 1) * CHAR_W;
	const mitem_t *it = menu_items();
	for (int i = 0; it[i].label; i++) {
		int w = (int)strlen(it[i].label) * CHAR_W;
		if (x + w > cw - 2) break;
		bool hot = (i == menu_hot);
		if (hot) z_win_fill_rect(&win, x - 1, 1, w + 2, BAR_H - 2, 1);
		z_win_draw_text2(&win, x, 2, it[i].label, hot ? 0 : 1, hot ? 1 : 0, cur_font);
		x += w + CHAR_W;
	}
	z_win_fill_rect(&win, 0, BAR_H - 1, cw, 1, 1);

}

static void run_command(int id);

static void menu_choose(int id) {
	if (id < 100) { menu = id; menu_hot = 0; draw_menu(); return; }
	menu = M_NONE;
	draw_bar();
	run_command(id);
}

static void menu_key(uint32_t k) {

	const mitem_t *it = menu_items();
	int n = 0;
	while (it[n].label) n++;

	if (k == 0x1b) {
		if (menu == M_TOP) { menu = M_NONE; draw_bar(); }
		else { menu = M_TOP; menu_hot = 0; draw_menu(); }
		return;
	}
	if (k == Z_KEY_LEFT) { menu_hot = (menu_hot + n - 1) % n; draw_menu(); return; }
	if (k == Z_KEY_RIGHT || k == ' ') { menu_hot = (menu_hot + 1) % n; draw_menu(); return; }
	if (k == '\r' || k == '\n') { menu_choose(it[menu_hot].id); return; }

	char c = (char)k;
	if (c >= 'a' && c <= 'z') c = (char)(c - 32);
	for (int i = 0; i < n; i++)
		if (it[i].label[0] == c) { menu_choose(it[i].id); return; }

}

static void run_command(int id) {

	switch (id) {
		case C_INS_ROWS:   do_structural(true, true); break;
		case C_INS_COLS:   do_structural(false, true); break;
		case C_DEL_ROWS:   do_structural(true, false); break;
		case C_DEL_COLS:   do_structural(false, false); break;
		case C_FORMAT:     do_format(); break;
		case C_WIDTH:      do_width(); break;
		case C_SORT_UP:    do_sort(false); break;
		case C_SORT_DOWN:  do_sort(true); break;
		case C_FILL_DOWN:  do_fill(true); break;
		case C_FILL_RIGHT: do_fill(false); break;
		case C_UNDO:       do_undo(); break;
		case C_REDO:       do_redo(); break;
		case C_GOTO:       do_goto(); break;
		case C_FIND:       do_find(); break;
		case C_HELP:       show_help(); break;
		case C_CHART:      do_chart(); break;
		default: break;
	}

}

// ---------------------------------------------------------------
// keyboard
// ---------------------------------------------------------------

static void move_to(int r, int c, bool extend) {

	if (r < 0) r = 0;
	if (c < 0) c = 0;
	if (r >= SHEET_ROWS) r = SHEET_ROWS - 1;
	if (c >= SHEET_COLS) c = SHEET_COLS - 1;

	int old_r = cur_row, old_c = cur_col;

	// Whether a multi-cell selection was on screen before this move.
	bool had_sel = (sel_row != old_r || sel_col != old_c);

	cur_row = r;
	cur_col = c;

	if (!extend) { sel_row = r; sel_col = c; }

	// Extending a selection, or dropping one, changes cells other than
	// the two ends -- there is no cheap set of them to redraw, so the
	// whole view goes. A plain move between two unselected cells is
	// the common case and repaints two cells.
	if (extend || had_sel) {
		scroll_to_cursor();
		repaint_view();
		return;
	}

	move_repaint(old_r, old_c);

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	bool shift = (mods & Z_KBD_MOD_SHIFT) != 0;
	bool ctrl = (mods & Z_KBD_MOD_CTRL) != 0;
	bool alt = (mods & Z_KBD_MOD_ALT) != 0;

	mods_now = mods;

	if (menu) { menu_key(keysym); return; }
	if (chart_mode && keysym != Z_KEY_HELP) { chart_key(keysym); return; }

	// F1: help (zdialog.h, z_dialog_help(); docs/app_help.md).
	if (keysym == Z_KEY_HELP) { show_help(); return; }

	if (editing) {

		switch (keysym) {

			case '\n':
			case '\r':
				commit_edit();
				move_to(cur_row + 1, cur_col, false);
				return;

			case '\t':
				commit_edit();
				move_to(cur_row, cur_col + (shift ? -1 : 1), false);
				return;

			case 0x1b:				// Escape
				cancel_edit();
				return;

			case '\b':
			case 0x7f:
				edit_backspace();
				return;

			case Z_KEY_DELETE:
				edit_delete();
				return;

			case Z_KEY_LEFT:
				if (edit_caret) { edit_caret--; draw_bar(); }
				return;

			case Z_KEY_RIGHT:
				if (edit_caret < edit_len) { edit_caret++; draw_bar(); }
				return;

			case Z_KEY_HOME:
				edit_caret = 0;
				draw_bar();
				return;

			case Z_KEY_END:
				edit_caret = edit_len;
				draw_bar();
				return;

			// Up and Down commit and move, the way a spreadsheet does
			// -- typing down a column should not need Enter between
			// every cell.
			case Z_KEY_UP:
				commit_edit();
				move_to(cur_row - 1, cur_col, false);
				return;

			case Z_KEY_DOWN:
				commit_edit();
				move_to(cur_row + 1, cur_col, false);
				return;

			default:
				if (keysym >= 0x20 && keysym < 0x7f)
					edit_insert((char)keysym);
				return;

		}

	}

	// -- not editing --

	if (ctrl) {

		switch (keysym) {
			case 'z': case 'Z': if (shift) do_redo(); else do_undo(); return;
			case 'y': case 'Y': do_redo(); return;
			case 'd': case 'D': do_fill(true); return;
			case 'r': case 'R': do_fill(false); return;
			case 'g': case 'G': do_goto(); return;
			case 'f': case 'F': do_find(); return;
			case '1':           do_format(); return;
			case 'c': case 'C': do_copy(); return;
			case 'x': case 'X': do_copy(); do_clear_selection(); return;
			case 'v': case 'V': do_paste(); return;
			case 's': case 'S': do_save(); return;
			case 'o': case 'O': do_open(); return;
			case 'n': case 'N': do_new(); return;
			case Z_KEY_HOME: move_to(0, 0, false); return;
			default: break;
		}

	}

	// Alt+Left/Right resizes the current column. There is no
	// unmodified key spare for this -- every printable character
	// starts an entry -- and a mouse-only way to change a column would
	// be a dead end on a machine with no pointer (see
	// docs/window_manager.md on keyboard-only operation being
	// first-class here).
	if (alt && (keysym == Z_KEY_LEFT || keysym == Z_KEY_RIGHT)) {

		will_change_meta();
		sheet_set_colw(&sh, cur_col,
			sheet_colw(&sh, cur_col) + (keysym == Z_KEY_RIGHT ? 1 : -1));

		update_title();
		scroll_to_cursor();
		repaint_view();

		return;

	}

	switch (keysym) {

		case Z_KEY_UP:    move_to(cur_row - 1, cur_col, shift); return;
		case Z_KEY_DOWN:  move_to(cur_row + 1, cur_col, shift); return;
		case Z_KEY_LEFT:  move_to(cur_row, cur_col - 1, shift); return;
		case Z_KEY_RIGHT: move_to(cur_row, cur_col + 1, shift); return;

		case Z_KEY_PAGEUP:
			move_to(cur_row - (vis_rows > 0 ? vis_rows : 1), cur_col, shift);
			return;

		case Z_KEY_PAGEDOWN:
			move_to(cur_row + (vis_rows > 0 ? vis_rows : 1), cur_col, shift);
			return;

		case Z_KEY_HOME:  move_to(cur_row, 0, shift); return;

		case Z_KEY_END: {
			int rows, cols;
			sheet_extent(&sh, &rows, &cols);
			move_to(cur_row, cols ? cols - 1 : 0, shift);
			return;
		}

		case '\t':
			move_to(cur_row, cur_col + (shift ? -1 : 1), false);
			return;

		case '\n':
		case '\r':
			move_to(cur_row + 1, cur_col, false);
			return;

		case Z_KEY_RENAME:	// F2: edit the cell
			begin_edit(true);
			draw_bar();
			return;

		case Z_KEY_FIND:	// F3: find, then the next match
			find_next();
			return;

		case Z_KEY_F5:		// sheet's own: the chart
			do_chart();
			return;

		// The command line, as in VisiCalc and 1-2-3. A cell's text can
		// still start with '/': type ' first ('/2 is the text "/2").
		// F4 is the app prompt everywhere (docs/keys.md).
		case Z_KEY_PROMPT:
		case '/':
			menu = M_TOP;
			menu_hot = 0;
			draw_menu();
			return;

		case '\b':
		case 0x7f:
		case Z_KEY_DELETE:
			do_clear_selection();
			return;

		default:
			break;

	}

	// Any printable character starts a fresh entry, replacing what was
	// there -- including '=', which is how a formula begins.
	if (keysym >= 0x20 && keysym < 0x7f) {
		begin_edit(false);
		edit_insert((char)keysym);
	}

}

// ---------------------------------------------------------------
// mouse
// ---------------------------------------------------------------

// True if `cx` is within a few pixels of the right edge of a column
// header, and sets *col to that column. This is the grab area for a
// width drag.
static bool hit_col_edge(int cx, int *col) {

	int nc = vis_cols();
	int x = grid_x;

	for (int i = 0; i < nc; i++) {

		int c = left_col + i;
		x += COL_PX(c);

		if (cx >= x - 3 && cx <= x + 1) { *col = c; return true; }

	}

	return false;

}

static void handle_mouse(uint32_t packed) {

	if (chart_mode) return;

	int cx, cy;

	z_win_mouse_content_xy(&win, packed, &cx, &cy);

	uint8_t buttons = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(packed);
	bool down = (buttons & Z_MOUSE_BTN_LEFT) != 0;
	bool was = (last_buttons & Z_MOUSE_BTN_LEFT) != 0;

	last_buttons = buttons;

	// A width drag in progress owns the pointer until it is released.
	if (width_drag_col >= 0) {

		if (!down) { width_drag_col = -1; return; }

		int delta = (cx - width_drag_x0) / CHAR_W;
		int w = width_drag_w0 + delta;

		if (w != sheet_colw(&sh, width_drag_col)) {
			sheet_set_colw(&sh, width_drag_col, w);
			update_title();
			repaint_view();
		}

		return;

	}

	// So does a scrollbar. z_scrollbar_has_pointer() is what tells
	// "not mine" from "mine, but nothing changed" -- without it, a
	// click on the thumb would fall through and start selecting cells.
	if (z_scrollbar_has_pointer(&vsb, cx, cy)) {
		if (z_scrollbar_mouse(&vsb, cx, cy, buttons)) {
			top_row = vsb.value;
			repaint_view();
		}
		return;
	}

	if (z_scrollbar_has_pointer(&hsb, cx, cy)) {
		if (z_scrollbar_mouse(&hsb, cx, cy, buttons)) {
			left_col = hsb.value;
			repaint_view();
		}
		return;
	}

	if (selecting) {

		if (!down) { selecting = false; return; }

		int r, c;

		if (cell_at(cx, cy, &r, &c) && (r != cur_row || c != cur_col)) {
			cur_row = r;
			cur_col = c;
			scroll_to_cursor();
			repaint_view();
		}

		return;

	}

	if (!down || was) return;

	// -- a fresh press --

	if (cy < BAR_H) {

		// The edit bar. Clicking it opens the current cell for
		// editing, which is the mouse equivalent of F2.
		if (!editing) {
			begin_edit(true);
			draw_bar();
		}

		return;

	}

	if (cy < BAR_H + LINE_H) {

		int col;

		if (hit_col_edge(cx, &col)) {
			width_drag_col = col;
			width_drag_x0 = cx;
			width_drag_w0 = sheet_colw(&sh, col);
		}

		return;

	}

	int r, c;

	if (!cell_at(cx, cy, &r, &c)) return;

	if (editing) commit_edit();

	int old_r = cur_row, old_c = cur_col;
	bool had_sel = (sel_row != cur_row || sel_col != cur_col);

	cur_row = r;
	cur_col = c;

	// Shift+click extends the selection from wherever it was anchored,
	// matching shift+arrow. Z_WM_MOUSE carries no modifiers, so this
	// uses the ones from the last key event -- see mods_now.
	if (!(mods_now & Z_KBD_MOD_SHIFT)) {
		sel_row = r;
		sel_col = c;
		selecting = true;
	}

	if (had_sel || (mods_now & Z_KBD_MOD_SHIFT)) {
		scroll_to_cursor();
		repaint_view();
	} else {
		move_repaint(old_r, old_c);
	}

}

// ---------------------------------------------------------------
// messages
// ---------------------------------------------------------------

static void forward_msg(z_msg_t *msg, void *user) {

	(void)user;

	switch (msg->subject) {

		case Z_WM_SET_CLIP:
			z_win_apply_clip(&win, &msg->obj);
			break;

		case Z_WM_REDRAW:

			if (msg->obj.type != Z_UINT32) break;

			// Only ours. A dialog's own redraws are handled inside
			// zdialog.c and never reach here, but this is also the
			// message that arrives while a dialog is being created,
			// before it has a window id to compare against.
			if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;

			z_win_apply_redraw(&win, msg->obj.val.uint32);
			repaint();
			z_win_redraw_done(&win);

			break;

		case Z_WM_WINDOW_MOVED:

			// No layout() -- moving doesn't change our size, and every
			// rect we hold is content-relative.
			z_win_parse_rect(&win, &msg->obj);
			break;

		case Z_WM_WINDOW_RESIZED:

			// Arrives BEFORE the Z_WM_REDRAW that follows a resize
			// (zwm.h guarantees that ordering), so the layout is
			// already correct by the time we are asked to repaint at
			// the new size.
			z_win_apply_resized(&win, &msg->obj);
			layout();
			scroll_to_cursor();
			break;

		default:
			break;

	}

}

int main(void) {

	printf("sheet: starting\n");

	sheet_init(&sh);
	mem_init();

	cur_row = cur_col = sel_row = sel_col = 0;
	top_row = left_col = 0;

	// Take the launch argument BEFORE creating the window, so the
	// window can be created with its final title -- see sw/apps/text's
	// own note on the redraw race that retitling immediately after
	// creation caused.
	char launch_arg[sizeof(filename)];
	bool have_arg = z_launch_path_take(launch_arg, sizeof(launch_arg));

	char initial_title[40];
	build_title(initial_title, (int)sizeof(initial_title),
		have_arg ? launch_arg : "", false);

	strcpy(sent_title, initial_title);

	// CLOSE_ICON WITHOUT CLOSE_KILLS_OWNER, deliberately: this app owns
	// more than one window at a time (a dialog is a window), and the
	// killing form takes every window of a pid down the instant any one
	// of them is clicked closed. Closing with unsaved changes also has
	// to get a chance to ask.
	if (z_win_create_flags(&win, initial_title, WIN_W, WIN_H, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_RESIZABLE |
		Z_WIN_FLAG_MIN_IS_CREATE |
		Z_WIN_FLAG_NEW_ICON | Z_WIN_FLAG_OPEN_ICON |
		Z_WIN_FLAG_SAVE_ICON | Z_WIN_FLAG_FONT_ICON) != Z_OK) {
		printf("sheet: failed to create window -- is wm running?\n");
		return 1;
	}

	z_scrollbar_init(&vsb, &win, Z_SB_VERT);
	z_scrollbar_init(&hsb, &win, Z_SB_HORZ);

	dlg_ctx.parent = &win;
	dlg_ctx.on_msg = forward_msg;
	dlg_ctx.user = NULL;

	layout();

	// The file the browser launched us with, taken above. load_path()
	// reports its own failures, but it needs the window to exist,
	// because it reports them in a dialog.
	if (have_arg) load_path(launch_arg);

	update_title();
	repaint();

	for (;;) {

		z_msg_t msg;

		// Drain the whole queue each pass rather than one message per
		// iteration -- see Z_WM_MOUSE's own note in zwm.h on why
		// handling one per loop makes an app fall progressively behind
		// the real cursor.
		while (z_msg_read(&msg) == Z_OK) {

			switch (msg.subject) {

				case Z_WM_KEY:

					if (msg.obj.type != Z_UINT32) break;
					if (!Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) {
						// Modifier releases matter: a shift let go
						// between a key and a click would otherwise
						// leave mods_now stale and turn the next plain
						// click into a shift+click.
						mods_now = (uint8_t)
							Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32);
						break;
					}

					handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
						(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32));

					break;

				case Z_WM_WHEEL:
					// Three rows a notch, up = back, like the other apps:
					// the view moves over the sheet, the cell cursor
					// stays -- as with the scrollbar, whose range bounds
					// it.
					if (msg.obj.type == Z_UINT32) {
						int t = top_row - 3 * Z_WM_WHEEL_NOTCHES(msg.obj.val.uint32);
						int max = vsb.total - vsb.page;
						if (t > max) t = max;
						if (t < 0) t = 0;
						if (t != top_row) {
							top_row = t;
							z_scrollbar_set_value(&vsb, top_row);
							repaint_view();
						}
					}
					break;

				case Z_WM_MOUSE:

					if (msg.obj.type == Z_UINT32)
						handle_mouse(msg.obj.val.uint32);

					break;

				case Z_WM_TITLEBAR_ICON: {

					if (msg.obj.type != Z_UINT32) break;

					uint32_t v = msg.obj.val.uint32;
					if ((int)Z_WM_UNPACK_TBICON_ID(v) != win.id) break;

					switch (Z_WM_UNPACK_TBICON_KIND(v)) {
						case Z_WM_TBICON_NEW:  do_new(); break;
						case Z_WM_TBICON_OPEN: do_open(); break;
						case Z_WM_TBICON_SAVE: do_save(); break;
						case Z_WM_TBICON_FONT: do_font(); break;
						default: break;
					}

					break;

				}

				case Z_WM_CLOSE:

					if (msg.obj.type == Z_UINT32 &&
						(int32_t)msg.obj.val.uint32 == win.id)
						do_close();

					break;

				default:

					forward_msg(&msg, NULL);
					break;

			}

		}

		/* Block until something arrives.
		 *
		 * Everything here is driven by wm messages, so a plain
		 * z_proc_wait(0) would be correct today. The timeout is
		 * insurance for the periodic work an editor tends to acquire
		 * (a blinking caret, an autosave) rather than for anything
		 * present now -- see docs/app_runtime.md on why a spinning
		 * app costs far more than the work it does. */
		z_proc_wait(Z_TICK_HZ / 30);

	}

	return 0;

}
