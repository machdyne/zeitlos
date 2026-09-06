/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Test suite for cal_core.c. Runs on the HOST, not the target:
 *
 *   cd sw/apps/cal && make test
 *
 * Links the REAL sw/common/zrtc.c, not a stub. That is the point of
 * cal_core.c having no date arithmetic of its own -- the thing under
 * test here is the grid, and the grid is only as right as the
 * calendar underneath it, so testing them together is testing what
 * actually ships.
 *
 * -- what is worth testing in a calendar --
 *
 * Not "does February have 28 days". zrtc.c settled that and has its
 * own reasons to be right. What this checks is the layer that turns a
 * month into a 7x6 grid, where the failures are:
 *
 *   - the 1st in the wrong column (an off-by-one in the week-start
 *     rotation, invisible unless you know what day it was)
 *   - a month needing a sixth row and not getting one, so the last
 *     days silently vanish
 *   - cell -> day and day -> cell disagreeing, so the today highlight
 *     lands on the wrong square
 *   - navigation walking off either end of the representable range
 *   - year entry accepting something it should not
 *
 * The grid tests are written as whole-month walks rather than spot
 * checks: every cell of every month across a span of years, verified
 * against the day count and against the round trip. A spot check only
 * finds the case somebody thought of, which is exactly how the
 * off-by-one survives.
 */

#include <stdio.h>
#include <string.h>

#include "../cal_core.h"
#include "../../../common/zrtc.h"

static int checks, fails;

static void ok(bool cond, const char *what) {
	checks++;
	if (!cond) {
		fails++;
		printf("  FAIL: %s\n", what);
	}
}

static void eq_int(long got, long want, const char *what) {
	checks++;
	if (got != want) {
		fails++;
		printf("  FAIL: %s -- got %ld, want %ld\n", what, got, want);
	}
}

static void eq_str(const char *got, const char *want, const char *what) {
	checks++;
	if (strcmp(got, want) != 0) {
		fails++;
		printf("  FAIL: %s -- got \"%s\", want \"%s\"\n", what, got, want);
	}
}

// -- known-good anchors ------------------------------------------
//
// Dates verified independently, so the whole suite is not just
// cal_core agreeing with itself.

static void test_anchors(void) {

	cal_month_t m;

	printf("anchors\n");

	// 2026-09-01 is a Tuesday, and September has 30 days.
	ok(cal_month_info(2026, 9, &m), "Sep 2026 resolves");
	eq_int(m.first_wday, 2, "Sep 2026 starts Tuesday");
	eq_int(m.days, 30, "Sep 2026 has 30 days");
	eq_int(m.lead, 2, "Sep 2026 lead cells");
	eq_int(m.rows, 5, "Sep 2026 rows");

	// Leap day, and a leap year that is not divisible by 400's
	// neighbour -- the rule zrtc.c owns, spot-checked here only
	// because a wrong answer would look like a grid bug.
	ok(cal_month_info(2024, 2, &m), "Feb 2024 resolves");
	eq_int(m.days, 29, "Feb 2024 has 29 days");

	ok(cal_month_info(2100, 2, &m), "Feb 2100 resolves");
	eq_int(m.days, 28, "Feb 2100 has 28 days (century, not /400)");

	ok(cal_month_info(2000, 2, &m), "Feb 2000 resolves");
	eq_int(m.days, 29, "Feb 2000 has 29 days (/400)");

	// The epoch itself, which is what the app opens on with no
	// clock. 1970-01-01 was a Thursday.
	ok(cal_month_info(CAL_EPOCH_YEAR, CAL_EPOCH_MONTH, &m), "epoch resolves");
	eq_int(m.first_wday, 4, "1970-01-01 is a Thursday");
	eq_int(m.days, 31, "Jan 1970 has 31 days");

}

// -- the six-row case --------------------------------------------
//
// The one that decides the window's height. A 31-day month whose 1st
// falls in the last column needs six rows; nothing shorter does.

