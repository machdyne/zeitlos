/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Geometry invariants for sw/apps/cal's panel. Runs on the HOST:
 *
 *   cd sw/apps/cal && make test
 *
 * The companion to tests/render.c, not a replacement for it. The
 * render is for looking at; this is for the relationships that can be
 * written down, so they are checked on every build rather than the
 * next time somebody happens to generate a picture.
 *
 * See sw/common/tests/zrender.h's header for why both exist: an
 * assertion only covers a relationship somebody thought of, and a
 * render covers all of them at once but only when a human looks. The
 * split is deliberate.
 *
 * What is worth asserting here is everything that would put pixels
 * where they cannot be seen:
 *
 *   - the grid, both arrows and the status line all inside the
 *     content rect, with six rows allocated rather than five
 *   - nothing overlapping anything else, in particular the arrows
 *     against the month heading
 *   - at least 2px between the two arrows, which the focus ring
 *     requires (docs/widgets.md) and which is invisible until a ring
 *     rubs out a neighbour's frame
 *   - the widest heading and the widest status line both fitting
 */

#include "../../../common/tests/zrender.h"

#define main cal_main_unused
#include "../cal.c"
#undef main

static int checks, fails;

static void ok(bool cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("  FAIL: %s\n", what); }
}

static void ge(int got, int least, const char *what) {
	checks++;
	if (got < least) {
		fails++;
		printf("  FAIL: %s -- got %d, want >= %d\n", what, got, least);
	}
}

static void le(int got, int most, const char *what) {
	checks++;
	if (got > most) {
		fails++;
		printf("  FAIL: %s -- got %d, want <= %d\n", what, got, most);
	}
}

// Do two rectangles overlap?
static bool overlaps(int ax, int ay, int aw, int ah,
	int bx, int by, int bw, int bh) {
	return !(ax + aw <= bx || bx + bw <= ax ||
			 ay + ah <= by || by + bh <= ay);
}

