/*
 * Host tests for sw/common/zcfg.c's parser and editor, and
 * sw/common/zrtc.c's time zones.
 *
 *   cc -std=gnu99 -Wall -DZCFG_KERNEL -I sw/common -o /tmp/test_zcfg \
 *      sw/common/tests/test_zcfg.c sw/common/zcfg.c sw/common/zrtc.c
 *   /tmp/test_zcfg
 *
 * -DZCFG_KERNEL leaves out the syscall wrappers, which need a kernel;
 * everything tested here is pure. See docs/config.md.
 *
 * The time-zone cases are real 2026 transitions, checked against
 * published dates rather than against this code's own idea of them: the
 * EU changes on the last Sunday of March and October at 01:00 UTC, the
 * US on the second Sunday of March and first of November at 02:00
 * local, south-east Australia on the first Sundays of October and
 * April, New Zealand on the last Sunday of September and first of
 * April.
 */

#include <stdio.h>
#include <string.h>

#include "zcfg.h"
#include "zrtc.h"

static int run, failed;

#define CHECK(cond, what) do { run++; if (!(cond)) { failed++; \
	printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, what); } } while (0)

static z_cfg_line_t parse(const char *s, char *k, char *v) {
	return z_cfg_parse_line(s, strlen(s), k, Z_CFG_KEY_MAX, v, Z_CFG_VAL_MAX);
}

static void test_parse(void) {

	char k[Z_CFG_KEY_MAX], v[Z_CFG_VAL_MAX];

	CHECK(parse("", k, v) == Z_CFG_LINE_BLANK, "empty is blank");
	CHECK(parse("   \t", k, v) == Z_CFG_LINE_BLANK, "whitespace is blank");
	CHECK(parse("  # comment: x", k, v) == Z_CFG_LINE_BLANK, "comment");

	CHECK(parse("apps.term.auto_connect: port repl0", k, v) == Z_CFG_LINE_ENTRY
		&& !strcmp(k, "apps.term.auto_connect") && !strcmp(v, "port repl0"),
		"colon form");
	CHECK(parse("system.video.mode = amber\r\n", k, v) == Z_CFG_LINE_ENTRY
		&& !strcmp(k, "system.video.mode") && !strcmp(v, "amber"),
		"equals form, CRLF");
	CHECK(parse("system.rtc.timezone UTC", k, v) == Z_CFG_LINE_ENTRY
		&& !strcmp(v, "UTC"), "whitespace form");
	CHECK(parse("\ta.key:value  ", k, v) == Z_CFG_LINE_ENTRY
		&& !strcmp(k, "a.key") && !strcmp(v, "value"), "no spaces, trailing trimmed");
	CHECK(parse("a.b: x # not a comment", k, v) == Z_CFG_LINE_ENTRY
		&& !strcmp(v, "x # not a comment"), "# inside a value is kept");
	CHECK(parse("a.empty:", k, v) == Z_CFG_LINE_ENTRY && !strcmp(v, ""),
		"empty value");
	CHECK(parse("a.x: a: b", k, v) == Z_CFG_LINE_ENTRY && !strcmp(v, "a: b"),
		"only the first separator splits");
	CHECK(parse("remember to set the zone", k, v) == Z_CFG_LINE_BAD,
		"prose is not a setting (keys are dotted)");
	CHECK(parse(".a: 1", k, v) == Z_CFG_LINE_BAD && parse("a.: 1", k, v) == Z_CFG_LINE_BAD,
		"the dot must be inside the key");

	CHECK(parse("foo!bar: 1", k, v) == Z_CFG_LINE_BAD, "bad key char");
	CHECK(parse(": value", k, v) == Z_CFG_LINE_BAD, "no key");

	{
		char big[300];
		memset(big, 'v', sizeof(big));
		memcpy(big, "a.k: ", 5);
		big[sizeof(big) - 1] = 0;
		CHECK(parse(big, k, v) == Z_CFG_LINE_BAD, "overlong value is bad, not truncated");
	}

	{
		// not NUL-terminated within len
		const char raw[] = "a.a: bXXXX";
		CHECK(z_cfg_parse_line(raw, 6, k, sizeof(k), v, sizeof(v)) == Z_CFG_LINE_ENTRY
			&& !strcmp(v, "b"), "len bounds the read");
	}

	CHECK(z_cfg_key_valid("system.rtc.timezone"), "valid key");
	CHECK(!z_cfg_key_valid("has space"), "invalid key");
	CHECK(!z_cfg_key_valid("undotted"), "undotted key");
	CHECK(!strcmp(z_cfg_default("system.rtc.timezone"), "UTC"), "default");
	CHECK(!strcmp(z_cfg_default("no.such.key"), ""), "unknown default is empty");

}