static void test_six_rows(void) {

	cal_month_t m;
	int32_t y;
	uint8_t mo;
	int seen_six = 0, seen_four = 0;

	printf("row counts\n");

	// August 2026 starts on a Saturday with 31 days -- the worst
	// case under a Sunday-first week.
	ok(cal_month_info(2026, 8, &m), "Aug 2026 resolves");
	eq_int(m.first_wday, 6, "Aug 2026 starts Saturday");
	eq_int(m.rows, 6, "Aug 2026 needs six rows");

	// February in a non-leap year starting on the week's first
	// column is the only four-row month there is.
	ok(cal_month_info(2026, 2, &m), "Feb 2026 resolves");
	eq_int(m.lead, 0, "Feb 2026 starts in column 0");
	eq_int(m.rows, 4, "Feb 2026 needs four rows");

	// Nothing anywhere in the range may exceed CAL_ROWS, and both
	// extremes must actually occur -- if six never happened the
	// grid would be oversized for no reason, and if it happened and
	// we allocated five the last days would vanish.
	for (y = CAL_YEAR_MIN; y <= CAL_YEAR_MAX; y++) {
		for (mo = 1; mo <= 12; mo++) {
			if (!cal_month_info(y, mo, &m)) {
				printf("  FAIL: %04ld-%02u did not resolve\n", (long)y, mo);
				checks++; fails++;
				continue;
			}
			if (m.rows > CAL_ROWS) {
				printf("  FAIL: %04ld-%02u wants %u rows\n",
					(long)y, mo, m.rows);
				checks++; fails++;
			}
			if (m.rows == 6) seen_six++;
			if (m.rows == 4) seen_four++;
		}
	}

	ok(seen_six > 0, "six-row months occur in range");
	ok(seen_four > 0, "four-row months occur in range");

	printf("  (%d six-row and %d four-row months in %d-%d)\n",
		seen_six, seen_four, CAL_YEAR_MIN, CAL_YEAR_MAX);

}

// -- the grid itself ---------------------------------------------
//
// Every cell of every month across a long span. Three properties,
// checked together because each one alone can hold while the layout
// is still wrong:
//
//   1. the cells holding a day are exactly 1..days, each once, in
//      ascending order
//   2. every day's cell agrees with cal_cell_of_day()
//   3. the weekday implied by a cell's column matches what the
//      calendar says that date actually was

static void test_grid(void) {

	int32_t y;
	uint8_t mo;
	int bad = 0;

	printf("grid consistency\n");

	for (y = 1970; y <= 2105 && bad < 5; y++) {
		for (mo = 1; mo <= 12 && bad < 5; mo++) {

			cal_month_t m;
			int row, col, expect = 1;

			if (!cal_month_info(y, mo, &m)) { bad++; continue; }

			for (row = 0; row < CAL_ROWS; row++) {
				for (col = 0; col < CAL_COLS; col++) {

					int day = cal_cell_day(&m, row, col);
					int r2, c2;
					z_tm_t tm;

					if (day == 0) continue;

					// 1: ascending, no gaps, no repeats
					if (day != expect) {
						printf("  FAIL: %04ld-%02u cell (%d,%d) is %d, "
							"expected %d\n", (long)y, mo, row, col, day,
							expect);
						checks++; fails++; bad++;
						continue;
					}
					expect++;

					// 2: round trip
					if (!cal_cell_of_day(&m, day, &r2, &c2) ||
						r2 != row || c2 != col) {
						printf("  FAIL: %04ld-%02u day %d round trip -> "
							"(%d,%d), want (%d,%d)\n", (long)y, mo, day,
							r2, c2, row, col);
						checks++; fails++; bad++;
						continue;
					}

					// 3: the column really is that weekday
					tm.year = y; tm.month = mo; tm.day = (uint8_t)day;
					tm.hour = tm.min = tm.sec = 0;
					tm.wday = 0; tm.yday = 0;
					z_time_to_tm(z_tm_to_time(&tm), &tm);

					if (cal_wday_col(tm.wday) != col) {
						printf("  FAIL: %04ld-%02u-%02d is a %s but sits "
							"in column %d\n", (long)y, mo, day,
							z_wday_name(tm.wday), col);
						checks++; fails++; bad++;
					}

				}
			}

			// every day placed
			if (expect != (int)m.days + 1) {
				printf("  FAIL: %04ld-%02u placed %d days, has %u\n",
					(long)y, mo, expect - 1, m.days);
				checks++; fails++; bad++;
			}

		}
	}

	checks++;
	if (bad == 0) printf("  (all cells 1970-2105 consistent)\n");

}

// -- week-start rotation -----------------------------------------

