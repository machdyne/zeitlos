/*
 * Render sw/apps/cal's panel to an image.
 *
 *   cd sw/apps/cal && make render
 *
 * See sw/common/tests/zrender.h. cal.c, cal_core.c, zrtc.c, zwin.c and
 * zwidget.c are the REAL sources; only the pixel plotting is software.
 *
 * -- why a calendar in particular needs this --
 *
 * tests/cal_test.c already proves the grid is arithmetically right:
 * every cell of every month from 1970 to 2105 holds the day it should
 * and sits in the column that day actually fell on. None of that says
 * anything about whether the 1st is under the right heading on
 * screen, whether the today highlight covers its number, or whether
 * six rows fit in the window.
 *
 * Those are exactly the failures a calendar has, and they are all
 * obvious in one look and invisible to an assertion.
 *
 * -- the four states --
 *
 * Rendered separately because they are genuinely different panels,
 * and because three of them are awkward to reach on real hardware:
 * the epoch needs a machine with no network, the six-row month needs
 * you to page to one, and the year-entry line only exists mid-keypress.
 *
 *   make render                 today marked, ordinary five-row month
 *   make render WHAT=sixrow     the worst case for the window height
 *   make render WHAT=epoch      no clock: 1970, no highlight, dead `<`
 *   make render WHAT=entry      a year being typed
 */

#include "../../../common/tests/zrender.h"

// cal.c's main() is replaced by this file's own -- everything else in
// it, including layout(), draw_grid() and the widget setup, is the
// shipped code.
#define main cal_main_unused
#include "../cal.c"
#undef main

// -- stubs -------------------------------------------------------
//
// Only the RTC register layer. Everything else cal.c calls comes from
// zwin.c, zwidget.c, zrtc.c and zeitlos.c, which are linked -- and
// the calendar arithmetic in particular is the real thing, so a month
// rendered here is a month the target would draw.
//
// z_rtc_available()/z_rtc_valid()/z_rtc_seconds() are static inlines
// over MMIO in zrtc.h, so they cannot be overridden by linking. The
// test drives cal.c's own rtc_ok / today_* state directly instead,
// which is the state those functions exist to produce.

static void set_today(int32_t y, uint8_t m, uint8_t d) {
	rtc_ok = true;
	today_year = y;
	today_month = m;
	today_day = d;
	shown_day_index = 0;
}

static void set_no_clock(void) {
	rtc_ok = true;			// the RTC exists, it has just never been set
	today_day = 0;
	shown_day_index = -1;
}

int main(int argc, char **argv) {

	const char *out = argc > 1 ? argv[1] : "/tmp/cal.pbm";
	const char *what = argc > 2 ? argv[2] : "today";

	if (!z_render_open(&win, WIN_W, WIN_H)) {
		printf("render: skipped (cannot map the VRAM address)\n");
		return 77;
	}

	cal_entry_reset(&entry);

	if (!strcmp(what, "epoch")) {

		// A machine that has never had a network: opens on the
		// epoch, marks nothing, and the back arrow has nowhere to
		// go. That last one is why zwidget.c draws a disabled
		// button as an empty frame -- look at the `<` here.
		set_no_clock();
		cur_year = CAL_EPOCH_YEAR;
		cur_month = CAL_EPOCH_MONTH;

	} else if (!strcmp(what, "sixrow")) {

		// August 2026: 31 days starting on a Saturday, the only
		// shape that fills all six rows under a Sunday-first week.
		// If the window is ever too short, this is the render that
		// shows it.
		set_today(2026, 8, 29);
		cur_year = 2026;
		cur_month = 8;

	} else if (!strcmp(what, "nortc")) {

		// A bitstream built without `RTC. The calendar is fully
		// usable -- that is the point of it not depending on the
		// clock for anything but the highlight -- and the status
		// line carries the longest fixed string the app has, which
		// is exactly why this state gets its own render.
		rtc_ok = false;
		today_day = 0;
		shown_day_index = -1;
		cur_year = CAL_EPOCH_YEAR;
		cur_month = CAL_EPOCH_MONTH;

	} else if (!strcmp(what, "entry")) {

		// Mid-keypress: three digits of a year typed, the heading
		// replaced by the buffer, the status line saying what the
		// remaining keys do.
		set_no_clock();
		cur_year = CAL_EPOCH_YEAR;
		cur_month = CAL_EPOCH_MONTH;
		cal_entry_digit(&entry, '2');
		cal_entry_digit(&entry, '0');
		cal_entry_digit(&entry, '2');

	} else {

		// The ordinary case: September 2026, today the 6th.
		set_today(2026, 9, 6);
		cur_year = 2026;
		cur_month = 9;

	}

	widgets_init();
	reload_month();
	layout();
	repaint();

	z_render_write(out, &win, 2);

	return 0;

}
