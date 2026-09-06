#ifndef CAL_CORE_H
#define CAL_CORE_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Month geometry and year entry for the cal app -- no drawing, no
 * windows, no messages, nothing that needs hardware.
 *
 * Separate from cal.c precisely so it can be compiled and tested on a
 * host (see tests/cal_test.c), the same split sw/apps/calc already
 * makes for its arithmetic. A calendar that puts the 1st in the wrong
 * column is wrong in a way nobody checks either: you read the shape,
 * not the maths, which is the entire point of a grid.
 *
 * -- the date arithmetic is NOT here --
 *
 * There is no leap-year rule in this file and no table of month
 * lengths. Both already exist, correct and tested, in
 * sw/common/zrtc.c, and a second copy is a second thing to be wrong
 * in February 2100.
 *
 * Everything below is derived from the two public functions there:
 *
 *   first weekday  = z_tm_to_time(1st of the month) -> z_time_to_tm()
 *   length         = (1st of next month - 1st of this month) / 86400
 *
 * That second one is worth pausing on, because it looks like a trick
 * and is actually the reason this file has no leap-year logic: the
 * difference between two consecutive firsts IS the month's length, by
 * definition, for every month including February in a century year.
 * zrtc.c already knows the Gregorian rules; this asks it rather than
 * repeating it.
 *
 * zrtc.c compiles on a host unmodified -- it includes stdint and its
 * own header, and the MMIO accessors it drags in are static inlines
 * nothing here calls. So the test links the real thing, not a copy.
 *
 * -- the year range, and why it stops at 2105 --
 *
 * Unix time in this system is uint32 seconds, which runs out on
 * 2106-02-07. That is not a round number of months, so the last month
 * this file will show is December 2105 -- one whose FOLLOWING first
 * (2106-01-01) is still representable, which the length calculation
 * above needs.
 *
 * Stopping a year early is deliberate. The alternative is a final
 * year that works for January and silently produces a 28-day March
 * for February, which is exactly the kind of edge that ships.
 */

#include <stdint.h>
#include <stdbool.h>

// -- the grid --

#define CAL_COLS  7

// Six, always. A 31-day month starting on the last day of the week
// spans six rows (6 lead cells + 31 days = 37 > 35), and reserving
// the space unconditionally is what stops the window changing height
// as you page through the year. Short months leave the last row
// empty rather than shrinking.
#define CAL_ROWS  6

// Which weekday sits in column 0, as a z_tm_t.wday (0 = Sunday).
//
// Sunday, matching z_wday_name()'s numbering and the rest of the
// system. Everything below derives the column from this rather than
// assuming it, so a Monday-first calendar is this one line -- see
// docs/cal_app.md.
#define CAL_WEEK_START  0

// -- the year range -- see this file's header on the 2105 --

#define CAL_YEAR_MIN  1970
#define CAL_YEAR_MAX  2105

// Where the app opens when the RTC has never been set: the epoch
// itself. Not a guess dressed up as a date -- 1970 is visibly not
// now, which is the honest thing to show when the machine genuinely
// does not know. Year entry (below) is how you leave.
#define CAL_EPOCH_YEAR   1970
#define CAL_EPOCH_MONTH  1

typedef struct {

	int32_t		year;
	uint8_t		month;		// 1-12

	// weekday of the 1st, 0 = Sunday -- as z_tm_t.wday reports it,
	// BEFORE any week-start rotation.
	uint8_t		first_wday;

	// 28-31
	uint8_t		days;

	// blank cells before the 1st, 0-6. This is first_wday rotated
	// by CAL_WEEK_START, and it is the number the grid actually
	// wants; first_wday is kept alongside it because they are equal
	// only in the Sunday-first case and confusing them is the whole
	// bug this struct exists to prevent.
	uint8_t		lead;

	// week rows actually occupied, 4-6. The grid still allocates
	// CAL_ROWS; this is for anything that wants to know how much of
	// it is live.
	uint8_t		rows;

} cal_month_t;

