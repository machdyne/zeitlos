/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Speaking other languages: text to phonemes from a language's rules,
 * which are data (lang.h). See docs/tts.md, "Other languages".
 *
 * The rules are in the style of the NRL letter-to-sound rules (Elovitz
 * et al., NRL Report 7948, 1976 -- a US Government work, in the public
 * domain), as lts.c's English ones are:
 *
 *     r a[ch]   = X        "ch" after "a" is the velar fricative
 *     r [s]V    = Z        "s" before a vowel is voiced
 *     r #[ge]   = G AX     at the start of a word
 *
 * left context, [the letters], right context = phonemes. In a context,
 * '#' is the edge of the word, V any vowel letter, C any other letter,
 * '.' any letter, {abc} one of those letters; anything else is itself.
 * The first rule that matches at a letter wins, in the file's order, so
 * the specific come before the general. A phoneme followed by ' is the
 * stressed one ("A'"); otherwise the language's stress rule chooses.
 *
 * Everything else in the file -- numbers, letter names, symbols, words
 * said other than the rules say -- is words in the language, said by
 * the same rules.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "lang.h"
#include "text2ph.h"

#ifdef PACK_HOST
#include <stdio.h>
#else
#include "../../common/zfsapp.h"
#include "../../common/zpaths.h"
#endif

// -- the language --

#define MAXR      420		// rules
#define MAXW      160		// words: exceptions and weak words
#define MAXSET    24		// {...} sets in contexts
#define SETN      16
#define CTXN      6
#define MATCHN    6

#define E_EDGE    0xFFF0u
#define E_VOWEL   0xFFF1u
#define E_CONS    0xFFF2u
#define E_ANY     0xFFF3u
#define E_SET     0xFE00u	// + set index

typedef struct {
	uint16_t	m[MATCHN], l[CTXN], r[CTXN];
	uint8_t		mn, ln, rn;
	const char	*out;
	int16_t		next;		// the next rule with the same first letter
} rule_t;

typedef struct {
	const char	*w;		// the word, lowercase UTF-8
	const char	*out;		// phonemes, or NULL for a weak word
} word_t;

enum { ST_FIRST, ST_PENULT, ST_LAST, ST_ANTEPENULT };

static char buf[LANG_FILE_MAX + 1];
static bool active;
static char code[8], name[24];

static rule_t rules[MAXR];
static int nrules;
static int16_t head[64];		// first rule for (letter & 63)
static uint16_t sets[MAXSET][SETN];
static uint8_t setn[MAXSET];
static int nsets;
static word_t words[MAXW];
static int nwords;

static uint16_t vowels[48];
static int nvowels;
static uint8_t stress_rule = ST_FIRST;
static char unstressed = '2';
static uint16_t last_if_ends[24];	// stress the last syllable after these
static int nlast_if_ends;
static const char *prefixes[16];
static int nprefixes;
static bool latin;			// the rules are for Latin letters

static const char *num[100];
static const char *hundred[10], *hundredx[10], *thousand[10];
static const char *thousands_one, *thousands_few, *thousands_many;
static const char *w_minus, *w_point, *w_percent, *w_join, *w_dot;
static uint16_t letter_cp[48];
static const char *letter_w[48];
static int nletters;
static uint16_t sym_cp[24];
static const char *sym_w[24];
static int nsyms;

bool lang_active(void) { return active; }
const char *lang_name(void) { return active ? name : ""; }

// -- UTF-8 and letters --

static uint32_t next_cp(const char *s, uint32_t len, uint32_t *i) {
	const uint8_t *p = (const uint8_t *)s;
	uint32_t k = *i;
	uint8_t c = p[k];
	if (c < 0x80) { *i = k + 1; return c; }
	if ((c & 0xE0) == 0xC0 && k + 1 < len && (p[k + 1] & 0xC0) == 0x80) {
		*i = k + 2;
		return ((uint32_t)(c & 0x1F) << 6) | (p[k + 1] & 0x3F);
	}
	if ((c & 0xF0) == 0xE0 && k + 2 < len && (p[k + 1] & 0xC0) == 0x80 && (p[k + 2] & 0xC0) == 0x80) {
		*i = k + 3;
		return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[k + 1] & 0x3F) << 6) | (p[k + 2] & 0x3F);
	}
	*i = k + 1;
	return 0xFFFD;
}

