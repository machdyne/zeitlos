/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Letter-to-sound rules: an English word's spelling to phonemes.
 * See lts.h and docs/tts.md, "Letters to sounds".
 *
 * -- Format --
 *
 * The rule style is the classic one from the public-domain NRL report
 * (Elovitz, Johnson, McHugh and Shore, NRL Report 7948, 1976): each
 * rule is
 *
 *     left context  [ match ]  right context  =  phonemes
 *
 * and the rules for a letter are tried IN ORDER, the first whose match
 * and both contexts fit winning -- so specific rules come first and
 * each letter's list ends with its plain default. The rules themselves
 * were written for this project from ordinary English phonics; they
 * are not the report's table.
 *
 * Context symbols:
 *
 *     ' '  a word boundary
 *     '#'  one or more vowels (A E I O U Y)
 *     ':'  zero or more consonants
 *     '^'  exactly one consonant
 *     '.'  one voiced consonant (B D G J L M N R V W Z)
 *     '+'  a front vowel (E I Y)
 *     '&'  a sibilant (S C G Z X J, CH, SH)            left only
 *     '@'  T S R D L Z N J, TH CH SH -- after which U is "oo"  left only
 *     '%'  a suffix: E ER ES ED ING ELY, then the end    right only
 *
 * Output is ARPAbet with no stress; lts.c's stress pass adds it.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "lts.h"

typedef struct { const char *l, *m, *r, *out; } rule_t;

#define END { 0, 0, 0, 0 }

static const rule_t R_A[] = {
	{ " ",   "ARE",   " ",    "AA R" },
	{ " ",   "AR",    "O",    "AX R" },
	{ "",    "ARR",   "",     "AE R" },
	{ "W",   "AR",    "",     "AO R" },
	{ "",    "AR",    "#",    "EH R" },
	{ "",    "AR",    "",     "AA R" },
	{ "",    "AIR",   "",     "EH R" },
	{ "",    "AI",    "",     "EY" },
	{ "",    "AY",    "",     "EY" },
	{ "",    "AUGH",  "",     "AO" },
	{ "",    "AU",    "",     "AO" },
	{ "",    "AW",    "",     "AO" },
	{ " :",  "ANY",   "",     "EH N IY" },
	{ "",    "AGAIN", "",     "AX G EH N" },
	{ "#:",  "ALLY",  "",     "AX L IY" },
	{ "",    "ALL",   "",     "AO L" },
	{ "",    "ALT",   "",     "AO L T" },
	{ "",    "ALK",   "",     "AO K" },
	{ "",    "ALW",   "",     "AO L W" },
	{ "#:",  "AL",    " ",    "AX L" },
	{ "#:",  "ALS",   " ",    "AX L Z" },
	{ " ",   "AL",    "#",    "AX L" },
	{ "",    "ATION", "",     "EY SH AX N" },
	{ "",    "ANGE",  "",     "EY N JH" },
	{ "#:",  "AGE",   " ",    "IX JH" },
	{ "W",   "A",     "T",    "AA" },
	{ "W",   "A",     "SH",   "AA" },
	{ "",    "A",     "^E ",  "EY" },
	{ "",    "A",     "^ES ", "EY" },
	{ "",    "A",     "^ED ", "EY" },
	{ "",    "A",     "^ING", "EY" },
	{ "",    "A",     "^ER ", "EY" },
	{ "",    "A",     "^IA",  "EY" },
	{ " ",   "A",     "^#",   "AX" },
	{ "",    "A",     " ",    "AX" },
	{ "",    "A",     "",     "AE" },
	END
};

static const rule_t R_B[] = {
	{ "",    "BB",    "",     "B" },
	{ "M",   "B",     " ",    "" },
	{ "",    "B",     "",     "B" },
	END
};

static const rule_t R_C[] = {
	{ "S",   "CH",    "",     "K" },
	{ "",    "CHR",   "",     "K R" },
	{ "",    "CH",    "",     "CH" },
	{ "",    "CK",    "",     "K" },
	{ "",    "CC",    "+",    "K S" },
	{ "",    "CC",    "",     "K" },
	{ "",    "CIAL",  "",     "SH AX L" },
	{ "",    "CIAN",  "",     "SH AX N" },
	{ "",    "CIOUS", "",     "SH AX S" },
	{ "",    "CIENT", "",     "SH AX N T" },
	{ "",    "C",     "+",    "S" },
	{ "",    "C",     "",     "K" },
	END
};

