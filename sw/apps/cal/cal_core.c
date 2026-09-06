/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Month geometry and year entry -- see cal_core.h for the API and,
 * more importantly, for why there is no leap-year rule anywhere in
 * this file.
 */

#include <stdint.h>
#include <stdbool.h>

#include "cal_core.h"

#include "../../common/zrtc.h"

// Seconds at 00:00 UTC on the 1st of (year, month).
//
// Not exported: everything a caller wants is derived from it here,
// and a bare "give me a timestamp" would invite arithmetic on it at
// call sites, which is where the range edge (cal_core.h) gets
// forgotten.
static uint32_t first_of(int32_t year, uint8_t month) {

	z_tm_t tm;

	tm.year = year;
	tm.month = month;
	tm.day = 1;
	tm.hour = 0;
	tm.min = 0;
	tm.sec = 0;
	tm.wday = 0;		// output-only for z_tm_to_time(); see zrtc.h
	tm.yday = 0;

	return z_tm_to_time(&tm);

}

bool cal_pos_valid(int32_t year, uint8_t month) {

	if (month < 1 || month > 12) return false;
	if (year < CAL_YEAR_MIN || year > CAL_YEAR_MAX) return false;

	return true;

}

int cal_wday_col(uint8_t wday) {
	return (int)((wday + 7 - CAL_WEEK_START) % 7);
}

uint8_t cal_col_wday(int col) {
	if (col < 0 || col >= CAL_COLS) return 0;
	return (uint8_t)((col + CAL_WEEK_START) % 7);
}

bool cal_month_info(int32_t year, uint8_t month, cal_month_t *out) {

	uint32_t t0, t1;
	int32_t ny;
	uint8_t nm;
	z_tm_t tm;
	int cells;

	if (!out) return false;
	if (!cal_pos_valid(year, month)) return false;

	// The following month, which the length calculation needs. This
	// is why CAL_YEAR_MAX stops at 2105 -- see cal_core.h.
	ny = year;
	nm = (uint8_t)(month + 1);
	if (nm > 12) { nm = 1; ny++; }

	t0 = first_of(year, month);
	t1 = first_of(ny, nm);

	z_time_to_tm(t0, &tm);

	out->year = year;
	out->month = month;
	out->first_wday = tm.wday;
	out->days = (uint8_t)((t1 - t0) / Z_SECS_PER_DAY);
	out->lead = (uint8_t)cal_wday_col(tm.wday);

	// Rounded up, so a month whose last day lands mid-row still
	// counts that row.
	cells = out->lead + out->days;
	out->rows = (uint8_t)((cells + CAL_COLS - 1) / CAL_COLS);

	return true;

}

int cal_cell_day(const cal_month_t *m, int row, int col) {

	int idx, day;

	if (!m) return 0;
	if (row < 0 || row >= CAL_ROWS) return 0;
	if (col < 0 || col >= CAL_COLS) return 0;

	idx = row * CAL_COLS + col;
	day = idx - (int)m->lead + 1;

	if (day < 1 || day > (int)m->days) return 0;

	return day;

}

bool cal_cell_of_day(const cal_month_t *m, int day, int *row, int *col) {

	int idx;

	if (!m) return false;
	if (day < 1 || day > (int)m->days) return false;

	idx = (int)m->lead + day - 1;

	if (row) *row = idx / CAL_COLS;
	if (col) *col = idx % CAL_COLS;

	return true;

}

bool cal_step_month(int32_t *year, uint8_t *month, int delta) {

	int32_t y;
	int m;

	if (!year || !month) return false;

	y = *year;
	m = (int)*month + delta;

	// A loop rather than a divide, because delta is +-1 at every
	// call site and a divide would need care about C's truncation
	// toward zero for the negative case -- care that would be
	// exercised by nothing.
	while (m > 12) { m -= 12; y++; }
	while (m < 1)  { m += 12; y--; }

	if (!cal_pos_valid(y, (uint8_t)m)) return false;

	*year = y;
	*month = (uint8_t)m;

	return true;

}

bool cal_step_year(int32_t *year, uint8_t *month, int delta) {

	int32_t y;

	if (!year || !month) return false;

	y = *year + delta;

	if (!cal_pos_valid(y, *month)) return false;

	*year = y;

	return true;

}

bool cal_from_time(uint32_t t, int32_t *year, uint8_t *month, uint8_t *day,
	uint8_t *wday) {

	z_tm_t tm;

	z_time_to_tm(t, &tm);

	if (!cal_pos_valid(tm.year, tm.month)) return false;

	if (year) *year = tm.year;
	if (month) *month = tm.month;
	if (day) *day = tm.day;
	if (wday) *wday = tm.wday;

	return true;

}

// -- year entry --

void cal_entry_reset(cal_entry_t *e) {

	if (!e) return;

	e->active = false;
	e->len = 0;
	e->buf[0] = '\0';

}

bool cal_entry_digit(cal_entry_t *e, char c) {

	if (!e) return false;
	if (c < '0' || c > '9') return false;
	if (e->len >= 4) return false;

	e->buf[e->len++] = c;
	e->buf[e->len] = '\0';
	e->active = true;

	return true;

}

bool cal_entry_back(cal_entry_t *e) {

	if (!e) return false;
	if (e->len <= 0) return false;

	e->buf[--e->len] = '\0';

	// Backspacing the last digit out is the same as cancelling.
	// Leaving entry active with an empty buffer would show a
	// year field with nothing in it and no way to tell whether the
	// app was waiting for something.
	if (e->len == 0) e->active = false;

	return true;

}

bool cal_entry_commit(cal_entry_t *e, int32_t *year) {

	int32_t y = 0;
	int i;

	if (!e || !year) return false;

	// Four digits or nothing -- see cal_core.h on why a short year
	// is not guessed at.
	if (e->len != 4) return false;

	for (i = 0; i < 4; i++)
		y = y * 10 + (e->buf[i] - '0');

	if (y < CAL_YEAR_MIN || y > CAL_YEAR_MAX) return false;

	*year = y;
	cal_entry_reset(e);

	return true;

}
