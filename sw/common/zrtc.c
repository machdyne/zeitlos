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
#include <stdbool.h>
#include <stddef.h>

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

// Spelled out, for a caller with room for it -- see zrtc.h on why
// this is a second table rather than the short names with a suffix
// rule. There isn't one: "Sep" -> "September" but "Jun" -> "June",
// and a rule with two exceptions is longer than the table.
static const char *const month_names_long[12] = {
	"January", "February", "March", "April", "May", "June",
	"July", "August", "September", "October", "November", "December"
};

const char *z_month_name_long(uint8_t month) {
	if (month < 1 || month > 12) return "???";
	return month_names_long[month - 1];
}

// -- time zones -- see zrtc.h --

#define H(h)      ((int16_t)((h) * 60))
#define HM(h, m)  ((int16_t)((h) * 60 + ((h) < 0 ? -(m) : (m))))

// Sorted by name -- sw/apps/settings shows it in this order, and
// sw/common/tests/test_zcfg.c checks that it stays sorted. Standard
// offsets and rules as of 2026.
const z_tz_city_t z_tz_cities[] = {
	{ "Adelaide",     HM(9, 30), Z_DST_AU,   "ACST", "ACDT" },
	{ "Amsterdam",    H(1),      Z_DST_EU,   "CET",  "CEST" },
	{ "Anchorage",    H(-9),     Z_DST_US,   "AKST", "AKDT" },
	{ "Athens",       H(2),      Z_DST_EU,   "EET",  "EEST" },
	{ "Auckland",     H(12),     Z_DST_NZ,   "NZST", "NZDT" },
	{ "Bangkok",      H(7),      Z_DST_NONE, NULL,   NULL },
	{ "Beijing",      H(8),      Z_DST_NONE, NULL,   NULL },
	{ "Berlin",       H(1),      Z_DST_EU,   "CET",  "CEST" },
	{ "Brisbane",     H(10),     Z_DST_NONE, "AEST", NULL },
	{ "Brussels",     H(1),      Z_DST_EU,   "CET",  "CEST" },
	{ "Bucharest",    H(2),      Z_DST_EU,   "EET",  "EEST" },
	{ "Buenos Aires", H(-3),     Z_DST_NONE, NULL,   NULL },
	{ "Chicago",      H(-6),     Z_DST_US,   "CST",  "CDT" },
	{ "Copenhagen",   H(1),      Z_DST_EU,   "CET",  "CEST" },
	{ "Denver",       H(-7),     Z_DST_US,   "MST",  "MDT" },
	{ "Dubai",        H(4),      Z_DST_NONE, NULL,   NULL },
	{ "Dublin",       H(0),      Z_DST_EU,   "GMT",  "IST" },
	{ "Halifax",      H(-4),     Z_DST_US,   "AST",  "ADT" },
	{ "Helsinki",     H(2),      Z_DST_EU,   "EET",  "EEST" },
	{ "Hong Kong",    H(8),      Z_DST_NONE, "HKT",  NULL },
	{ "Honolulu",     H(-10),    Z_DST_NONE, "HST",  NULL },
	{ "Istanbul",     H(3),      Z_DST_NONE, NULL,   NULL },
	{ "Jakarta",      H(7),      Z_DST_NONE, "WIB",  NULL },
	{ "Johannesburg", H(2),      Z_DST_NONE, "SAST", NULL },
	{ "Karachi",      H(5),      Z_DST_NONE, "PKT",  NULL },
	{ "Kathmandu",    HM(5, 45), Z_DST_NONE, NULL,   NULL },
	{ "Kolkata",      HM(5, 30), Z_DST_NONE, "IST",  NULL },
	{ "Kyiv",         H(2),      Z_DST_EU,   "EET",  "EEST" },
	{ "Lagos",        H(1),      Z_DST_NONE, "WAT",  NULL },
	{ "Lima",         H(-5),     Z_DST_NONE, NULL,   NULL },
	{ "Lisbon",       H(0),      Z_DST_EU,   "WET",  "WEST" },
	{ "London",       H(0),      Z_DST_EU,   "GMT",  "BST" },
	{ "Los Angeles",  H(-8),     Z_DST_US,   "PST",  "PDT" },
	{ "Madrid",       H(1),      Z_DST_EU,   "CET",  "CEST" },
	{ "Manila",       H(8),      Z_DST_NONE, NULL,   NULL },
	{ "Melbourne",    H(10),     Z_DST_AU,   "AEST", "AEDT" },
	{ "Mexico City",  H(-6),     Z_DST_NONE, "CST",  NULL },
	{ "Moscow",       H(3),      Z_DST_NONE, "MSK",  NULL },
	{ "Mumbai",       HM(5, 30), Z_DST_NONE, "IST",  NULL },
	{ "Munich",       H(1),      Z_DST_EU,   "CET",  "CEST" },
	{ "Nairobi",      H(3),      Z_DST_NONE, "EAT",  NULL },
	{ "New York",     H(-5),     Z_DST_US,   "EST",  "EDT" },
	{ "Oslo",         H(1),      Z_DST_EU,   "CET",  "CEST" },
	{ "Paris",        H(1),      Z_DST_EU,   "CET",  "CEST" },
	{ "Perth",        H(8),      Z_DST_NONE, "AWST", NULL },
	{ "Phoenix",      H(-7),     Z_DST_NONE, "MST",  NULL },
	{ "Prague",       H(1),      Z_DST_EU,   "CET",  "CEST" },
	{ "Reykjavik",    H(0),      Z_DST_NONE, "GMT",  NULL },
	{ "Rome",         H(1),      Z_DST_EU,   "CET",  "CEST" },
	{ "Sao Paulo",    H(-3),     Z_DST_NONE, NULL,   NULL },
	{ "Seoul",        H(9),      Z_DST_NONE, "KST",  NULL },
	{ "Shanghai",     H(8),      Z_DST_NONE, NULL,   NULL },
	{ "Singapore",    H(8),      Z_DST_NONE, NULL,   NULL },
	{ "Stockholm",    H(1),      Z_DST_EU,   "CET",  "CEST" },
	{ "Sydney",       H(10),     Z_DST_AU,   "AEST", "AEDT" },
	{ "Taipei",       H(8),      Z_DST_NONE, NULL,   NULL },
	{ "Tehran",       HM(3, 30), Z_DST_NONE, NULL,   NULL },
	{ "Tokyo",        H(9),      Z_DST_NONE, "JST",  NULL },
	{ "Toronto",      H(-5),     Z_DST_US,   "EST",  "EDT" },
	{ "Vancouver",    H(-8),     Z_DST_US,   "PST",  "PDT" },
	{ "Vienna",       H(1),      Z_DST_EU,   "CET",  "CEST" },
	{ "Warsaw",       H(1),      Z_DST_EU,   "CET",  "CEST" },
	{ "Zurich",       H(1),      Z_DST_EU,   "CET",  "CEST" },
};