static const rule_t R_D[] = {
	{ "",    "DD",    "",     "D" },
	{ "",    "DG",    "",     "JH" },
	{ "",    "DURE",  "",     "JH ER" },
	{ "",    "D",     "",     "D" },
	END
};

static const rule_t R_E[] = {
	// -ED: "wanted" / "stopped" / "played"
	{ "#:T", "ED",    " ",    "IX D" },
	{ "#:D", "ED",    " ",    "IX D" },
	{ "#:P", "ED",    " ",    "T" },
	{ "#:K", "ED",    " ",    "T" },
	{ "#:S", "ED",    " ",    "T" },
	{ "#:F", "ED",    " ",    "T" },
	{ "#:H", "ED",    " ",    "T" },
	{ "#:X", "ED",    " ",    "T" },
	{ "#:C", "ED",    " ",    "T" },
	{ "#:",  "ED",    " ",    "D" },
	// -ES after a sibilant: "places", "boxes"
	{ "#:&", "ES",    " ",    "IX Z" },
	{ "#:",  "E",     "S ",   "" },
	{ "#:",  "ELY",   " ",    "L IY" },
	{ "#:",  "EMENT", "",     "M AX N T" },
	{ "#:",  "ENESS", "",     "N AX S" },
	{ "#:",  "EN",    " ",    "AX N" },
	{ "#:",  "ER",    " ",    "ER" },
	{ "#:",  "ERED",  " ",    "ER D" },
	{ "#:",  "ERS",   " ",    "ER Z" },
	{ "#:",  "ERING", "",     "ER IX NG" },
	{ "#:",  "E",     " ",    "" },
	{ " :",  "E",     " ",    "IY" },
	// prefixes
	{ " R",  "E",     "^#",   "IY" },
	{ " B",  "E",     "^#",   "IH" },
	{ " D",  "E",     "^#",   "IH" },
	{ "",    "EE",    "",     "IY" },
	{ "",    "EAR",   "^",    "ER" },
	{ "",    "EAR",   "",     "IH R" },
	{ "",    "EAD",   "",     "EH D" },
	{ "",    "EAU",   "",     "OW" },
	{ "",    "EA",    "",     "IY" },
	{ "",    "EIGH",  "",     "EY" },
	{ "",    "EI",    "",     "IY" },
	{ "",    "EY",    " ",    "IY" },
	{ "",    "EY",    "",     "EY" },
	{ "@",   "EW",    "",     "UW" },
	{ "",    "EW",    "",     "Y UW" },
	{ "",    "EU",    "",     "Y UW" },
	{ "",    "ERE",   " ",    "IH R" },
	{ "",    "ER",    "#",    "EH R" },
	{ "",    "ER",    "",     "ER" },
	{ "",    "E",     "^E ",  "IY" },
	{ "",    "E",     "^ES ", "IY" },
	{ "",    "E",     "",     "EH" },
	END
};

static const rule_t R_F[] = {
	{ "",    "FF",    "",     "F" },
	{ "",    "F",     "",     "F" },
	END
};

static const rule_t R_G[] = {
	{ "",    "GG",    "",     "G" },
	{ " ",   "GN",    "",     "N" },
	{ "",    "GN",    " ",    "N" },
	{ "",    "GH",    " ",    "" },
	{ " ",   "GH",    "",     "G" },
	{ "",    "GH",    "",     "" },
	{ "",    "GET",   "",     "G EH T" },
	{ "",    "GIVE",  "",     "G IH V" },
	{ "",    "GIRL",  "",     "G ER L" },
	{ " BE", "GIN",   "",     "G IH N" },
	{ "",    "GU",    "#",    "G" },
	{ "",    "G",     "+",    "JH" },
	{ "",    "G",     "",     "G" },
	END
};

static const rule_t R_H[] = {
	{ " ",   "HOUR",  "",     "AW ER" },
	{ "",    "H",     "#",    "HH" },
	{ "",    "H",     "",     "" },
	END
};