static void test_text_set(void) {

	char out[1024];
	int n;

	const char *file =
		"# Zeitlos\r\n"
		"\r\n"
		"my.unknown.key: keep me\r\n"
		"system.video.mode: white\r\n"
		"this line is not valid!\r\n"
		"system.video.mode = green\r\n"
		"apps.other: 1";		// no final newline

	n = z_cfg_text_set(file, strlen(file), "system.video.mode", "amber", out, sizeof(out));
	CHECK(n > 0, "set existing");
	CHECK(strstr(out, "# Zeitlos\r\n\r\nmy.unknown.key: keep me\r\n") == out,
		"leading lines copied byte for byte");
	CHECK(strstr(out, "system.video.mode: amber\n") != NULL, "first occurrence replaced");
	CHECK(strstr(out, "green") == NULL, "later duplicate dropped");
	CHECK(strstr(out, "this line is not valid!\r\n") != NULL, "malformed line kept");
	CHECK(strstr(out, "apps.other: 1") != NULL && out[n - 1] == '1',
		"last line kept without inventing a newline");
	CHECK((int)strlen(out) == n, "returned length");

	n = z_cfg_text_set(file, strlen(file), "system.rtc.timezone", "UTC+2", out, sizeof(out));
	CHECK(n > 0 && strstr(out, "apps.other: 1\nsystem.rtc.timezone: UTC+2\n") != NULL,
		"append adds a newline to an unterminated last line first");

	n = z_cfg_text_set(file, strlen(file), "system.video.mode", NULL, out, sizeof(out));
	CHECK(n > 0 && strstr(out, "video") == NULL, "NULL removes every occurrence");
	CHECK(strstr(out, "my.unknown.key: keep me") != NULL, "removal keeps the rest");

	n = z_cfg_text_set(NULL, 0, "apps.term.auto_connect", "port posix0", out, sizeof(out));
	CHECK(n > 0 && !strcmp(out, "apps.term.auto_connect: port posix0\n"), "new file");

	CHECK(z_cfg_text_set(file, strlen(file), "bad key", "x", out, sizeof(out)) == -1,
		"invalid key refused");
	CHECK(z_cfg_text_set(file, strlen(file), "a.b", "two\nlines", out, sizeof(out)) == -1,
		"newline in value refused");
	CHECK(z_cfg_text_set(file, strlen(file), "a.b", "x", out, 20) == -1,
		"overflow refused");

	// Round trip: what the editor writes, the parser reads back --
	// including a value with a space in it.
	{
		char k[Z_CFG_KEY_MAX], v[Z_CFG_VAL_MAX];
		n = z_cfg_text_set("", 0, "system.rtc.timezone", "New York", out, sizeof(out));
		CHECK(parse(out, k, v) == Z_CFG_LINE_ENTRY && !strcmp(v, "New York"),
			"round trip");
	}

}

// 2026-mm-dd hh:mm:ss UTC as epoch seconds
static uint32_t at(int mon, int day, int h, int m, int s) {
	z_tm_t tm = { 2026, (uint8_t)mon, (uint8_t)day, (uint8_t)h, (uint8_t)m, (uint8_t)s, 0, 0 };
	return z_tm_to_time(&tm);
}

