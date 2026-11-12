/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The calendar in sw/common/zrtc.c, 32 and 64 bits (docs/rtc.md,
 * "64-bit time"): known dates both ways, past 2106 and before 1970,
 * and the two agreeing wherever both reach. On the host.
 */

#include <stdio.h>
#include <string.h>
#include "../zrtc.h"

static int checks, fails;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int is(const z_tm_t *t, int y, int mo, int d, int h, int mi, int s, int wd) {
	return t->year == y && t->month == mo && t->day == d && t->hour == h &&
		t->min == mi && t->sec == s && t->wday == wd;
}

int main(void) {

	z_tm_t t;

	z_time_to_tm(0, &t);
	CHECK(is(&t, 1970, 1, 1, 0, 0, 0, 4), "0 is Thursday 1970-01-01");
	z_time_to_tm64(0, &t);
	CHECK(is(&t, 1970, 1, 1, 0, 0, 0, 4) && t.yday == 0, "and in 64 bits");

	z_time_to_tm64(1794355200LL, &t);		// 2026-11-11 00:00:00
	CHECK(is(&t, 2026, 11, 11, 0, 0, 0, 3), "2026-11-11 is a Wednesday (%d-%d-%d w%d)",
		(int)t.year, t.month, t.day, t.wday);

	z_time_to_tm64(4294967295LL, &t);		// the last second 32 bits hold
	CHECK(is(&t, 2106, 2, 7, 6, 28, 15, 0), "2^32-1 is 2106-02-07 06:28:15, a Sunday (%d-%d-%d %d:%d:%d w%d)",
		(int)t.year, t.month, t.day, t.hour, t.min, t.sec, t.wday);
	z_time_to_tm64(4294967296LL, &t);
	CHECK(is(&t, 2106, 2, 7, 6, 28, 16, 0), "and the next one, which 32 bits cannot");

	z_time_to_tm64(-1, &t);
	CHECK(is(&t, 1969, 12, 31, 23, 59, 59, 3) && t.yday == 364, "-1 is 1969-12-31 23:59:59, a Wednesday");

	z_time_to_tm64(253402300799LL, &t);		// 9999-12-31 23:59:59
	CHECK(is(&t, 9999, 12, 31, 23, 59, 59, 5), "9999-12-31 23:59:59 is a Friday (w%d)", t.wday);

	t.year = 2200; t.month = 3; t.day = 1; t.hour = 12; t.min = 0; t.sec = 0;
	int64_t s = z_tm_to_time64(&t);
	z_tm_t back;
	z_time_to_tm64(s, &back);
	CHECK(s == 7263259200LL && back.year == 2200 && back.month == 3 && back.day == 1 && back.hour == 12,
		"2200-03-01 12:00 is 7263259200 and back (%lld)", (long long)s);

	// The 32- and 64-bit conversions agree across the whole 32-bit range
	int agree = 1;
	for (uint64_t x = 0; x <= 0xffffffffull; x += 86399ull * 37 + 11) {
		z_tm_t a, b;
		z_time_to_tm((uint32_t)x, &a);
		z_time_to_tm64((int64_t)x, &b);
		if (memcmp(&a, &b, sizeof a)) { agree = 0; break; }
		if (z_tm_to_time64(&a) != (int64_t)x) { agree = 0; break; }
	}
	CHECK(agree, "32 and 64 bits agree from 1970 to 2106, both ways");

	printf("zrtc: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;

}