static const rule_t R_I[] = {
	{ "",    "IGH",   "",     "AY" },
	{ "",    "IGN",   " ",    "AY N" },
	{ "",    "IND",   " ",    "AY N D" },
	{ "",    "ILD",   "",     "AY L D" },
	{ "",    "IE",    " ",    "AY" },
	{ "",    "IE",    "D ",   "AY" },
	{ "",    "IE",    "S ",   "AY" },
	{ "",    "IE",    "",     "IY" },
	{ "#:",  "ING",   "",     "IX NG" },
	{ "",    "IQUE",  "",     "IY K" },
	{ "#:",  "IVE",   " ",    "IX V" },
	{ "#:",  "ICAL",  "",     "IX K AX L" },
	{ "#:",  "IC",    " ",    "IX K" },
	{ "#:",  "IFY",   "",     "IX F AY" },
	{ "#:",  "ITY",   " ",    "IX T IY" },
	{ "",    "IOUS",  "",     "IY AX S" },
	{ "",    "ION",   "",     "Y AX N" },
	{ "",    "IO",    "",     "IY OW" },
	{ "",    "IA",    "",     "IY AX" },
	{ "",    "IR",    "E",    "AY ER" },
	{ "",    "IR",    "#",    "AY R" },
	{ "",    "IR",    "",     "ER" },
	{ "",    "I",     "^E ",  "AY" },
	{ "",    "I",     "^ES ", "AY" },
	{ "",    "I",     "^ED ", "AY" },
	{ "",    "I",     "^ING", "AY" },
	{ "",    "I",     "^ER ", "AY" },
	{ "",    "I",     " ",    "AY" },
	{ "",    "I",     "",     "IH" },
	END
};

static const rule_t R_J[] = {
	{ "",    "J",     "",     "JH" },
	END
};

static const rule_t R_K[] = {
	{ " ",   "K",     "N",    "" },
	{ "",    "K",     "",     "K" },
	END
};

static const rule_t R_L[] = {
	{ "#:^", "LE",    " ",    "AX L" },
	{ "",    "LL",    "",     "L" },
	{ "",    "L",     "",     "L" },
	END
};

static const rule_t R_M[] = {
	{ "",    "MM",    "",     "M" },
	{ "",    "M",     "",     "M" },
	END
};

static const rule_t R_N[] = {
	{ "",    "NG",    "",     "NG" },
	{ "",    "NK",    "",     "NG K" },
	{ "",    "NN",    "",     "N" },
	{ "",    "N",     "",     "N" },
	END
};

static const rule_t R_O[] = {
	{ "",    "OOK",   "",     "UH K" },
	{ "",    "OOD",   "",     "UH D" },
	{ "",    "OOR",   "",     "AO R" },
	{ "",    "OO",    "",     "UW" },
	{ "",    "OAR",   "",     "AO R" },
	{ "",    "OA",    "",     "OW" },
	{ "",    "OUGHT", "",     "AO T" },
	{ "",    "OUGH",  "",     "OW" },
	{ "",    "OULD",  "",     "UH D" },
	{ "",    "OUR",   "",     "AW ER" },
	{ "",    "OUS",   "",     "AX S" },
	{ "",    "OU",    "",     "AW" },
	{ "",    "OWN",   "",     "AW N" },
	{ "",    "OW",    "",     "OW" },
	{ "",    "OY",    "",     "OY" },
	{ "",    "OI",    "",     "OY" },
	{ "W",   "OR",    "",     "ER" },
	{ "",    "ORE",   "",     "AO R" },
	{ "",    "OR",    "",     "AO R" },
	{ "",    "OLL",   "",     "OW L" },
	{ "",    "O",     "LD",   "OW" },
	{ "",    "O",     "LT",   "OW" },
	{ "",    "OST",   " ",    "OW S T" },
	{ "",    "OE",    " ",    "OW" },
	{ "#:",  "ON",    " ",    "AX N" },
	{ "#:",  "OM",    " ",    "AX M" },
	{ "",    "O",     "^E ",  "OW" },
	{ "",    "O",     "^ES ", "OW" },
	{ "",    "O",     "^ED ", "OW" },
	{ "",    "O",     "^ING", "OW" },
	{ "",    "O",     " ",    "OW" },
	{ "",    "O",     "",     "AA" },
	END
};

static const rule_t R_P[] = {
	{ "",    "PH",    "",     "F" },
	{ "",    "PP",    "",     "P" },
	{ " ",   "PS",    "",     "S" },
	{ "",    "P",     "",     "P" },
	END
};

static const rule_t R_Q[] = {
	{ "",    "QUE",   " ",    "K" },
	{ "",    "QU",    "",     "K W" },
	{ "",    "Q",     "",     "K" },
	END
};

static const rule_t R_R[] = {
	{ " ",   "RH",    "",     "R" },
	{ "",    "RR",    "",     "R" },
	{ "",    "R",     "",     "R" },
	END
};