static uint32_t lower(uint32_t c) {
	if (c >= 'A' && c <= 'Z') return c + 32;
	if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
	if (c >= 0x100 && c <= 0x17F) {
		// pairs, upper even -- but for i/l/n's runs and the y and z ones
		if (c == 0x130) return 'i';
		if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
			return (c & 1) ? c + 1 : c;
		if (c == 0x178) return 0xFF;
		if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c;
		return (c & 1) ? c : c + 1;
	}
	if (c >= 0x410 && c <= 0x42F) return c + 32;
	if (c >= 0x400 && c <= 0x40F) return c + 80;
	if (c == 0x490) return 0x491;
	return c;
}

static bool is_upper_cp(uint32_t c) { return lower(c) != c; }

static bool is_letter(uint32_t c) {
	c = lower(c);
	return (c >= 'a' && c <= 'z') || (c >= 0xDF && c <= 0xFF && c != 0xF7) ||
		(c >= 0x100 && c <= 0x17F) || (c >= 0x430 && c <= 0x45F) || c == 0x491;
}

static bool is_vowel(uint32_t c) {
	for (int i = 0; i < nvowels; i++) if (vowels[i] == c) return true;
	return false;
}

static bool matches(uint16_t e, uint32_t c) {
	if (e == E_ANY) return is_letter(c);
	if (e == E_VOWEL) return is_vowel(c);
	if (e == E_CONS) return is_letter(c) && !is_vowel(c);
	if (e >= E_SET && e < E_SET + MAXSET) {
		int s = e - E_SET;
		for (int i = 0; i < setn[s]; i++) if (sets[s][i] == c) return true;
		return false;
	}
	return e == c;
}

// -- the file --

static char *skip_sp(char *p) { while (*p == ' ' || *p == '\t') p++; return p; }

static void trim_end(char *p) {
	char *e = p + strlen(p);
	while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
}

// A context or match: elements into `out` (at most `max`); false if it
// does not parse.
static bool parse_elems(const char *s, uint32_t n, uint16_t *out, uint8_t *count, int max) {
	uint32_t i = 0;
	int k = 0;
	while (i < n) {
		if (k >= max) return false;
		char ch = s[i];
		if (ch == '#') { out[k++] = E_EDGE; i++; continue; }
		if (ch == 'V') { out[k++] = E_VOWEL; i++; continue; }
		if (ch == 'C') { out[k++] = E_CONS; i++; continue; }
		if (ch == '.') { out[k++] = E_ANY; i++; continue; }
		if (ch == '{') {
			if (nsets >= MAXSET) return false;
			i++;
			setn[nsets] = 0;
			while (i < n && s[i] != '}') {
				uint32_t c = next_cp(s, n, &i);
				if (setn[nsets] < SETN) sets[nsets][setn[nsets]++] = (uint16_t)c;
			}
			if (i < n) i++;
			out[k++] = (uint16_t)(E_SET + nsets++);
			continue;
		}
		out[k++] = (uint16_t)next_cp(s, n, &i);
	}
	*count = (uint8_t)k;
	return true;
}

static void add_rule(char *pat, char *out) {
	if (nrules >= MAXR) return;
	char *lb = strchr(pat, '['), *rb = lb ? strchr(lb, ']') : 0;
	if (!lb || !rb) return;
	rule_t *r = &rules[nrules];
	if (!parse_elems(pat, (uint32_t)(lb - pat), r->l, &r->ln, CTXN)) return;
	if (!parse_elems(lb + 1, (uint32_t)(rb - lb - 1), r->m, &r->mn, MATCHN) || !r->mn) return;
	if (!parse_elems(rb + 1, (uint32_t)strlen(rb + 1), r->r, &r->rn, CTXN)) return;
	r->out = out;
	if (r->m[0] < 0x80 && r->m[0] >= 'a') latin = true;
	// appended to its letter's chain, keeping the file's order
	int h = r->m[0] & 63;
	r->next = -1;
	if (head[h] < 0) head[h] = (int16_t)nrules;
	else {
		int j = head[h];
		while (rules[j].next >= 0) j = rules[j].next;
		rules[j].next = (int16_t)nrules;
	}
	nrules++;
}

