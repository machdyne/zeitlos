#ifndef FSVOL_H
#define FSVOL_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Pure helpers for fs (sw/apps/fs/fs.c): FatFs drive prefixes, the
 * checks a rename makes, and FAT's packed timestamps. Moved here from
 * the kernel's sw/os/fs/fs.h when the filesystem left the kernel;
 * tests/test_fsvol.c checks them, as the kernel's test_fsrename.c did.
 */

#include <stdint.h>
#include <stdbool.h>

// The FatFs drive a resolved path is on: "1:/x" is 1, "/x" is 0.
static inline int fs_vol_of(const char *p) {
	return (p[0] >= '0' && p[0] <= '9' && p[1] == ':') ? p[0] - '0' : 0;
}

// The path with its drive prefix removed.
static inline const char *fs_vol_strip(const char *p) {
	return (p[0] >= '0' && p[0] <= '9' && p[1] == ':') ? p + 2 : p;
}

// "" and "/" (after the drive) are a volume's root.
static inline bool fs_is_root(const char *p) {
	p = fs_vol_strip(p);
	while (*p == '/') p++;
	return *p == 0;
}

// Is `inner` the directory `outer` itself, or somewhere below it?
// Case-insensitive, as FAT names are. Trailing slashes on `outer` are
// ignored, so "/a/" and "/a" are the same directory.
static inline bool fs_is_within(const char *outer, const char *inner) {
	uint32_t n = 0, i;
	while (outer[n]) n++;
	while (n > 1 && outer[n - 1] == '/') n--;
	for (i = 0; i < n; i++) {
		char a = outer[i], b = inner[i];
		if (a >= 'a' && a <= 'z') a -= 32;
		if (b >= 'a' && b <= 'z') b -= 32;
		if (a != b) return false;
	}
	return inner[n] == 0 || inner[n] == '/';
}

// FAT's packed date/time (as get_fattime() returns it) for `t`, Unix
// seconds, UTC, which must be at or after 1980-01-01 (315532800).
// Among the years FAT can hold (1980-2107), a year divisible by 4 is a
// leap year with ONE exception, 2100 (y == 120 here) -- which the RTC's
// uint32 seconds do reach (they run out in 2106), so it has to be
// right; test_fsrename.c checks every day through 2106. Years past
// 2107 clamp to FAT's last second.
#define FS_LEAP(y)	(!((y) & 3) && (y) != 120)	// y: years since 1980
static inline uint32_t fs_fattime_of(uint32_t t) {
	static const uint8_t mdays[12] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
	uint32_t days, secs, y, m, n;
	t -= 315532800u;			// seconds since FAT's epoch
	days = t / 86400;
	secs = t % 86400;
	for (y = 0; days >= (n = FS_LEAP(y) ? 366 : 365); y++) days -= n;
	for (m = 0; m < 11; m++) {
		n = mdays[m] + (m == 1 && FS_LEAP(y));
		if (days < n) break;
		days -= n;
	}
	if (y > 127) return (127u << 25) | (12u << 21) | (31u << 16) |
		(23u << 11) | (59u << 5) | 29u;
	return (y << 25) | ((m + 1) << 21) | ((days + 1) << 16) |
		((secs / 3600) << 11) | (((secs / 60) % 60) << 5) | ((secs % 60) / 2);
}

#endif
