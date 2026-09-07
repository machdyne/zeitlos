/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See uni.h.
 */

#include <string.h>

#include "uni.h"

#define UNI_REPLACEMENT 0xFFFDu

uint32_t uni_utf8_next(const char *s, uint32_t len, uint32_t *cp) {

	const unsigned char *p = (const unsigned char *)s;
	uint32_t need, v, i;

	if (len == 0) { *cp = 0; return 1; }

	if (p[0] < 0x80) { *cp = p[0]; return 1; }

	if ((p[0] & 0xE0) == 0xC0) { need = 2; v = p[0] & 0x1Fu; }
	else if ((p[0] & 0xF0) == 0xE0) { need = 3; v = p[0] & 0x0Fu; }
	else if ((p[0] & 0xF8) == 0xF0) { need = 4; v = p[0] & 0x07u; }
	else { *cp = UNI_REPLACEMENT; return 1; }		// stray continuation

	if (len < need) { *cp = UNI_REPLACEMENT; return 1; }

	for (i = 1; i < need; i++) {
		if ((p[i] & 0xC0) != 0x80) { *cp = UNI_REPLACEMENT; return 1; }
		v = (v << 6) | (uint32_t)(p[i] & 0x3Fu);
	}

	// Overlong forms and surrogates are rejected rather than decoded.
	// An overlong encoding of '/' or '.' is the classic way to slip a
	// path separator past a filter; nothing here filters paths today,
	// but decoding one to the character it pretends to be is how that
	// becomes true later without anyone noticing.
	if ((need == 2 && v < 0x80) ||
		(need == 3 && v < 0x800) ||
		(need == 4 && v < 0x10000) ||
		(v >= 0xD800 && v <= 0xDFFF) ||
		v > 0x10FFFF) {
		*cp = UNI_REPLACEMENT;
		return 1;
	}

	*cp = v;
	return need;

}

bool uni_is_space(uint32_t cp) {
	return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' ||
		cp == '\f' || cp == 0x00A0u || cp == 0x2007u || cp == 0x202Fu;
}

// -- Latin-1 and Latin Extended-A, folded to their base letter ----
//
// A flat table over 0xC0..0x17F. One byte per codepoint, so 192 bytes
// of .rodata for the whole of the Latin alphabet as European
// languages actually write it. A '?' entry means "no sensible base
// letter"; those fall through to the general rules below.
//
// 0xDF (eszett) and 0xC6/0xE6 (AE ligature) want TWO letters and are
// handled separately -- one byte per entry is what keeps this table
// small, and the exceptions are few enough to name.
static const char latin1_base[] =
	/* C0 */ "AAAAAA?CEEEEIIII"
	/* D0 */ "DNOOOOO?OUUUUY??"
	/* E0 */ "aaaaaa?ceeeeiiii"
	/* F0 */ "dnooooo?ouuuuy?y";

static const char latin_a_base[] =
	/* 100 */ "AaAaAaCcCcCcCcDd"
	/* 110 */ "DdEeEeEeEeEeGgGg"
	/* 120 */ "GgGgHhHhIiIiIiIi"
	/* 130 */ "Ii??JjKkkLlLlLlL"
	/* 140 */ "lLlNnNnNnnNnOoOo"
	/* 150 */ "Oo??RrRrRrSsSsSs"
	/* 160 */ "SsTtTtTtUuUuUuUu"
	/* 170 */ "UuUuWwYyYZzZzZzs";

