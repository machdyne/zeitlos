/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * bbs -- UTF-8, CP437 and the dates on screen. Text is UTF-8 inside
 * the BBS and in every file it keeps; a caller's terminal may want
 * CP437 (SyncTERM, NetRunner, the DOS tradition) or plain ASCII, and
 * out.c translates at the edge with what is here.
 */
#include <string.h>
#include "bbs_int.h"

uint32_t utf8_next(const char **sp, const char *end) {
	const uint8_t *s = (const uint8_t *)*sp;
	uint32_t cp;
	int need;
	if ((const char *)s >= end) return 0;
	uint8_t c = *s++;
	if (c < 0x80) { *sp = (const char *)s; return c; }
	if (c >= 0xC2 && c <= 0xDF) { cp = c & 0x1F; need = 1; }
	else if (c >= 0xE0 && c <= 0xEF) { cp = c & 0x0F; need = 2; }
	else if (c >= 0xF0 && c <= 0xF4) { cp = c & 0x07; need = 3; }
	else { *sp = (const char *)s; return 0xFFFD; }
	uint32_t min = need == 1 ? 0x80 : need == 2 ? 0x800 : 0x10000;
	while (need--) {
		if ((const char *)s >= end || (*s & 0xC0) != 0x80) { *sp = (const char *)s; return 0xFFFD; }
		cp = (cp << 6) | (*s++ & 0x3F);
	}
	*sp = (const char *)s;
	if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return 0xFFFD;
	return cp;
}

int utf8_put(uint32_t cp, char *o) {
	if (cp < 0x80) { o[0] = (char)cp; return 1; }
	if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
	if (cp < 0x10000) {
		o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
	}
	o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

int utf8_chars(const char *s) {
	const char *end = s + strlen(s);
	int n = 0;
	while (s < end) { utf8_next(&s, end); n++; }
	return n;
}

// Code page 437's upper half, 0x80-0xFF, as Unicode.
static const uint16_t cp437_hi[128] = {
	0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7,
	0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
	0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
	0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
	0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA,
	0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
	0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
	0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
	0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
	0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
	0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B,
	0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
	0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
	0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
	0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
	0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

uint8_t cp437_from(uint32_t cp) {
	if (cp < 0x80) return (uint8_t)cp;
	for (int i = 0; i < 128; i++) if (cp437_hi[i] == cp) return (uint8_t)(0x80 + i);
	return 0;
}

uint32_t cp437_to(uint8_t b) {
	return b < 0x80 ? b : cp437_hi[b - 0x80];
}

char ascii_from(uint32_t cp) {
	if (cp < 0x80) return (char)cp;
	if (cp >= 0x2500 && cp <= 0x257F) {
		// lines: along, across, or a junction
		static const uint16_t horiz[] = { 0x2500, 0x2501, 0x2504, 0x2505, 0x2508, 0x2509,
			0x254C, 0x254D, 0x2574, 0x2576, 0x2578, 0x257A, 0x257C, 0x257E, 0 };
		static const uint16_t vert[] = { 0x2502, 0x2503, 0x2506, 0x2507, 0x250A, 0x250B,
			0x254E, 0x254F, 0x2551, 0x2575, 0x2577, 0x2579, 0x257B, 0x257D, 0x257F, 0 };
		if (cp == 0x2550) return '=';
		for (int i = 0; horiz[i]; i++) if (horiz[i] == cp) return '-';
		for (int i = 0; vert[i]; i++) if (vert[i] == cp) return '|';
		return '+';
	}
	switch (cp) {
	case 0x2588: case 0x2593: case 0x2580: case 0x2584: case 0x258C: case 0x2590: case 0x25A0: return '#';
	case 0x2592: return ':';
	case 0x2591: return '.';
	case 0x00A0: return ' ';
	case 0x20AC: return 'E';
	case 0x00DF: return 's';
	case 0x2018: case 0x2019: return '\'';
	case 0x201C: case 0x201D: return '"';
	case 0x2013: case 0x2014: return '-';
	case 0x2022: case 0x00B7: case 0x2219: return '*';
	}
	if (cp >= 0xC0 && cp <= 0xFF) {
		static const char lat[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
		return lat[cp - 0xC0];
	}
	return '?';
}

void fmt_time(uint32_t t, char *out) {
	if (!t) { strcpy(out, "never"); return; }
	uint32_t days = t / 86400, sod = t % 86400;
	// civil date from days since 1970-01-01 (Howard Hinnant's algorithm)
	int32_t z = (int32_t)days + 719468;
	int32_t era = z / 146097;
	uint32_t doe = (uint32_t)(z - era * 146097);
	uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int32_t y = (int32_t)yoe + era * 400;
	uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	uint32_t mp = (5 * doy + 2) / 153;
	uint32_t d = doy - (153 * mp + 2) / 5 + 1;
	uint32_t m = mp < 10 ? mp + 3 : mp - 9;
	if (m <= 2) y++;
	unsigned hh = (unsigned)(sod / 3600), mm = (unsigned)((sod / 60) % 60);
	out[0] = (char)('0' + (y / 1000) % 10); out[1] = (char)('0' + (y / 100) % 10);
	out[2] = (char)('0' + (y / 10) % 10);   out[3] = (char)('0' + y % 10);
	out[4] = '-'; out[5] = (char)('0' + m / 10); out[6] = (char)('0' + m % 10);
	out[7] = '-'; out[8] = (char)('0' + d / 10); out[9] = (char)('0' + d % 10);
	out[10] = ' '; out[11] = (char)('0' + hh / 10); out[12] = (char)('0' + hh % 10);
	out[13] = ':'; out[14] = (char)('0' + mm / 10); out[15] = (char)('0' + mm % 10);
	out[16] = 0;
}

// Copies src into dst[cap], cut at a character boundary if it must be
// cut: a truncated string is still UTF-8.
void utf8_copy(char *dst, size_t cap, const char *src) {
	size_t n = strlen(src);
	if (!cap) return;
	if (n >= cap) {
		n = cap - 1;
		while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;
	}
	memcpy(dst, src, n);
	dst[n] = 0;
}

// src cut to `width` characters and padded with spaces to exactly
// `width`, for columns: printf's %-20s counts bytes, and a handle with
// an accent in it would push the rest of its line out of line.
void utf8_pad(char *dst, size_t cap, const char *src, int width) {
	const char *p = src, *end = src + strlen(src);
	size_t o = 0;
	int w = 0;
	while (p < end && w < width) {
		const char *c = p;
		utf8_next(&p, end);
		size_t k = (size_t)(p - c);
		if (o + k + 1 > cap) break;
		memcpy(dst + o, c, k);
		o += k;
		w++;
	}
	while (w < width && o + 2 <= cap) { dst[o++] = ' '; w++; }
	dst[o] = 0;
}