static void test_week_start(void) {

	int col;
	int seen[7];

	printf("week start\n");

	memset(seen, 0, sizeof(seen));

	// Column 0 is CAL_WEEK_START, and the seven columns are a
	// permutation of the seven weekdays -- which is the property
	// that survives changing CAL_WEEK_START, unlike "column 0 is
	// Sunday".
	eq_int(cal_col_wday(0), CAL_WEEK_START, "column 0 is the week start");

	for (col = 0; col < CAL_COLS; col++) {
		uint8_t w = cal_col_wday(col);
		ok(w < 7, "column maps to a real weekday");
		seen[w]++;
		eq_int(cal_wday_col(w), col, "wday_col inverts col_wday");
	}

	for (col = 0; col < 7; col++)
		eq_int(seen[col], 1, "each weekday appears in exactly one column");

	// With the shipped default this is Sunday-first, and saying so
	// here means changing the constant fails loudly rather than
	// quietly reshaping every calendar in the tree.
	eq_int(CAL_WEEK_START, 0, "default week start is Sunday");
	eq_str(z_wday_name(cal_col_wday(0)), "Sun", "column 0 is Sunday");
	eq_str(z_wday_name(cal_col_wday(6)), "Sat", "column 6 is Saturday");

}

// -- navigation --------------------------------------------------

static void test_nav(void) {

	int32_t y;
	uint8_t m;
	int i;

	printf("navigation\n");

	// Forward across a year boundary.
	y = 2026; m = 12;
	ok(cal_step_month(&y, &m, 1), "Dec -> Jan steps");
	eq_int(y, 2027, "  year advanced");
	eq_int(m, 1, "  month wrapped to January");

	// Backward across one.
	ok(cal_step_month(&y, &m, -1), "Jan -> Dec steps");
	eq_int(y, 2026, "  year went back");
	eq_int(m, 12, "  month wrapped to December");

	// Twelve forward steps is one year, exactly.
	y = 1999; m = 3;
	for (i = 0; i < 12; i++) ok(cal_step_month(&y, &m, 1), "step within range");
	eq_int(y, 2000, "twelve steps advance one year");
	eq_int(m, 3, "twelve steps keep the month");

	// -- the ends --
	//
	// A refused step must change NOTHING. A partial update here
	// would leave the app showing a month it cannot resolve.

	y = CAL_YEAR_MIN; m = 1;
	ok(!cal_step_month(&y, &m, -1), "cannot step before Jan 1970");
	eq_int(y, CAL_YEAR_MIN, "  year unchanged after refusal");
	eq_int(m, 1, "  month unchanged after refusal");

	y = CAL_YEAR_MAX; m = 12;
	ok(!cal_step_month(&y, &m, 1), "cannot step past Dec 2105");
	eq_int(y, CAL_YEAR_MAX, "  year unchanged after refusal");
	eq_int(m, 12, "  month unchanged after refusal");

	// Year steps refuse at the same edges.
	y = CAL_YEAR_MIN; m = 6;
	ok(!cal_step_year(&y, &m, -1), "cannot step a year before the range");
	eq_int(y, CAL_YEAR_MIN, "  year unchanged");
	ok(cal_step_year(&y, &m, 1), "can step a year forward from the floor");
	eq_int(y, CAL_YEAR_MIN + 1, "  year advanced");

	y = CAL_YEAR_MAX; m = 6;
	ok(!cal_step_year(&y, &m, 1), "cannot step a year past the range");
	eq_int(y, CAL_YEAR_MAX, "  year unchanged");

	// Every position the range admits must actually resolve --
	// this is what would catch CAL_YEAR_MAX being set one year too
	// generous for the uint32 second count.
	for (y = CAL_YEAR_MIN; y <= CAL_YEAR_MAX; y++) {
		cal_month_t mm;
		for (m = 1; m <= 12; m++) {
			if (!cal_month_info(y, m, &mm) || mm.days < 28 || mm.days > 31) {
				printf("  FAIL: %04ld-%02u is in range but does not "
					"resolve sanely\n", (long)y, m);
				checks++; fails++;
				return;
			}
		}
	}
	checks++;
	printf("  (every month in range resolves)\n");

}

// -- the range edge itself ---------------------------------------
//
// CAL_YEAR_MAX exists because uint32 seconds run out in February
// 2106. The month after the last displayable one must be the thing
// that overflows -- if it isn't, the constant is wrong in one
// direction or the other.