static const rule_t R_S[] = {
	{ "",    "SH",    "",     "SH" },
	{ "",    "SSION", "",     "SH AX N" },
	{ "#",   "SION",  "",     "ZH AX N" },
	{ "",    "SION",  "",     "SH AX N" },
	{ "#",   "SURE",  "",     "ZH ER" },
	{ "",    "SCH",   "",     "S K" },
	{ "",    "SC",    "+",    "S" },
	{ "",    "SS",    "",     "S" },
	// final S: voiceless after a voiceless sound, voiced otherwise
	{ "PE",  "S",     " ",    "S" },
	{ "TE",  "S",     " ",    "S" },
	{ "KE",  "S",     " ",    "S" },
	{ "FE",  "S",     " ",    "S" },
	{ "P",   "S",     " ",    "S" },
	{ "T",   "S",     " ",    "S" },
	{ "K",   "S",     " ",    "S" },
	{ "F",   "S",     " ",    "S" },
	{ "#",   "S",     " ",    "Z" },
	{ ".",   "S",     " ",    "Z" },
	{ "#",   "S",     "#",    "Z" },
	{ "",    "S",     "",     "S" },
	END
};

static const rule_t R_T[] = {
	{ "",    "TION",  "",     "SH AX N" },
	{ "",    "TIAL",  "",     "SH AX L" },
	{ "",    "TIOUS", "",     "SH AX S" },
	{ "",    "TIENT", "",     "SH AX N T" },
	{ "",    "TURE",  "",     "CH ER" },
	{ "",    "TCH",   "",     "CH" },
	{ "#",   "TH",    "#",    "DH" },
	{ "",    "TH",    "",     "TH" },
	{ "",    "TT",    "",     "T" },
	{ "",    "T",     "",     "T" },
	END
};

static const rule_t R_U[] = {
	{ " ",   "UN",    "^",    "AH N" },
	{ "@",   "UR",    "#",    "UH R" },
	{ "",    "UR",    "#",    "Y UH R" },
	{ "",    "UR",    "",     "ER" },
	{ "P",   "U",     "T ",   "UH" },
	{ "P",   "ULL",   "",     "UH L" },
	{ "B",   "ULL",   "",     "UH L" },
	{ "F",   "ULL",   "",     "UH L" },
	{ "P",   "USH",   "",     "UH SH" },
	{ "B",   "USH",   "",     "UH SH" },
	{ "",    "UE",    " ",    "UW" },
	{ "",    "UI",    "",     "UW" },
	{ "@",   "U",     "^E ",  "UW" },
	{ "@",   "U",     "^ES ", "UW" },
	{ "",    "U",     "^E ",  "Y UW" },
	{ "",    "U",     "^ES ", "Y UW" },
	{ "",    "U",     "^ING", "Y UW" },
	{ "",    "U",     "",     "AH" },
	END
};

static const rule_t R_V[] = {
	{ "",    "V",     "",     "V" },
	END
};

static const rule_t R_W[] = {
	{ " ",   "WR",    "",     "R" },
	{ "",    "WH",    "",     "W" },
	{ "",    "W",     "",     "W" },
	END
};

static const rule_t R_X[] = {
	{ " ",   "X",     "",     "Z" },
	{ "",    "X",     "",     "K S" },
	END
};

static const rule_t R_Y[] = {
	{ " ",   "Y",     "#",    "Y" },
	{ " :",  "Y",     " ",    "AY" },
	{ "#:",  "Y",     " ",    "IY" },
	{ "",    "Y",     "^E ",  "AY" },
	{ "",    "Y",     "^ES ", "AY" },
	{ "",    "Y",     "^ED ", "AY" },
	{ "",    "Y",     "^ING", "AY" },
	{ "",    "Y",     "",     "IH" },
	END
};

static const rule_t R_Z[] = {
	{ "",    "ZZ",    "",     "Z" },
	{ "",    "Z",     "",     "Z" },
	END
};

static const rule_t *const rules[26] = {
	R_A, R_B, R_C, R_D, R_E, R_F, R_G, R_H, R_I, R_J, R_K, R_L, R_M,
	R_N, R_O, R_P, R_Q, R_R, R_S, R_T, R_U, R_V, R_W, R_X, R_Y, R_Z,
};

// -- character classes --