static int local_hour(const z_tz_t *tz, uint32_t utc, bool *dst) {
	z_tm_t tm;
	z_time_to_tm(z_tz_local(tz, utc, dst), &tm);
	return tm.hour;
}

static void test_tz(void) {

	z_tz_t tz;
	bool dst;

	// -- parsing --
	CHECK(z_tz_parse("UTC", &tz) && tz.std_off == 0 && tz.dst == Z_DST_NONE &&
		!strcmp(z_tz_name(&tz, false), "UTC"), "UTC");
	CHECK(z_tz_parse("UTC+2", &tz) && tz.std_off == 7200 &&
		!strcmp(z_tz_name(&tz, false), "UTC+2"), "UTC+2 is ahead");
	CHECK(z_tz_parse("utc-5:30", &tz) && tz.std_off == -(5 * 3600 + 1800) &&
		!strcmp(z_tz_name(&tz, false), "UTC-5:30"), "UTC-5:30, any case");
	CHECK(z_tz_parse("UTC+14", &tz) && !z_tz_parse("UTC+15", &tz), "offset range");
	CHECK(z_tz_parse("berlin", &tz) && tz.std_off == 3600 && tz.dst == Z_DST_EU &&
		tz.city && !strcmp(tz.city->city, "Berlin"), "city, any case");
	CHECK(z_tz_parse("New York", &tz) && tz.std_off == -5 * 3600, "city with a space");
	CHECK(z_tz_parse("Kathmandu", &tz) && tz.std_off == 5 * 3600 + 45 * 60 &&
		!strcmp(z_tz_name(&tz, false), "UTC+5:45"), "no abbreviation shows the offset");
	CHECK(!z_tz_parse("Atlantis", &tz) && tz.std_off == 0 &&
		!strcmp(tz.std_name, "UTC"), "unknown city: false, and UTC");
	CHECK(!z_tz_parse("CET-1CEST,M3.5.0,M10.5.0/3", &tz), "POSIX rules are not accepted");

	// -- the table --
	{
		bool sorted = true, all_parse = true;
		for (int i = 0; i < z_tz_city_count; i++) {
			if (i && strcmp(z_tz_cities[i - 1].city, z_tz_cities[i].city) >= 0)
				sorted = false;
			if (!z_tz_parse(z_tz_cities[i].city, &tz) || tz.city != &z_tz_cities[i])
				all_parse = false;
		}
		CHECK(sorted, "city table is sorted and has no duplicates");
		CHECK(all_parse, "every city parses back to its own entry");
		CHECK(z_tz_city_count >= 50, "a useful number of cities");
	}

	// -- EU: 29 Mar 01:00 UTC and 25 Oct 01:00 UTC, 2026 --
	z_tz_parse("Berlin", &tz);
	CHECK(local_hour(&tz, at(3, 29, 0, 59, 59), &dst) == 1 && !dst, "CET 01:59:59 before start");
	CHECK(local_hour(&tz, at(3, 29, 1, 0, 0), &dst) == 3 && dst, "CEST 03:00 at start");
	CHECK(!strcmp(z_tz_name(&tz, dst), "CEST"), "CEST name");
	CHECK(local_hour(&tz, at(9, 11, 12, 0, 0), &dst) == 14 && dst, "11 Sep 2026 is CEST");
	CHECK(local_hour(&tz, at(10, 25, 0, 59, 59), &dst) == 2 && dst, "CEST 02:59:59 before end");
	CHECK(local_hour(&tz, at(10, 25, 1, 0, 0), &dst) == 2 && !dst, "CET 02:00 after end");
	CHECK(local_hour(&tz, at(12, 31, 23, 30, 0), &dst) == 0 && !dst, "New Year's Eve rolls over");

	z_tz_parse("London", &tz);
	CHECK(local_hour(&tz, at(3, 29, 1, 0, 0), &dst) == 2 && dst &&
		!strcmp(z_tz_name(&tz, dst), "BST"), "BST at the same instant as CEST");
	CHECK(local_hour(&tz, at(1, 15, 9, 0, 0), &dst) == 9 && !dst, "GMT in winter");

	z_tz_parse("Athens", &tz);
	CHECK(local_hour(&tz, at(3, 29, 1, 0, 0), &dst) == 4 && dst, "EEST, same instant");

	// -- US Eastern 2026: 8 Mar 07:00 UTC and 1 Nov 06:00 UTC --
	z_tz_parse("New York", &tz);
	CHECK(local_hour(&tz, at(3, 8, 6, 59, 59), &dst) == 1 && !dst, "EST 01:59:59");
	CHECK(local_hour(&tz, at(3, 8, 7, 0, 0), &dst) == 3 && dst, "EDT 03:00");
	CHECK(local_hour(&tz, at(11, 1, 5, 59, 59), &dst) == 1 && dst, "EDT 01:59:59");
	CHECK(local_hour(&tz, at(11, 1, 6, 0, 0), &dst) == 1 && !dst, "EST 01:00 again");

	// -- US Pacific: 8 Mar 10:00 UTC --
	z_tz_parse("Los Angeles", &tz);
	CHECK(local_hour(&tz, at(3, 8, 9, 59, 59), &dst) == 1 && !dst, "PST 01:59:59");
	CHECK(local_hour(&tz, at(3, 8, 10, 0, 0), &dst) == 3 && dst, "PDT 03:00");

	// -- Sydney 2026: ends 4 Apr 16:00 UTC, starts 3 Oct 16:00 UTC --
	z_tz_parse("Sydney", &tz);
	CHECK(local_hour(&tz, at(1, 10, 0, 0, 0), &dst) == 11 && dst, "January is summer");
	CHECK(local_hour(&tz, at(4, 4, 15, 59, 59), &dst) == 2 && dst, "AEDT 02:59:59 before end");
	CHECK(local_hour(&tz, at(4, 4, 16, 0, 0), &dst) == 2 && !dst, "AEST 02:00 after end");
	CHECK(local_hour(&tz, at(10, 3, 15, 59, 59), &dst) == 1 && !dst, "AEST 01:59:59");
	CHECK(local_hour(&tz, at(10, 3, 16, 0, 0), &dst) == 3 && dst, "AEDT 03:00");

	// -- Auckland 2026: ends 4 Apr 14:00 UTC, starts 26 Sep 14:00 UTC --
	z_tz_parse("Auckland", &tz);
	CHECK(local_hour(&tz, at(4, 4, 13, 59, 59), &dst) == 2 && dst, "NZDT 02:59:59 before end");
	CHECK(local_hour(&tz, at(4, 4, 14, 0, 0), &dst) == 2 && !dst, "NZST 02:00 after end");
	CHECK(local_hour(&tz, at(9, 26, 13, 59, 59), &dst) == 1 && !dst, "NZST 01:59:59");
	CHECK(local_hour(&tz, at(9, 26, 14, 0, 0), &dst) == 3 && dst, "NZDT 03:00");

	// -- no DST --
	z_tz_parse("Tokyo", &tz);
	CHECK(local_hour(&tz, at(7, 1, 0, 0, 0), &dst) == 9 && !dst &&
		!strcmp(z_tz_name(&tz, dst), "JST"), "Tokyo, no DST");
	z_tz_parse("Brisbane", &tz);
	CHECK(local_hour(&tz, at(1, 10, 0, 0, 0), &dst) == 10 && !dst, "Brisbane has no DST");

	// clamps rather than wrapping below the epoch
	z_tz_parse("Los Angeles", &tz);
	CHECK(z_tz_local(&tz, 3600, NULL) == 0, "clamped at the epoch");

}

int main(void) {

	test_parse();
	test_text_set();
	test_tz();

	printf("test_zcfg: %d checks, %d failed\n", run, failed);
	return failed ? 1 : 0;

}