static int to_int(const char *s) {
	int v = 0;
	while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
	return v;
}

static void parse(char *text) {

	char *line = text;
	while (line && *line) {
		char *nl = strchr(line, '\n');
		if (nl) *nl = 0;
		char *p = skip_sp(line);
		trim_end(p);
		line = nl ? nl + 1 : 0;
		if (!*p || *p == '#') continue;

		// "key rest" or "key arg = value"
		char *sp = p;
		while (*sp && *sp != ' ' && *sp != '\t') sp++;
		char *rest = *sp ? skip_sp(sp + 1) : sp;
		*sp = 0;
		char *eq = strstr(rest, " = ");
		char *val = 0, *arg = rest;
		if (rest[0] == '=' && rest[1] == ' ') { *rest = 0; val = skip_sp(rest + 2); }
		else if (eq) { *eq = 0; val = skip_sp(eq + 3); trim_end(arg); }

		if (!strcmp(p, "r") && val) add_rule(arg, val);
		else if (!strcmp(p, "name")) { strncpy(name, rest, sizeof(name) - 1); }
		else if (!strcmp(p, "stress")) {
			stress_rule = !strcmp(rest, "penult") ? ST_PENULT : !strcmp(rest, "last") ? ST_LAST :
				!strcmp(rest, "antepenult") ? ST_ANTEPENULT : ST_FIRST;
		}
		else if (!strcmp(p, "unstressed")) unstressed = rest[0] == '0' ? '0' : '2';
		else if (!strcmp(p, "vowels") || !strcmp(p, "last-if-ends")) {
			bool v = p[0] == 'v';
			uint32_t i = 0, n = (uint32_t)strlen(rest);
			while (i < n) {
				uint32_t c = next_cp(rest, n, &i);
				if (c == ' ') continue;
				if (v && nvowels < (int)(sizeof vowels / sizeof vowels[0])) vowels[nvowels++] = (uint16_t)c;
				if (!v && nlast_if_ends < (int)(sizeof last_if_ends / sizeof last_if_ends[0]))
					last_if_ends[nlast_if_ends++] = (uint16_t)c;
			}
		}
		else if (!strcmp(p, "prefixes") || !strcmp(p, "weak")) {
			bool pre = p[0] == 'p';
			char *q = rest;
			while (*q) {
				char *w = q;
				while (*q && *q != ' ') q++;
				if (*q) *q++ = 0;
				q = skip_sp(q);
				if (!*w) continue;
				if (pre && nprefixes < 16) prefixes[nprefixes++] = w;
				if (!pre && nwords < MAXW) { words[nwords].w = w; words[nwords].out = 0; nwords++; }
			}
		}
		else if (!strcmp(p, "word") && val && nwords < MAXW) {
			words[nwords].w = arg; words[nwords].out = val; nwords++;
		}
		else if (!strcmp(p, "num") && val) { int n = to_int(arg); if (n >= 0 && n < 100) num[n] = val; }
		else if (!strcmp(p, "hundred") && val) { int n = to_int(arg); if (n > 0 && n < 10) hundred[n] = val; }
		else if (!strcmp(p, "hundredx") && val) { int n = to_int(arg); if (n > 0 && n < 10) hundredx[n] = val; }
		else if (!strcmp(p, "thousand") && val) { int n = to_int(arg); if (n > 0 && n < 10) thousand[n] = val; }
		else if (!strcmp(p, "thousands") && val) {
			if (!strcmp(arg, "one")) thousands_one = val;
			else if (!strcmp(arg, "few")) thousands_few = val;
			else thousands_many = val;
		}
		else if (!strcmp(p, "minus") && val) w_minus = val;
		else if (!strcmp(p, "join") && val) w_join = val;
		else if (!strcmp(p, "dot") && val) w_dot = val;
		else if (!strcmp(p, "point") && val) w_point = val;
		else if (!strcmp(p, "percent") && val) w_percent = val;
		else if ((!strcmp(p, "letter") || !strcmp(p, "sym")) && val) {
			uint32_t i = 0;
			uint32_t c = next_cp(arg, (uint32_t)strlen(arg), &i);
			if (p[0] == 'l' && nletters < 48) { letter_cp[nletters] = (uint16_t)c; letter_w[nletters++] = val; }
			if (p[0] == 's' && nsyms < 24) { sym_cp[nsyms] = (uint16_t)c; sym_w[nsyms++] = val; }
		}
	}

	if (!thousands_one) thousands_one = thousands_many;
	if (!thousands_few) thousands_few = thousands_many;

}