const int z_tz_city_count = (int)(sizeof(z_tz_cities) / sizeof(z_tz_cities[0]));

static char tz_lower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool tz_same(const char *a, const char *b) {
	while (*a && *b && tz_lower(*a) == tz_lower(*b)) { a++; b++; }
	return *a == 0 && *b == 0;
}

static void tz_copy(char *dst, const char *src) {
	int n = 0;
	for (; src && src[n] && n < Z_TZ_NAME_MAX - 1; n++) dst[n] = src[n];
	dst[n] = 0;
}

void z_tz_format_offset(int32_t minutes, char *out, int outlen) {

	char buf[Z_TZ_NAME_MAX];
	int n = 0;
	uint32_t a = (uint32_t)(minutes < 0 ? -minutes : minutes);
	uint32_t h = a / 60, m = a % 60;

	buf[n++] = 'U'; buf[n++] = 'T'; buf[n++] = 'C';

	if (minutes != 0) {
		buf[n++] = minutes < 0 ? '-' : '+';
		if (h >= 10) buf[n++] = (char)('0' + h / 10);
		buf[n++] = (char)('0' + h % 10);
		if (m) {
			buf[n++] = ':';
			buf[n++] = (char)('0' + m / 10);
			buf[n++] = (char)('0' + m % 10);
		}
	}
	buf[n] = 0;

	for (n = 0; buf[n] && n < outlen - 1; n++) out[n] = buf[n];
	if (outlen > 0) out[n] = 0;

}

static void tz_set_fixed(z_tz_t *tz, int32_t minutes) {
	tz->std_off = minutes * 60;
	tz->dst = Z_DST_NONE;
	tz->city = NULL;
	z_tz_format_offset(minutes, tz->std_name, Z_TZ_NAME_MAX);
	tz->dst_name[0] = 0;
}

// "+2", "-5:30", "+05:30" -> minutes. Up to +-14 hours.
static bool tz_offset(const char *p, int32_t *minutes) {

	int sign, h = 0, m = 0, digits = 0;

	if (*p != '+' && *p != '-') return false;
	sign = (*p++ == '-') ? -1 : 1;

	while (*p >= '0' && *p <= '9' && digits < 2) { h = h * 10 + (*p++ - '0'); digits++; }
	if (!digits) return false;

	if (*p == ':') {
		p++;
		if (p[0] < '0' || p[0] > '5' || p[1] < '0' || p[1] > '9') return false;
		m = (p[0] - '0') * 10 + (p[1] - '0');
		p += 2;
	}

	if (*p || h > 14 || (h == 14 && m)) return false;

	*minutes = sign * (h * 60 + m);
	return true;

}

