#ifndef ZUTF8_H
#define ZUTF8_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * UTF-8 and ISO 8859-15 (Latin-9) helpers. See docs/text_encoding.md.
 *
 * The rule these exist for: text that crosses a process boundary --
 * the clipboard, messages, port streams, window titles, speech -- is
 * UTF-8. ASCII is already valid UTF-8, so an ASCII-only app follows
 * the rule without doing anything. An app that keeps text internally
 * as single Latin-9 bytes converts at its edges with the functions
 * here; an app that keeps UTF-8 internally walks it with
 * z_utf8_next().
 *
 * Header-only (static inline) on purpose: every app Makefile lists its
 * common objects by hand, and a header needs no Makefile change to
 * use. Anything an app does not call costs it nothing.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// What z_utf8_next() returns for a byte that does not start a valid
// sequence (the Unicode replacement character). It consumes exactly
// one byte in that case, so a caller that wants to keep invalid bytes
// intact -- an editor saving a file it could not fully decode -- knows
// the malformed byte is s[0] and can copy it through unchanged.
#define Z_UTF8_BAD  0xFFFDu

// Longest UTF-8 encoding of one codepoint.
#define Z_UTF8_MAX  4

// -- decoding --

// Decodes the codepoint at *s, not reading at or past `end`, and
// advances *s past it. Returns Z_UTF8_BAD (advancing one byte) for a
// malformed, overlong, truncated or surrogate sequence -- never reads
// past `end`, never returns a value above 0x10FFFF. *s == end is the
// caller's to check first; this returns 0 and does not advance there.
static inline uint32_t z_utf8_next(const char **s, const char *end) {

	const uint8_t *p = (const uint8_t *)*s;
	const uint8_t *e = (const uint8_t *)end;

	if (p >= e) return 0;

	uint8_t b = p[0];
	uint32_t cp, min;
	int n;

	if (b < 0x80) { *s = (const char *)(p + 1); return b; }
	else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; n = 1; min = 0x80; }
	else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; n = 2; min = 0x800; }
	else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; n = 3; min = 0x10000; }
	else goto bad;

	if (e - p <= n) goto bad;

	for (int i = 1; i <= n; i++) {
		if ((p[i] & 0xC0) != 0x80) goto bad;
		cp = (cp << 6) | (p[i] & 0x3F);
	}

	if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000))
		goto bad;

	*s = (const char *)(p + 1 + n);
	return cp;

bad:
	*s = (const char *)(p + 1);
	return Z_UTF8_BAD;

}

// Decodes a NUL-terminated string the same way (the NUL is the end).
static inline uint32_t z_utf8_next_z(const char **s) {
	const char *p = *s;
	const char *end = p;
	// A sequence is at most 4 bytes; look no further than that, and
	// never past the terminator.
	while (end < p + Z_UTF8_MAX && *end) end++;
	return z_utf8_next(s, end);
}

// True if s[0..len) is entirely valid UTF-8. (A malformed sequence is
// the only thing that makes z_utf8_next() consume a single byte of
// 0x80 or above; a real U+FFFD in the text is three bytes, and valid.)
static inline bool z_utf8_valid(const char *s, size_t len) {
	const char *end = s + len;
	while (s < end) {
		const char *before = s;
		z_utf8_next(&s, end);
		if (s - before == 1 && (uint8_t)before[0] >= 0x80) return false;
	}
	return true;
}

// Number of codepoints in s[0..len), counting each malformed byte as
// one (which is also how an editor shows them: one placeholder each).
static inline size_t z_utf8_count(const char *s, size_t len) {
	const char *end = s + len;
	size_t n = 0;
	while (s < end) { z_utf8_next(&s, end); n++; }
	return n;
}

// -- encoding --