static void reset(void) {
	active = false;
	nrules = nsets = nwords = nvowels = nprefixes = nletters = nsyms = nlast_if_ends = 0;
	for (int i = 0; i < 64; i++) head[i] = -1;
	memset(num, 0, sizeof num);
	memset(hundred, 0, sizeof hundred);
	memset(hundredx, 0, sizeof hundredx);
	memset(thousand, 0, sizeof thousand);
	thousands_one = thousands_few = thousands_many = 0;
	w_minus = w_point = w_percent = w_join = w_dot = 0;
	stress_rule = ST_FIRST;
	unstressed = '2';
	latin = false;
	name[0] = 0;
}

bool lang_load_text(char *text, uint32_t len) {
	reset();
	(void)len;
	parse(text);
	active = nrules > 0;
	return active;
}

bool lang_load(const char *c) {
	if (!c || !c[0] || !strcmp(c, "en")) { reset(); code[0] = 0; return false; }
	if (active && !strcmp(c, code)) return true;
	char path[64];
	int n = -1;
#ifdef PACK_HOST
	// from the tree's root, or from here (sw/apps/tts)
	snprintf(path, sizeof path, "sw/data/tts/lang/%s.ttl", c);
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(path, sizeof path, "../../data/tts/lang/%s.ttl", c);
		f = fopen(path, "rb");
	}
	if (f) { n = (int)fread(buf, 1, LANG_FILE_MAX, f); fclose(f); }
#else
	strcpy(path, Z_DIR_DATA "/tts/lang/");
	if (strlen(c) > 6) return false;
	strcat(path, c);
	strcat(path, ".ttl");
	n = fs_read_file(path, buf, LANG_FILE_MAX);
#endif
	reset();
	strncpy(code, c, sizeof(code) - 1);
	if (n <= 0) return false;
	buf[n] = 0;
	return lang_load_text(buf, (uint32_t)n);
}

// -- writing phonemes --

typedef struct { char *p; uint32_t cap, n; bool full; } sink_t;

static void put(sink_t *s, const char *t) {
	uint32_t l = (uint32_t)strlen(t);
	if (s->full || s->n + l + 2 >= s->cap) { s->full = true; return; }
	if (s->n) s->p[s->n++] = ' ';
	memcpy(s->p + s->n, t, l);
	s->n += l;
	s->p[s->n] = 0;
}

// The vowels phon.c knows: what a syllable is counted by.
static bool vowel_token(const char *t, int n) {
	static const char *const v[] = {
		"IY", "IH", "EH", "AE", "AA", "AO", "UH", "UW", "AH", "ER", "AX", "IX",
		"EY", "OW", "AY", "AW", "OY", "I", "E", "A", "O", "U", "UE", "OE", "YI",
		"AN", "EN", "ON", "UN",
	};
	for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++)
		if ((int)strlen(v[i]) == n && !memcmp(v[i], t, (size_t)n)) return true;
	return false;
}

#define WMAX   32		// letters in a word
#define TMAX   64		// phonemes in a word