int main(void) {

	int cw, ch;

	printf("cal layout tests\n\n");

	if (!z_render_open(&win, WIN_W, WIN_H)) {
		printf("skipped (cannot map the VRAM address)\n");
		return 77;
	}

	widgets_init();
	reload_month();
	layout();

	cw = z_win_content_w(&win);
	ch = z_win_content_h(&win);

	printf("window %dx%d, content %dx%d\n", WIN_W, WIN_H, cw, ch);

	// -- the window is big enough for what it promises --

	printf("content fits\n");

	ge(cw, CAL_COLS * CELL_W, "content width holds the grid");
	ge(ch, MARGIN + BTN_H + GAP + HDR_H + CAL_ROWS * CELL_H + STATUS_H,
		"content height holds the whole panel");

	// Six rows, not five. A 31-day month starting in the last
	// column needs all six, and allocating five would silently drop
	// its last days off the bottom -- see cal_core.h.
	ok(CAL_ROWS == 6, "six rows are allocated");

	// -- everything inside the content rect --

	printf("inside the content area\n");

	ge(grid_x, 0, "grid left edge");
	le(grid_x + CAL_COLS * CELL_W, cw, "grid right edge");
	ge(hdr_y, 0, "weekday header top");
	ge(grid_y, hdr_y + HDR_H, "grid starts below the weekday header");
	le(grid_y + CAL_ROWS * CELL_H, ch, "grid bottom edge");

	ge(status_y, grid_y + CAL_ROWS * CELL_H, "status below a full six-row grid");
	le(status_y + STATUS_H, ch, "status bottom edge");

	ge(title_y, 0, "heading top");
	le(title_y + z_font_6x12.h, ch, "heading bottom");
	ge(title_x, 0, "heading left");
	le(title_x + title_w, cw, "heading right");

	for (int i = 0; i < W_COUNT; i++) {
		ge(widgets[i].x, 0, "widget left edge");
		ge(widgets[i].y, 0, "widget top edge");
		le(widgets[i].x + widgets[i].w, cw, "widget right edge");
		le(widgets[i].y + widgets[i].h, ch, "widget bottom edge");
	}

	// The focus ring is drawn one pixel OUTSIDE a widget, so a
	// widget flush against the content edge would have its ring
	// clipped away and appear never to take focus.
	for (int i = 0; i < W_COUNT; i++) {
		ge(widgets[i].x - 1, 0, "focus ring clears the left edge");
		ge(widgets[i].y - 1, 0, "focus ring clears the top edge");
		le(widgets[i].x + widgets[i].w + 1, cw, "focus ring clears the right");
	}

	// -- nothing on top of anything else --

	printf("no overlaps\n");

	ok(!overlaps(widgets[W_PREV].x, widgets[W_PREV].y,
			widgets[W_PREV].w, widgets[W_PREV].h,
			widgets[W_NEXT].x, widgets[W_NEXT].y,
			widgets[W_NEXT].w, widgets[W_NEXT].h),
		"the two arrows do not overlap");

	// >= 2px between them, because the ring sits one pixel outside
	// each. This is the constraint docs/widgets.md calls out, and
	// the failure it prevents (a ring erasing the neighbour's
	// frame) looks like a rendering fault rather than a spacing one.
	ge(widgets[W_NEXT].x - (widgets[W_PREV].x + widgets[W_PREV].w), 2,
		"2px between the arrows for their focus rings");

	for (int i = 0; i < W_COUNT; i++) {
		ok(!overlaps(widgets[i].x, widgets[i].y, widgets[i].w, widgets[i].h,
				title_x, title_y, title_w, z_font_6x12.h),
			"arrow does not overlap the heading");
		ok(!overlaps(widgets[i].x, widgets[i].y, widgets[i].w, widgets[i].h,
				grid_x, hdr_y, CAL_COLS * CELL_W,
				HDR_H + CAL_ROWS * CELL_H),
			"arrow does not overlap the grid");
	}

	ok(!overlaps(grid_x, grid_y, CAL_COLS * CELL_W, CAL_ROWS * CELL_H,
			0, status_y, cw, STATUS_H),
		"the grid does not overlap the status line");

	// -- the text actually fits --

	printf("text fits\n");

	// The widest heading there is: the longest month name plus a
	// four-digit year. If a constant shrinks the window, this is
	// what says so rather than a clipped word on screen.
	{
		int widest = 0;
		for (uint8_t m = 1; m <= 12; m++) {
			char buf[24];
			int w;
			snprintf(buf, sizeof(buf), "%s %04d", z_month_name_long(m), 2026);
			w = (int)strlen(buf) * z_font_6x12.w;
			if (w > widest) widest = w;
		}
		le(widest, title_w, "the longest month heading fits between the arrows");
		printf("  (widest heading %dpx, %dpx available)\n", widest, title_w);
	}

	// The widest day number in a cell.
	le(2 * z_font_6x12.w, CELL_W - 2, "a two-digit day fits its cell");

	// Two-letter weekday initials.
	le(2 * z_font_6x12.w, CELL_W - 2, "a weekday initial fits its column");

	// The status line, in its longest form. Built the same way
	// draw_status() builds it, so a change there that overruns is
	// caught here.
	{
		char buf[40];
		int widest = 0;
		for (uint8_t m = 1; m <= 12; m++) {
			int w;
			snprintf(buf, sizeof(buf), "Today %s %d %s %04d UTC",
				z_wday_name(3), 30, z_month_name(m), 2026);
			w = (int)strlen(buf) * z_font_5x8.w;
			if (w > widest) widest = w;
		}
		le(widest, cw, "the longest today line fits");

		// The app's own symbols, not copies of them -- see the
		// comment on these in cal.c.
		le((int)strlen(STATUS_NO_RTC) * z_font_5x8.w, cw,
			"the no-RTC message fits");
		le((int)strlen(STATUS_NOT_SET) * z_font_5x8.w, cw,
			"the clock-not-set message fits");
		le((int)strlen(STATUS_ENTRY) * z_font_5x8.w, cw,
			"the year-entry hint fits");
	}

	// -- the grid maps back to the cells it is drawn in --
	//
	// cal_test.c checks the day numbers; this checks that the cell
	// each one lands in is on screen, which is the other half.

	printf("cells on screen\n");

	{
		cal_month_t m;
		int worst = 0;

		// August 2026 -- 31 days from a Saturday, the six-row case.
		ok(cal_month_info(2026, 8, &m), "six-row month resolves");
		ok(m.rows == 6, "  and really needs six rows");

		for (int row = 0; row < CAL_ROWS; row++)
			for (int col = 0; col < CAL_COLS; col++)
				if (cal_cell_day(&m, row, col)) {
					int y = grid_y + row * CELL_H + CELL_H;
					if (y > worst) worst = y;
				}

		le(worst, ch, "the last row of a six-row month is on screen");
		le(worst, status_y, "  and above the status line");
	}

	printf("\n%d checks, %d failures\n", checks, fails);

	return fails ? 1 : 0;

}