// Writes cp as UTF-8 into out (room for Z_UTF8_MAX bytes) and returns
// the length. A value that is not a codepoint (a surrogate, or above
// 0x10FFFF) is written as the replacement character.
static inline int z_utf8_put(uint32_t cp, char *out) {

	uint8_t *o = (uint8_t *)out;

	if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) cp = Z_UTF8_BAD;

	if (cp < 0x80) { o[0] = (uint8_t)cp; return 1; }
	if (cp < 0x800) {
		o[0] = (uint8_t)(0xC0 | (cp >> 6));
		o[1] = (uint8_t)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		o[0] = (uint8_t)(0xE0 | (cp >> 12));
		o[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
		o[2] = (uint8_t)(0x80 | (cp & 0x3F));
		return 3;
	}
	o[0] = (uint8_t)(0xF0 | (cp >> 18));
	o[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
	o[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
	o[3] = (uint8_t)(0x80 | (cp & 0x3F));
	return 4;

}

// -- display width --
//
// How many terminal columns a character takes: 0 for a combining mark
// or a zero-width character, 2 for an East Asian wide one (CJK, kana,
// hangul, fullwidth forms, most emoji), 1 for everything else. The
// same answer, in the ranges that matter, as glibc's wcwidth() -- which
// is what a remote program (vim over ssh) uses to decide where its
// cursor is, so a terminal that disagrees draws everything after a
// wide character in the wrong column.
static inline int z_cp_width(uint32_t cp) {

	if (cp == 0) return 0;

	// zero width: combining marks, ZW space/joiners, variation selectors
	if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) ||
	    (cp >= 0x0591 && cp <= 0x05BD) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
	    (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x200B && cp <= 0x200F) ||
	    (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
	    (cp >= 0xFE20 && cp <= 0xFE2F) || cp == 0xFEFF ||
	    (cp >= 0x3099 && cp <= 0x309A))
		return 0;

	// wide
	if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
	    (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
	    (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
	    (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
	    (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
	    (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
	    (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD))
		return 2;

	return 1;

}

// -- ISO 8859-15 (Latin-9) --
//
// Latin-9 is Latin-1 with eight slots changed, to make room for the
// euro sign and the letters French and Finnish/Estonian need:
//
//   byte  Latin-1        Latin-9
//   0xA4  ¤ U+00A4   ->  € U+20AC
//   0xA6  ¦ U+00A6   ->  Š U+0160
//   0xA8  ¨ U+00A8   ->  š U+0161
//   0xB4  ´ U+00B4   ->  Ž U+017D
//   0xB8  ¸ U+00B8   ->  ž U+017E
//   0xBC  ¼ U+00BC   ->  Œ U+0152
//   0xBD  ½ U+00BD   ->  œ U+0153
//   0xBE  ¾ U+00BE   ->  Ÿ U+0178
//
// Everything else maps a byte to the codepoint of the same value.
// Bytes 0x80-0x9F are C1 control codes in both.

// The codepoint a Latin-9 byte stands for.
static inline uint32_t z_l9_to_cp(uint8_t b) {
	switch (b) {
		case 0xA4: return 0x20AC;
		case 0xA6: return 0x0160;
		case 0xA8: return 0x0161;
		case 0xB4: return 0x017D;
		case 0xB8: return 0x017E;
		case 0xBC: return 0x0152;
		case 0xBD: return 0x0153;
		case 0xBE: return 0x0178;
		default:   return b;
	}
}

// The Latin-9 byte for a codepoint, or 0 if Latin-9 has none -- which
// includes the eight Latin-1 characters Latin-9 dropped (¤ ¦ ¨ ´ ¸ ¼
// ½ ¾) and the C1 controls. NUL maps to 0 as well, which is never a
// character anyone draws or stores in text.
static inline uint8_t z_cp_to_l9(uint32_t cp) {
	if (cp < 0x80) return (uint8_t)cp;
	if (cp < 0xA0) return 0;
	if (cp < 0x100) {
		switch (cp) {
			case 0xA4: case 0xA6: case 0xA8: case 0xB4:
			case 0xB8: case 0xBC: case 0xBD: case 0xBE:
				return 0;
			default:
				return (uint8_t)cp;
		}
	}
	switch (cp) {
		case 0x20AC: return 0xA4;
		case 0x0160: return 0xA6;
		case 0x0161: return 0xA8;
		case 0x017D: return 0xB4;
		case 0x017E: return 0xB8;
		case 0x0152: return 0xBC;
		case 0x0153: return 0xBD;
		case 0x0178: return 0xBE;
		default:     return 0;
	}
}

// Latin-9 text to UTF-8. Converts src[0..len) into out (capacity
// outcap, always NUL-terminated if outcap > 0) and returns the number
// of bytes written, not counting the NUL. Stops early, at a character
// boundary, if out fills.
static inline size_t z_l9_to_utf8(const char *src, size_t len,
	char *out, size_t outcap) {

	size_t o = 0;
	if (!outcap) return 0;

	for (size_t i = 0; i < len; i++) {
		char buf[Z_UTF8_MAX];
		int n = z_utf8_put(z_l9_to_cp((uint8_t)src[i]), buf);
		if (o + (size_t)n + 1 > outcap) break;
		for (int k = 0; k < n; k++) out[o++] = buf[k];
	}
	out[o] = 0;
	return o;

}

// UTF-8 text to Latin-9. Converts src[0..len) into out (capacity
// outcap, always NUL-terminated if outcap > 0) and returns the number
// of bytes written, not counting the NUL. A character Latin-9 cannot
// hold becomes `subst` (typically '?'); *lost, if not NULL, gets the
// number of such characters, so a caller can warn before discarding
// them.
static inline size_t z_utf8_to_l9(const char *src, size_t len,
	char *out, size_t outcap, char subst, size_t *lost) {

	const char *end = src + len;
	size_t o = 0, nlost = 0;
	if (!outcap) { if (lost) *lost = 0; return 0; }

	while (src < end && o + 1 < outcap) {
		uint32_t cp = z_utf8_next(&src, end);
		uint8_t b = z_cp_to_l9(cp);
		if (!b) { b = (uint8_t)subst; nlost++; }
		out[o++] = (char)b;
	}
	out[o] = 0;
	if (lost) *lost = nlost;
	return o;

}

#endif