uint32_t uni_fold(uint32_t cp, char *out) {

	const char *s = NULL;
	char one = 0;

	out[0] = '\0';

	// -- plain ASCII, the overwhelmingly common case --
	if (cp >= 0x20 && cp < 0x7F) { out[0] = (char)cp; out[1] = '\0'; return 1; }

	// Tab and newline survive: html.c collapses them itself and the
	// preformatted path needs them intact.
	if (cp == '\t' || cp == '\n') { out[0] = (char)cp; out[1] = '\0'; return 1; }

	// Everything else below 0x20, plus DEL, is dropped. A control
	// character in page text is never meaningful and a stray CR would
	// otherwise become a '?' on every line of a CRLF document.
	if (cp < 0x20 || cp == 0x7F) return 0;

	switch (cp) {

	// -- spaces --
	case 0x00A0:					// no-break space
	case 0x2002: case 0x2003:		// en/em space
	case 0x2004: case 0x2005: case 0x2006:
	case 0x2007: case 0x2008: case 0x2009: case 0x200A:
	case 0x202F: case 0x205F: case 0x3000:
		one = ' '; break;

	// Zero-width things: dropped, not spaced. A zero-width joiner
	// rendered as a space breaks a word in half.
	case 0x00AD:					// soft hyphen
	case 0x200B: case 0x200C: case 0x200D:
	case 0xFEFF:					// BOM, if one survived to here
		return 0;

	// -- quotes --
	case 0x2018: case 0x2019: case 0x201A: case 0x201B:
	case 0x2032:					// prime, as in 5' 3"
		one = '\''; break;
	case 0x201C: case 0x201D: case 0x201E: case 0x201F:
	case 0x2033:
		one = '"'; break;
	case 0x00AB: s = "<<"; break;
	case 0x00BB: s = ">>"; break;

	// -- dashes --
	//
	// The em dash becomes "--" rather than "-" because the two mean
	// different things in running prose, and because Markdown's own
	// reader in this tree (sw/apps/read) already shows "--" for it.
	case 0x2010: case 0x2011: case 0x2012: case 0x2013:
	case 0x2212:					// minus sign
		one = '-'; break;
	case 0x2014: case 0x2015: s = "--"; break;

	// -- other punctuation that carries meaning --
	case 0x2026: s = "..."; break;
	case 0x2022: case 0x00B7: case 0x2027:
	case 0x25AA: case 0x25CF: case 0x2043:
		one = '*'; break;			// bullets, in text that escaped a list
	case 0x2192: s = "->"; break;
	case 0x2190: s = "<-"; break;
	case 0x00D7: one = 'x'; break;	// multiplication sign
	case 0x00F7: one = '/'; break;
	case 0x2044: one = '/'; break;	// fraction slash
	case 0x00A9: s = "(c)"; break;
	case 0x00AE: s = "(R)"; break;
	case 0x2122: s = "(TM)"; break;
	case 0x00B0: one = 'o'; break;	// degree
	case 0x2030: one = '%'; break;
	case 0x2020: case 0x2021: one = '+'; break;

	// -- currency: the ones a price is likely to be in --
	case 0x20AC: s = "EUR"; break;
	case 0x00A3: s = "GBP"; break;
	case 0x00A5: s = "JPY"; break;
	case 0x00A2: s = "c"; break;

	// -- ligatures and two-letter foldings --
	case 0x00C6: s = "AE"; break;
	case 0x00E6: s = "ae"; break;
	case 0x0152: s = "OE"; break;
	case 0x0153: s = "oe"; break;
	case 0x00DF: s = "ss"; break;
	case 0x00DE: s = "Th"; break;
	case 0x00FE: s = "th"; break;
	case 0x00D0: case 0x0110: one = 'D'; break;
	case 0x00F0: case 0x0111: one = 'd'; break;
	case 0xFB00: s = "ff"; break;
	case 0xFB01: s = "fi"; break;
	case 0xFB02: s = "fl"; break;
	case 0xFB03: s = "ffi"; break;
	case 0xFB04: s = "ffl"; break;

	default:
		break;
	}

	if (s) {
		uint32_t n = (uint32_t)strlen(s);
		memcpy(out, s, n + 1);
		return n;
	}

	if (!one) {

		if (cp >= 0x00C0 && cp <= 0x00FF)
			one = latin1_base[cp - 0x00C0];
		else if (cp >= 0x0100 && cp <= 0x017F)
			one = latin_a_base[cp - 0x0100];

		// Combining marks (U+0300..U+036F) are dropped rather than
		// replaced. They follow a base letter that has already been
		// emitted, so dropping leaves the word readable; a '?' after
		// every accented letter would not be.
		else if (cp >= 0x0300 && cp <= 0x036F)
			return 0;

	}

	// A '?' from either table is the table's own "no sensible base
	// letter" marker, and it is also exactly what an unmapped
	// codepoint should produce -- so there is nothing to distinguish
	// and nothing to convert. See uni.h on why this is not a drop.
	if (!one) one = '?';

	out[0] = one;
	out[1] = '\0';
	return 1;

}
