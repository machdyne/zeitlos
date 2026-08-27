/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Calendar arithmetic: Unix seconds <-> year/month/day/hour/min/sec.
 * See zrtc.h for the API and the register side; this file is only the
 * date maths.
 *
 * -- why not <time.h> --
 *
 * newlib has gmtime() and mktime() and they would work. They also drag
 * in newlib's timezone machinery, its own struct tm, and a chain of
 * locale-adjacent code that costs several KB in a binary where main
 * memory is a 1MB budget shared between every running process
 * (sw/os/mem.c). This is two functions and a 12-entry table, and it
 * has no dependency on anything outside stdint.
 *
 * The other reason is that mktime() interprets its input as LOCAL
 * time, using a timezone this system has no notion of, so it is not
 * actually the inverse of what is wanted here anyway.
 *
 * -- the algorithm --
 *
 * days_from_civil / civil_from_days, from Howard Hinnant's public
 * domain chrono-compatible date algorithms. The trick in both is to
 * shift the year so it starts in March: leap day then falls at the END
 * of a year rather than in the middle of one, which removes every
 * special case from the month-length arithmetic. That is why the code
 * subtracts 3 from the month, works in 400-year "eras", and adds the
 * 719468-day offset at the end to move the epoch from 0000-03-01 back
 * to 1970-01-01.
 *
 * It is proleptic Gregorian and knows nothing about leap seconds --
 * neither does the RTC, neither does the NTP timestamp it is set
 * from, and neither does Unix time itself, so all three agree.
 *
 * Working range is 1970 through 2106, the span of a uint32 second
 * count. Nothing here handles a year before 1970; the era arithmetic
 * would cope, but a negative Unix time cannot be expressed in the
 * unsigned type this system uses everywhere for timestamps, so the
 * question never arises.
 */

#include <stdint.h>

#include "zrtc.h"

// Days from 1970-01-01 to the given civil date. Internal -- the public
// direction is z_tm_to_time() below.
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d) {

	// March-based year: January and February belong to the PREVIOUS
	// one, which is what puts the leap day last.
	y -= (m <= 2) ? 1 : 0;

	int32_t era = (y >= 0 ? y : y - 399) / 400;
	uint32_t yoe = (uint32_t)(y - era * 400);			// [0, 399]

	// day of the March-based year, [0, 365]
	uint32_t mp = (m > 2) ? (m - 3) : (m + 9);
	uint32_t doy = (153 * mp + 2) / 5 + d - 1;

	// day of the 400-year era, [0, 146096]
	uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return era * 146097 + (int32_t)doe - 719468;

}

void z_time_to_tm(uint32_t t, z_tm_t *tm) {

	if (!tm) return;

	uint32_t days = t / Z_SECS_PER_DAY;
	uint32_t sod = t % Z_SECS_PER_DAY;

	tm->hour = (uint8_t)(sod / Z_SECS_PER_HOUR);
	tm->min = (uint8_t)((sod % Z_SECS_PER_HOUR) / Z_SECS_PER_MIN);
	tm->sec = (uint8_t)(sod % Z_SECS_PER_MIN);

	// 1970-01-01 was a Thursday, hence the +4 before reducing mod 7
	// with Sunday as 0.
	tm->wday = (uint8_t)((days + 4) % 7);

	// -- civil_from_days --

	int32_t z = (int32_t)days + 719468;
	int32_t era = (z >= 0 ? z : z - 146096) / 146097;
	uint32_t doe = (uint32_t)(z - era * 146097);		// [0, 146096]

	// year of era, [0, 399]. The three correction terms subtract the
	// leap days already accounted for at the 4/100/400-year periods.
	uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;

	int32_t y = (int32_t)yoe + era * 400;
	uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);	// [0, 365]
	uint32_t mp = (5 * doy + 2) / 153;					// [0, 11], March = 0

	tm->day = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
	uint32_t m = (mp < 10) ? (mp + 3) : (mp - 9);
	tm->month = (uint8_t)m;

	// undo the March shift
	tm->year = y + ((m <= 2) ? 1 : 0);

	// yday wants the ordinary January-based year, so it is easier to
	// derive from the finished date than to carry through the shifted
	// arithmetic above.
	tm->yday = (uint16_t)((int32_t)days -
		days_from_civil(tm->year, 1, 1));

}

uint32_t z_tm_to_time(const z_tm_t *tm) {

	if (!tm) return 0;

	int32_t days = days_from_civil(tm->year, tm->month, tm->day);
	if (days < 0) return 0;		// before the epoch -- see this file's header

	return (uint32_t)days * Z_SECS_PER_DAY +
		(uint32_t)tm->hour * Z_SECS_PER_HOUR +
		(uint32_t)tm->min * Z_SECS_PER_MIN +
		(uint32_t)tm->sec;

}

static const char *const wday_names[7] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *const month_names[12] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

const char *z_wday_name(uint8_t wday) {
	if (wday > 6) return "???";
	return wday_names[wday];
}

const char *z_month_name(uint8_t month) {
	if (month < 1 || month > 12) return "???";
	return month_names[month - 1];
}