// The phonemes of `w` (lowercase codepoints), stressed by the
// language's rule, into the sink. `weak`: every vowel unstressed.
static void g2p(sink_t *s, const uint16_t *w, int n, bool weak) {

	char tok[TMAX][4];
	int src[TMAX], nt = 0, forced = -1;

	for (int pos = 0; pos < n; ) {
		int ri = head[w[pos] & 63];
		const rule_t *hit = 0;
		for (; ri >= 0; ri = rules[ri].next) {
			const rule_t *r = &rules[ri];
			if (pos + r->mn > n) continue;
			int k;
			for (k = 0; k < r->mn; k++) if (r->m[k] != w[pos + k]) break;
			if (k < r->mn) continue;
			// left, right to left
			int j = pos - 1;
			for (k = r->ln - 1; k >= 0; k--) {
				if (r->l[k] == E_EDGE) { if (j >= 0) break; continue; }
				if (j < 0 || !matches(r->l[k], w[j])) break;
				j--;
			}
			if (k >= 0) continue;
			j = pos + r->mn;
			for (k = 0; k < r->rn; k++) {
				if (r->r[k] == E_EDGE) { if (j < n) break; continue; }
				if (j >= n || !matches(r->r[k], w[j])) break;
				j++;
			}
			if (k < r->rn) continue;
			hit = r;
			break;
		}
		if (!hit) { pos++; continue; }		// a letter no rule says: silent

		// its phonemes, one token at a time
		const char *o = hit->out;
		while (*o && nt < TMAX) {
			while (*o == ' ') o++;
			if (!*o || *o == '-') { if (*o) o++; continue; }	// "-": silent
			int l = 0;
			while (o[l] && o[l] != ' ' && o[l] != '\'' && l < 3) { tok[nt][l] = o[l]; l++; }
			tok[nt][l] = 0;
			o += l;
			while (*o && *o != ' ' && *o != '\'') o++;
			if (*o == '\'') { forced = nt; o++; }
			src[nt] = pos;
			nt++;
		}
		pos += hit->mn;
	}

	// which vowel is stressed
	int vi[TMAX], nv = 0;
	for (int t = 0; t < nt; t++)
		if (vowel_token(tok[t], (int)strlen(tok[t]))) vi[nv++] = t;

	int stressed = -1;
	if (!weak && nv) {
		if (forced >= 0) stressed = forced;
		else {
			int skip = 0;		// letters of an unstressed prefix
			if (stress_rule == ST_FIRST) {
				for (int p = 0; p < nprefixes; p++) {
					uint32_t i = 0, pl = (uint32_t)strlen(prefixes[p]);
					int k = 0;
					bool same = true;
					while (i < pl) {
						uint32_t c = next_cp(prefixes[p], pl, &i);
						if (k >= n || w[k] != c) { same = false; break; }
						k++;
					}
					if (same && k < n - 1 && k > skip) skip = k;
				}
			}
			int pick = 0;
			bool last_end = false;
			for (int i = 0; i < nlast_if_ends; i++) if (w[n - 1] == last_if_ends[i]) last_end = true;
			if (stress_rule == ST_LAST || last_end) pick = nv - 1;
			else if (stress_rule == ST_PENULT) pick = nv >= 2 ? nv - 2 : 0;
			else if (stress_rule == ST_ANTEPENULT) pick = nv >= 3 ? nv - 3 : nv >= 2 ? nv - 2 : 0;
			else {
				pick = 0;
				while (pick < nv - 1 && src[vi[pick]] < skip) pick++;
			}
			stressed = vi[pick];
		}
	}

	for (int t = 0; t < nt; t++) {
		char b[6];
		int l = (int)strlen(tok[t]);
		memcpy(b, tok[t], (size_t)l);
		if (vowel_token(tok[t], l)) b[l++] = t == stressed ? '1' : weak ? '0' : unstressed;
		b[l] = 0;
		put(s, b);
	}

}

// -- words --

static void say_words(sink_t *s, const char *text);

static bool same_word(const char *a, const uint16_t *w, int n) {
	uint32_t i = 0, l = (uint32_t)strlen(a);
	for (int k = 0; k < n; k++) {
		if (i >= l) return false;
		if (next_cp(a, l, &i) != w[k]) return false;
	}
	return i == l;
}

// A word of the language: lowercase codepoints.
static void say_word(sink_t *s, const uint16_t *w, int n) {
	for (int i = 0; i < nwords; i++) {
		if (!same_word(words[i].w, w, n)) continue;
		if (!words[i].out) { g2p(s, w, n, true); return; }	// weak
		put(s, words[i].out);
		return;
	}
	g2p(s, w, n, false);
}

static void say_letter(sink_t *s, uint32_t c) {
	c = lower(c);
	for (int i = 0; i < nletters; i++)
		if (letter_cp[i] == c) { say_words(s, letter_w[i]); return; }
	uint16_t w = (uint16_t)c;
	g2p(s, &w, 1, false);
}

