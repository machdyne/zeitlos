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

// -- the document --

static sheet_t sh;

static char filename[80];		// "" when never saved

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

	const char *base = path;

	for (const char *p = path; *p; p++)
		if (*p == '/') base = p + 1;

	int n = 0;
	const char *pre = "sheet: ";

	while (pre[n] && n < cap - 1) { t[n] = pre[n]; n++; }

	if (!*base) {
		const char *u = "untitled";
		for (int i = 0; u[i] && n < cap - 1; i++) t[n++] = u[i];
	} else {
		for (int i = 0; base[i] && n < cap - 1; i++) t[n++] = base[i];
	}

	if (star && n < cap - 1) t[n++] = '*';

	t[n] = 0;

}

// What was last sent, so an unchanged title costs no message. wm
// repairs the titlebar strip on every Z_WM_SET_TITLE and tells the
// windows underneath to repaint; doing that on every keystroke would
// be a visible flicker across the whole screen.
static char sent_title[40];

static void update_title(void) {

	char t[40];

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

static void draw_bar(void) {

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
static void repaint_view(void) {

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

static z_dialog_ctx_t dlg_ctx;

static char last_dir[Z_FLIST_PATH_MAX] = "/";

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

static bool do_save_to(const char *path) {

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

	char path[80];

	const char *suggest = filename[0] ? filename : "SHEET.ZSS";

	if (!z_dialog_save(&dlg_ctx, last_dir, suggest, path, sizeof(path)))
		return false;

	bool ok = do_save_to(path);

	repaint();

	return ok;

}

static bool do_save(void) {

	if (!filename[0]) return do_save_as();

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

	filename[0] = 0;
	cur_row = cur_col = sel_row = sel_col = 0;
	top_row = left_col = 0;
	editing = false;

	update_title();
	repaint();

}

static void do_open(void) {

	if (!confirm_discard()) { repaint(); return; }

	char path[80];

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

static char clip_buf[1024];

static void do_copy(void) {

	int r0, c0, r1, c1;
	sel_rect(&r0, &c0, &r1, &c1);

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

	z_clip_set(clip_buf, n);

}

static void do_paste(void) {

	int got = z_clip_get(clip_buf, (int)sizeof(clip_buf));
	if (!got) return;

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

	sheet_clear_range(&sh, r0, c0, r1, c1);

	update_title();
	repaint_view();

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

		case Z_KEY_F2:
			begin_edit(true);
			draw_bar();
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

	cur_row = cur_col = sel_row = sel_col = 0;
	top_row = left_col = 0;

	// Take the launch argument BEFORE creating the window, so the
	// window can be created with its final title -- see sw/apps/text's
	// own note on the redraw race that retitling immediately after
	// creation caused.
	char launch_arg[sizeof(filename)];
	bool have_arg = z_launch_arg_take(launch_arg, sizeof(launch_arg));

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