// Fills `out` for the given month. Returns false, leaving `out`
// untouched, if (year, month) is outside the range above -- callers
// that have already gone through cal_step_*() cannot hit that, but a
// year typed by a user can.
bool cal_month_info(int32_t year, uint8_t month, cal_month_t *out);

// The day number at (row, col) of the grid, or 0 for a cell outside
// the month. Rows and columns are 0-based; col 0 is CAL_WEEK_START.
int cal_cell_day(const cal_month_t *m, int row, int col);

// The (row, col) holding `day`, for placing the today highlight
// without searching the grid for it. Returns false if `day` is not in
// the month.
bool cal_cell_of_day(const cal_month_t *m, int day, int *row, int *col);

// -- navigation --
//
// All four take the current position by pointer and move it in place,
// returning false and changing NOTHING if the move would leave the
// range. A false return is what greys out the arrow: the caller does
// not have to know where the ends are.

bool cal_step_month(int32_t *year, uint8_t *month, int delta);
bool cal_step_year(int32_t *year, uint8_t *month, int delta);

// True if the position is inside the range at all.
bool cal_pos_valid(int32_t year, uint8_t month);

// Breaks a UTC timestamp into a position plus its day of the month
// and weekday, for "which month is today in". Returns false if the
// timestamp falls outside the displayable range -- which for a clock
// that has been set by NTP cannot happen, and for one that has not is
// the caller's cue to fall back to the epoch.
//
// Any output pointer may be NULL. `wday` is 0 = Sunday, as
// z_wday_name() takes it -- NOT a column, since a caller wanting the
// name should not have to know about CAL_WEEK_START to get it.
bool cal_from_time(uint32_t t, int32_t *year, uint8_t *month, uint8_t *day,
	uint8_t *wday);

// The column, 0-6, that weekday `wday` (0 = Sunday) occupies. Used
// for the weekday header row so it rotates with CAL_WEEK_START rather
// than being written out in one fixed order.
int cal_wday_col(uint8_t wday);

// The weekday (0 = Sunday) shown in column `col`. The inverse of the
// above, for labelling the header.
uint8_t cal_col_wday(int col);

// -- year entry --
//
// Typing a year is the only way out of 1970 that isn't 400 presses of
// the right arrow, so it is not a convenience -- on a machine with no
// network it is the app's primary navigation.
//
// Kept here rather than in cal.c because entry state is where this
// kind of thing goes wrong: a digit arriving after the buffer is
// full, a commit on a half-typed year, a backspace past the start.
// Those are all testable and none of them are visible in a
// screenshot.

typedef struct {
	bool	active;			// is a year being typed right now
	char	buf[5];			// up to 4 digits, NUL-terminated
	int		len;
} cal_entry_t;

// Clears the buffer and leaves entry inactive.
void cal_entry_reset(cal_entry_t *e);

// Feeds one character. Accepts '0'-'9' up to 4 digits, starting entry
// if it was not already active. Returns true if the buffer changed,
// so the caller knows whether to repaint.
//
// Anything else is rejected without side effects -- deliberately, so
// the app can hand this every keypress it doesn't otherwise use and
// let the buffer decide what counts as a digit.
bool cal_entry_digit(cal_entry_t *e, char c);

// Removes the last digit. Deactivates entry when the buffer empties,
// so backspacing all the way out is the same as cancelling. Returns
// true if anything changed.
bool cal_entry_back(cal_entry_t *e);

// Parses the buffer and, if it is a complete and in-range year,
// writes it to *year and resets the entry. Returns false and leaves
// both alone otherwise -- a year of fewer than 4 digits is
// deliberately NOT padded or guessed at, since "26" could reasonably
// mean 2026 or 1926 and picking one silently would be worse than
// asking for the other two digits.
bool cal_entry_commit(cal_entry_t *e, int32_t *year);

#endif