// Words written in the file -- a number's, a letter's name -- said.
static void say_words(sink_t *s, const char *text) {
	uint32_t n = (uint32_t)strlen(text), i = 0;
	while (i < n) {
		uint16_t w[WMAX];
		int k = 0;
		while (i < n) {
			uint32_t at = i;
			uint32_t c = next_cp(text, n, &i);
			(void)at;
			if ((c == '\'' || c == 0x2019) && k && k < WMAX) { w[k++] = '\''; continue; }
			if (!is_letter(c)) { if (k) break; continue; }
			if (k < WMAX) w[k++] = (uint16_t)lower(c);
		}
		if (k) say_word(s, w, k);
	}
}

static void say_number(sink_t *s, uint32_t v) {
	if (v < 100) { if (num[v]) say_words(s, num[v]); return; }
	if (v < 1000) {
		uint32_t h = v / 100, r = v % 100;
		const char *hw = r && hundredx[h] ? hundredx[h] : hundred[h];
		if (hw) say_words(s, hw); else { say_number(s, h); if (hundred[1]) say_words(s, hundred[1]); }
		if (r && w_join) say_words(s, w_join);	// Portuguese: "cento e vinte"
		if (r) say_number(s, r);
		return;
	}
	if (v < 1000000) {
		uint32_t t = v / 1000, r = v % 1000;
		if (t < 10 && thousand[t]) say_words(s, thousand[t]);
		else {
			say_number(s, t);
			uint32_t d = t % 10, dd = t % 100;
			const char *f = (d == 1 && dd != 11) ? thousands_one :
				(d >= 2 && d <= 4 && (dd < 12 || dd > 14)) ? thousands_few : thousands_many;
			if (f) say_words(s, f);
		}
		if (r && w_join && (r < 100 || r % 100 == 0)) say_words(s, w_join);	// "mil e vinte"
		if (r) say_number(s, r);
		return;
	}
	// past a million: the digits
	char d[12];
	int n = 0;
	while (v && n < 12) { d[n++] = (char)(v % 10); v /= 10; }
	while (n) say_number(s, (uint32_t)d[--n]);
}

static bool is_space(uint32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0xA0; }
static bool is_digit(uint32_t c) { return c >= '0' && c <= '9'; }

// A word the rules do not cover (Latin letters in a Cyrillic language):
// the English front end says it.
static void say_english(sink_t *s, const char *text, uint32_t n) {
	char t[64], ph[256];
	if (n >= sizeof t) n = sizeof t - 1;
	memcpy(t, text, n);
	t[n] = 0;
	uint32_t pos = 0;
	bool more = true;
	while (more && text2ph_chunk(t, n, &pos, false, ph, sizeof ph, &more)) {
		// the English front end's pauses are not this text's
		char *q = ph;
		while (*q) { if (*q == '.' || *q == ',' || *q == '?' || *q == '|') *q = ' '; q++; }
		put(s, ph);
	}
}