static void test_range_edge(void) {

	z_tm_t a, b;
	uint32_t ta, tb;

	printf("range edge\n");

	// 2106-01-01 is representable, which is what makes Dec 2105
	// measurable.
	memset(&a, 0, sizeof(a));
	a.year = 2106; a.month = 1; a.day = 1;
	ta = z_tm_to_time(&a);
	z_time_to_tm(ta, &b);
	eq_int(b.year, 2106, "2106-01-01 round trips");
	eq_int(b.month, 1, "  month");
	eq_int(b.day, 1, "  day");

	// 2106-03-01 is not -- it is past the uint32 horizon, and comes
	// back as something else entirely. This is the assertion that
	// justifies CAL_YEAR_MAX.
	memset(&a, 0, sizeof(a));
	a.year = 2106; a.month = 3; a.day = 1;
	tb = z_tm_to_time(&a);
	z_time_to_tm(tb, &b);
	ok(b.year != 2106 || b.month != 3,
		"2106-03-01 does NOT round trip (uint32 horizon)");

	ok(!cal_pos_valid(CAL_YEAR_MAX + 1, 1), "year past max rejected");
	ok(!cal_pos_valid(CAL_YEAR_MIN - 1, 12), "year before min rejected");
	ok(!cal_pos_valid(2026, 0), "month 0 rejected");
	ok(!cal_pos_valid(2026, 13), "month 13 rejected");
	ok(cal_pos_valid(CAL_YEAR_MIN, 1), "floor accepted");
	ok(cal_pos_valid(CAL_YEAR_MAX, 12), "ceiling accepted");

}

// -- from a timestamp --------------------------------------------

static void test_from_time(void) {

	int32_t y;
	uint8_t m, d, w;
	z_tm_t tm;

	printf("from timestamp\n");

	// 2026-09-06, the day this app was written.
	memset(&tm, 0, sizeof(tm));
	tm.year = 2026; tm.month = 9; tm.day = 6;
	tm.hour = 13; tm.min = 45; tm.sec = 2;

	ok(cal_from_time(z_tm_to_time(&tm), &y, &m, &d, &w), "resolves a timestamp");
	eq_int(y, 2026, "  year");
	eq_int(m, 9, "  month");
	eq_int(d, 6, "  day");
	eq_str(z_wday_name(w), "Sun", "  weekday (2026-09-06 was a Sunday)");

	// Time of day must not affect the date -- a clock read one
	// second before midnight and one second after must give
	// different days, and nothing in between may drift.
	memset(&tm, 0, sizeof(tm));
	tm.year = 2026; tm.month = 9; tm.day = 6;
	tm.hour = 23; tm.min = 59; tm.sec = 59;
	ok(cal_from_time(z_tm_to_time(&tm), &y, &m, &d, &w), "resolves 23:59:59");
	eq_int(d, 6, "  still the 6th one second before midnight");

	ok(cal_from_time(z_tm_to_time(&tm) + 1, &y, &m, &d, &w), "resolves midnight");
	eq_int(d, 7, "  the 7th one second later");

	// Timestamp 0 is the epoch, which is exactly what an unset RTC
	// reads as -- the app leans on this.
	ok(cal_from_time(0, &y, &m, &d, &w), "resolves the epoch");
	eq_int(y, CAL_EPOCH_YEAR, "  epoch year");
	eq_int(m, CAL_EPOCH_MONTH, "  epoch month");
	eq_int(d, 1, "  epoch day");

}

// -- year entry --------------------------------------------------