bool z_tz_parse(const char *s, z_tz_t *tz) {

	int32_t minutes;

	if (!tz) return false;
	tz_set_fixed(tz, 0);
	if (!s) return false;

	while (*s == ' ') s++;

	if ((tz_lower(s[0]) == 'u' && tz_lower(s[1]) == 't' && tz_lower(s[2]) == 'c')) {
		if (s[3] == 0) return true;
		if (tz_offset(s + 3, &minutes)) { tz_set_fixed(tz, minutes); return true; }
		return false;
	}

	for (int i = 0; i < z_tz_city_count; i++) {
		const z_tz_city_t *c = &z_tz_cities[i];
		if (!tz_same(s, c->city)) continue;

		tz->std_off = (int32_t)c->std_min * 60;
		tz->dst = c->dst;
		tz->city = c;
		if (c->std_abbr) tz_copy(tz->std_name, c->std_abbr);
		else z_tz_format_offset(c->std_min, tz->std_name, Z_TZ_NAME_MAX);
		if (c->dst_abbr) tz_copy(tz->dst_name, c->dst_abbr);
		else z_tz_format_offset(c->std_min + 60, tz->dst_name, Z_TZ_NAME_MAX);
		return true;
	}

	return false;

}

// Midnight UTC of the `week`th (5 = last) `wday` of `mon` in `year`, as
// seconds since the epoch.
static int64_t tz_nth_sunday(int32_t year, uint32_t mon, int week) {

	int32_t first = days_from_civil(year, mon, 1);
	int32_t next = (mon == 12) ? days_from_civil(year + 1, 1, 1)
		: days_from_civil(year, mon + 1, 1);
	int32_t dow = (int32_t)(((first % 7) + 7 + 4) % 7);	// 1970-01-01 was a Thursday

	int32_t d = first + (7 - dow) % 7 + (week - 1) * 7;
	while (d >= next) d -= 7;		// week 5 means "last"

	return (int64_t)d * Z_SECS_PER_DAY;

}

uint32_t z_tz_local(const z_tz_t *tz, uint32_t utc, bool *is_dst) {

	bool dst = false;
	int32_t off;

	if (!tz) { if (is_dst) *is_dst = false; return utc; }

	off = tz->std_off;

	if (tz->dst != Z_DST_NONE) {

		z_tm_t tm;
		int64_t std_local = (int64_t)utc + tz->std_off;
		int64_t start = 0, end = 0;
		int32_t so = tz->std_off, dso = tz->std_off + 3600;

		z_time_to_tm(std_local < 0 ? 0 : (uint32_t)std_local, &tm);

		// Transition instants in UTC. A local-time rule is given in
		// the time in force just before the change: standard time for
		// the start, daylight time for the end.
		switch (tz->dst) {
		case Z_DST_EU:
			start = tz_nth_sunday(tm.year, 3, 5) + 3600;
			end = tz_nth_sunday(tm.year, 10, 5) + 3600;
			break;
		case Z_DST_US:
			start = tz_nth_sunday(tm.year, 3, 2) + 2 * 3600 - so;
			end = tz_nth_sunday(tm.year, 11, 1) + 2 * 3600 - dso;
			break;
		case Z_DST_AU:
			start = tz_nth_sunday(tm.year, 10, 1) + 2 * 3600 - so;
			end = tz_nth_sunday(tm.year, 4, 1) + 3 * 3600 - dso;
			break;
		case Z_DST_NZ:
			start = tz_nth_sunday(tm.year, 9, 5) + 2 * 3600 - so;
			end = tz_nth_sunday(tm.year, 4, 1) + 3 * 3600 - dso;
			break;
		default:
			break;
		}

		if (start < end)
			dst = (int64_t)utc >= start && (int64_t)utc < end;	// north
		else
			dst = (int64_t)utc >= start || (int64_t)utc < end;	// south

		if (dst) off = dso;

	}

	if (is_dst) *is_dst = dst;

	int64_t local = (int64_t)utc + off;
	return local < 0 ? 0 : (uint32_t)local;

}

const char *z_tz_name(const z_tz_t *tz, bool is_dst) {
	if (!tz) return "UTC";
	return (is_dst && tz->dst != Z_DST_NONE) ? tz->dst_name : tz->std_name;
}