bool lang_chunk(const char *text, uint32_t len, uint32_t *pos, bool spell,
	char *out, uint32_t outsize, bool *more) {

	sink_t s = { out, outsize, 0, false };
	if (outsize) out[0] = 0;
	uint32_t i = *pos;

	while (i < len && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) i++;
	if (i >= len) { *pos = len; *more = false; return false; }

	// A single character on its own is a name: a key echo.
	{
		uint32_t k = i, hi = len;
		while (hi > k && is_space((uint8_t)text[hi - 1])) hi--;
		next_cp(text, len, &k);
		if (k == hi) spell = true;
	}

	int nw = 0;
	while (i < len && nw < 12 && !s.full) {

		uint32_t at = i;
		uint32_t c = next_cp(text, len, &i);

		if (spell) {
			if (is_letter(c)) say_letter(&s, c);
			else if (is_digit(c)) say_number(&s, c - '0');
			else for (int k = 0; k < nsyms; k++) if (sym_cp[k] == c) say_words(&s, sym_w[k]);
			nw++;
			continue;
		}

		if (is_space(c)) {
			if (c == '\n') {
				uint32_t k = i;
				while (k < len && (text[k] == ' ' || text[k] == '\r')) k++;
				put(&s, (k < len && text[k] == '\n') ? "|" : ",");
			}
			continue;
		}

		if (is_letter(c)) {
			// the word: letters, and an apostrophe between two
			uint16_t w[WMAX];
			int n = 0, upper = 0;
			bool ascii = true;
			uint32_t st = at, en = i;
			uint32_t cc = c;
			for (;;) {
				if (n < WMAX) w[n++] = (uint16_t)lower(cc);
				if (is_upper_cp(cc)) upper++;
				if (cc >= 0x80) ascii = false;
				en = i;
				if (i >= len) break;
				uint32_t j = i;
				uint32_t d = next_cp(text, len, &j);
				if ((d == '\'' || d == 0x2019) && j < len) {
					// kept as a letter of the word, for the rules that
					// read it (Ukrainian п'ять: the я after it is "ya")
					uint32_t j2 = j;
					if (is_letter(next_cp(text, len, &j2))) {
						if (n < WMAX) w[n++] = '\'';
						cc = next_cp(text, len, &j);
						i = j;
						continue;
					}
				}
				if (!is_letter(d)) break;
				cc = d;
				i = j;
			}
			nw++;
			if (ascii && !latin) { say_english(&s, text + st, en - st); continue; }
			bool vowel = false;
			for (int k = 0; k < n; k++) if (is_vowel(w[k])) vowel = true;
			if ((upper == n && n >= 2 && n <= 5) || (!vowel && n >= 2)) {
				// an abbreviation (USB), or no vowel to say (txt): spelled
				for (int k = 0; k < n; k++) say_letter(&s, w[k]);
				continue;
			}
			say_word(&s, w, n);
			continue;
		}

		if (is_digit(c) || (c == '-' && i < len && is_digit((uint8_t)text[i]))) {
			if (c == '-') { if (w_minus) say_words(&s, w_minus); c = (uint8_t)text[i++]; }
			uint32_t v = c - '0';
			int digits = 1;
			bool big = false;
			while (i < len) {
				char d = text[i];
				if (is_digit((uint8_t)d)) { if (v < 100000000u) v = v * 10 + (uint32_t)(d - '0'); else big = true; digits++; i++; continue; }
				// a thousands separator: three digits follow
				if ((d == ',' || d == '.' || d == ' ') && i + 3 < len &&
					is_digit((uint8_t)text[i + 1]) && is_digit((uint8_t)text[i + 2]) &&
					is_digit((uint8_t)text[i + 3]) && (i + 4 >= len || !is_digit((uint8_t)text[i + 4]))) {
					i++;
					continue;
				}
				break;
			}
			if (big || (digits > 1 && text[at] == '0')) {
				// a serial number, a code: digit by digit
				for (uint32_t k = at; k < i; k++)
					if (is_digit((uint8_t)text[k])) say_number(&s, (uint32_t)(text[k] - '0'));
			} else say_number(&s, v);
			if (i + 1 < len && (text[i] == '.' || text[i] == ',') && is_digit((uint8_t)text[i + 1])) {
				if (w_point) say_words(&s, w_point);
				i++;
				while (i < len && is_digit((uint8_t)text[i])) say_number(&s, (uint32_t)(text[i++] - '0'));
			}
			if (i < len && text[i] == '%') { if (w_percent) say_words(&s, w_percent); i++; }
			nw += digits / 3 + 1;
			continue;
		}

		// A dot between two letters or digits is in a name: "dot"
		// (report.txt, example.com), not the end of a sentence.
		if (c == '.' && at > 0 && i < len && is_letter((uint8_t)text[i]) &&
			(is_letter((uint8_t)text[at - 1]) || is_digit((uint8_t)text[at - 1]))) {
			if (w_dot) say_words(&s, w_dot);
			continue;
		}
		// punctuation: pauses, as the English front end makes them; a
		// run of dots is one
		if (c == '.' || c == '!' || c == 0x2026) {
			while (i < len && (text[i] == '.' || text[i] == '!')) i++;
			put(&s, ".");
			break;
		}
		if (c == '?') { put(&s, "?"); break; }
		if (c == ',' || c == ';' || c == ':') { put(&s, ","); continue; }
		for (int k = 0; k < nsyms; k++) if (sym_cp[k] == c) { say_words(&s, sym_w[k]); break; }
	}

	*pos = i;
	while (*pos < len && is_space((uint8_t)text[*pos])) (*pos)++;
	*more = *pos < len;
	return true;

}