static void test_entry(void) {

	cal_entry_t e;
	int32_t y;

	printf("year entry\n");

	cal_entry_reset(&e);
	ok(!e.active, "starts inactive");

	ok(cal_entry_digit(&e, '2'), "accepts a digit");
	ok(e.active, "  becomes active");
	ok(cal_entry_digit(&e, '0'), "accepts a second");
	ok(cal_entry_digit(&e, '2'), "accepts a third");

	// Not four digits yet -- a commit here must be refused rather
	// than guessing at 0202 or padding to 2020.
	y = 1234;
	ok(!cal_entry_commit(&e, &y), "refuses a three-digit year");
	eq_int(y, 1234, "  target untouched");
	ok(e.active, "  entry still live after a refused commit");

	ok(cal_entry_digit(&e, '6'), "accepts the fourth");
	eq_str(e.buf, "2026", "  buffer");

	// Full: further digits are rejected rather than shifting the
	// buffer, so a fat-fingered fifth key cannot turn 2026 into 026x.
	ok(!cal_entry_digit(&e, '7'), "rejects a fifth digit");
	eq_str(e.buf, "2026", "  buffer unchanged");

	ok(cal_entry_commit(&e, &y), "commits a complete year");
	eq_int(y, 2026, "  committed value");
	ok(!e.active, "  entry cleared after commit");
	eq_int(e.len, 0, "  buffer emptied");

	// Non-digits bounce off without side effects, so the app can
	// feed this every unhandled key.
	cal_entry_reset(&e);
	ok(!cal_entry_digit(&e, 'x'), "rejects a letter");
	ok(!cal_entry_digit(&e, ' '), "rejects a space");
	ok(!e.active, "  and does not start entry");

	// Backspace, including all the way out.
	cal_entry_reset(&e);
	ok(!cal_entry_back(&e), "backspace on empty does nothing");
	cal_entry_digit(&e, '1'); cal_entry_digit(&e, '9');
	ok(cal_entry_back(&e), "backspace removes a digit");
	eq_str(e.buf, "1", "  buffer");
	ok(e.active, "  still active with one digit left");
	ok(cal_entry_back(&e), "backspace removes the last");
	ok(!e.active, "  emptying cancels entry");

	// Out-of-range years are refused at commit, not at typing --
	// you have to be able to type 1969's first digit to reach 1970.
	cal_entry_reset(&e);
	cal_entry_digit(&e, '1'); cal_entry_digit(&e, '9');
	cal_entry_digit(&e, '6'); cal_entry_digit(&e, '9');
	y = 2026;
	ok(!cal_entry_commit(&e, &y), "refuses 1969");
	eq_int(y, 2026, "  target untouched");

	cal_entry_reset(&e);
	cal_entry_digit(&e, '2'); cal_entry_digit(&e, '2');
	cal_entry_digit(&e, '0'); cal_entry_digit(&e, '0');
	ok(!cal_entry_commit(&e, &y), "refuses 2200");

	// Both ends of the range are typable.
	cal_entry_reset(&e);
	cal_entry_digit(&e, '1'); cal_entry_digit(&e, '9');
	cal_entry_digit(&e, '7'); cal_entry_digit(&e, '0');
	ok(cal_entry_commit(&e, &y), "accepts 1970");
	eq_int(y, 1970, "  committed");

	cal_entry_reset(&e);
	cal_entry_digit(&e, '2'); cal_entry_digit(&e, '1');
	cal_entry_digit(&e, '0'); cal_entry_digit(&e, '5');
	ok(cal_entry_commit(&e, &y), "accepts 2105");
	eq_int(y, 2105, "  committed");

}

// -- month names -------------------------------------------------

static void test_names(void) {

	printf("month names\n");

	eq_str(z_month_name_long(1), "January", "January");
	eq_str(z_month_name_long(6), "June", "June");
	eq_str(z_month_name_long(9), "September", "September");
	eq_str(z_month_name_long(12), "December", "December");
	eq_str(z_month_name_long(0), "???", "month 0");
	eq_str(z_month_name_long(13), "???", "month 13");

	// The long name must never be shorter than the short one, which
	// is what the header's width budget assumes.
	for (uint8_t m = 1; m <= 12; m++) {
		checks++;
		if (strlen(z_month_name_long(m)) < strlen(z_month_name(m))) {
			fails++;
			printf("  FAIL: long name for month %u is shorter than short\n", m);
		}
	}

	// The widest header the app has to fit. If a longer name ever
	// appears this assertion is the thing that says so, rather than
	// a clipped word on screen.
	{
		size_t widest = 0;
		for (uint8_t m = 1; m <= 12; m++) {
			size_t l = strlen(z_month_name_long(m));
			if (l > widest) widest = l;
		}
		eq_int((long)widest, 9, "widest month name is 9 chars (September)");
	}

}

int main(void) {

	printf("cal_core tests\n\n");

	test_anchors();
	test_six_rows();
	test_grid();
	test_week_start();
	test_nav();
	test_range_edge();
	test_from_time();
	test_entry();
	test_names();

	printf("\n%d checks, %d failures\n", checks, fails);

	return fails ? 1 : 0;

}