static bool vowel(char c) { return c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U' || c == 'Y'; }
static bool letter(char c) { return c >= 'A' && c <= 'Z'; }
static bool cons(char c) { return letter(c) && !vowel(c); }
static bool voiced(char c) { return c && strchr("BDGJLMNRVWZ", c) != 0; }
static bool front(char c) { return c == 'E' || c == 'I' || c == 'Y'; }

// w is the word with a boundary on each side: w[0] and w[n+1] are ' '.
// Everything outside it is also ' ' as far as these are concerned.

static char at(const char *w, int n, int i) {
	return (i < 0 || i > n + 1) ? ' ' : w[i];
}

// Left context, matched leftward from position p (the letter just
// before the match).
static bool left_ok(const char *pat, const char *w, int n, int p) {
	for (int k = (int)strlen(pat) - 1; k >= 0; k--) {
		char c = pat[k];
		switch (c) {
		case ' ':
			if (letter(at(w, n, p))) return false;
			p--;
			break;
		case '#':
			if (!vowel(at(w, n, p))) return false;
			while (vowel(at(w, n, p))) p--;
			break;
		case ':':
			while (cons(at(w, n, p))) p--;
			break;
		case '^':
			if (!cons(at(w, n, p))) return false;
			p--;
			break;
		case '.':
			if (!voiced(at(w, n, p))) return false;
			p--;
			break;
		case '+':
			if (!front(at(w, n, p))) return false;
			p--;
			break;
		case '&': {
			char a = at(w, n, p), b = at(w, n, p - 1);
			if (a == 'H' && (b == 'C' || b == 'S')) { p -= 2; break; }
			if (a && strchr("SCGZXJ", a)) { p--; break; }
			return false;
		}
		case '@': {
			char a = at(w, n, p), b = at(w, n, p - 1);
			if (a == 'H' && (b == 'T' || b == 'C' || b == 'S')) { p -= 2; break; }
			if (a && strchr("TSRDLZNJ", a)) { p--; break; }
			return false;
		}
		default:
			if (at(w, n, p) != c) return false;
			p--;
			break;
		}
	}
	return true;
}

static int suffix_len(const char *w, int n, int p) {
	static const char *const sfx[] = { "ELY", "ING", "ER", "ES", "ED", "E" };
	for (unsigned i = 0; i < sizeof(sfx) / sizeof(sfx[0]); i++) {
		int l = (int)strlen(sfx[i]);
		if (p + l > n + 1) continue;
		if (strncmp(&w[p], sfx[i], (size_t)l) != 0) continue;
		if (letter(at(w, n, p + l))) continue;
		return l;
	}
	return -1;
}

// Right context, matched rightward from position p (the letter just
// after the match).
static bool right_ok(const char *pat, const char *w, int n, int p) {
	for (; *pat; pat++) {
		char c = *pat;
		switch (c) {
		case ' ':
			if (letter(at(w, n, p))) return false;
			p++;
			break;
		case '#':
			if (!vowel(at(w, n, p))) return false;
			while (vowel(at(w, n, p))) p++;
			break;
		case ':':
			while (cons(at(w, n, p))) p++;
			break;
		case '^':
			if (!cons(at(w, n, p))) return false;
			p++;
			break;
		case '.':
			if (!voiced(at(w, n, p))) return false;
			p++;
			break;
		case '+':
			if (!front(at(w, n, p))) return false;
			p++;
			break;
		case '%': {
			int l = suffix_len(w, n, p);
			if (l < 0) return false;
			p += l;
			break;
		}
		default:
			if (at(w, n, p) != c) return false;
			p++;
			break;
		}
	}
	return true;
}

// -- stress --
//
// Rules produce no stress; English puts primary stress on the first
// syllable of most words, except after an unstressed prefix, and on
// the syllable before -tion/-sion/-ic endings. Crude, and that is
// fine: stress here only lengthens and raises one vowel (phon.c), and
// a misplaced one sounds odd rather than unintelligible.

static bool is_vowel_ph(const char *p) {
	return p[0] && strchr("AEIOU", p[0]) != 0;
}

// Only the open-syllable prefixes, and only when what follows looks
// like the start of a syllable (one consonant, then a vowel):
// "return", "delete", "begin", "about" -- but not "belts", "desk" or
// "reckon". The closed ones (con-, com-, in-, pro-...) are stressed as
// often as not ("control" but "comfort", "product"), so the default
// is left to decide those.
static const char *const prefixes[] = { "BE", "DE", "RE", "PRE", "A", 0 };

static bool is_vowel_letter(char c) {
	return c == 'A' || c == 'E' || c == 'I' || c == 'O' || c == 'U' || c == 'Y';
}

void lts_add_stress(char *out, uint32_t size, const char *word, int n) {

	// Locate vowel phonemes.
	char *v[32];
	int nv = 0;
	for (char *p = out; *p && nv < 32; ) {
		while (*p == ' ') p++;
		if (!*p) break;
		if (is_vowel_ph(p)) v[nv++] = p;
		while (*p && *p != ' ') p++;
	}
	if (nv == 0) return;

	int stressed = 0;
	if (nv >= 2) {
		for (int i = 0; prefixes[i]; i++) {
			int l = (int)strlen(prefixes[i]);
			if (n > l + 2 && strncmp(word, prefixes[i], (size_t)l) == 0 &&
			    !is_vowel_letter(word[l]) && is_vowel_letter(word[l + 1])) {
				stressed = 1;
				break;
			}
		}
		if (n > 5 && (strcmp(word + n - 4, "TION") == 0 || strcmp(word + n - 4, "SION") == 0 ||
		    strcmp(word + n - 2, "IC") == 0 || strcmp(word + n - 4, "ICAL") == 0))
			stressed = nv >= 2 ? nv - 2 : 0;
	}
	// Never on a schwa if anything else is available.
	if ((v[stressed][0] == 'A' && v[stressed][1] == 'X') ||
	    (v[stressed][0] == 'I' && v[stressed][1] == 'X')) {
		for (int i = 0; i < nv; i++)
			if (!(v[i][1] == 'X')) { stressed = i; break; }
	}

	// Rebuild with digits appended to vowels.
	char tmp[256];
	uint32_t t = 0;
	int vi = 0;
	for (char *p = out; *p; ) {
		while (*p == ' ') p++;
		if (!*p) break;
		char *s = p;
		while (*p && *p != ' ') p++;
		uint32_t l = (uint32_t)(p - s);
		if (t + l + 3 >= sizeof(tmp)) break;
		if (t) tmp[t++] = ' ';
		memcpy(&tmp[t], s, l);
		t += l;
		if (is_vowel_ph(s)) {
			bool st = (vi++ == stressed);
			// Unstressed short vowels reduce to schwa, as they do in
			// speech: the second vowel of "citizen", "method",
			// "lemon". Long vowels and diphthongs keep their quality.
			if (!st && l == 2 && (!strncmp(s, "AE", 2) || !strncmp(s, "AA", 2) ||
			    !strncmp(s, "AH", 2) || !strncmp(s, "EH", 2) || !strncmp(s, "UH", 2))) {
				t -= 2;
				tmp[t++] = 'A'; tmp[t++] = 'X';
			} else if (!st && l == 2 && !strncmp(s, "IH", 2)) {
				t -= 2;
				tmp[t++] = 'I'; tmp[t++] = 'X';
			}
			tmp[t++] = st ? '1' : '0';
		}
	}
	tmp[t] = 0;
	if (t + 1 <= size) memcpy(out, tmp, t + 1);

}

// -- the engine --

bool lts_word(const char *word, char *out, uint32_t size) {

	char w[LTS_WORD_MAX + 2];
	int n = 0;
	w[0] = ' ';
	for (const char *s = word; *s && n < LTS_WORD_MAX; s++) {
		char c = *s;
		if (c >= 'a' && c <= 'z') c -= 32;
		if (c < 'A' || c > 'Z') continue;
		w[++n] = c;
	}
	w[n + 1] = ' ';
	if (n == 0 || size == 0) return false;

	uint32_t o = 0;
	out[0] = 0;

	int i = 1;
	while (i <= n) {
		const rule_t *r = rules[w[i] - 'A'];
		bool hit = false;
		for (; r->m; r++) {
			int ml = (int)strlen(r->m);
			if (i + ml - 1 > n) continue;
			if (strncmp(&w[i], r->m, (size_t)ml) != 0) continue;
			if (!left_ok(r->l, w, n, i - 1)) continue;
			if (!right_ok(r->r, w, n, i + ml)) continue;
			uint32_t l = (uint32_t)strlen(r->out);
			if (l && o + l + 2 < size) {
				if (o) out[o++] = ' ';
				memcpy(&out[o], r->out, l);
				o += l;
				out[o] = 0;
			}
			i += ml;
			hit = true;
			break;
		}
		if (!hit) i++;		// every list ends in a default; not reached
	}

	w[n + 1] = 0;
	lts_add_stress(out, size, &w[1], n);
	return out[0] != 0;

}
