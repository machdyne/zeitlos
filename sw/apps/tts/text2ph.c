/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Text to phonemes: the front end. See text2ph.h and docs/tts.md,
 * "Text".
 *
 * A word is looked up in the exception dictionary below, and failing
 * that pronounced by the letter-to-sound rules (lts.c) -- except that
 * an acronym ("USB", "CPU") or a word with no vowels ("mkdir") is
 * spelled, since that is how people say those. camelCase and
 * letter/digit runs are split first ("fileName" -> file name, "mp3"
 * -> M P three). Numbers are read as numbers up to the trillions,
 * with decimals, signs and percent. Symbols that carry meaning in
 * running text (@ & + = / % ...) are named; brackets and quotes are
 * not. A lone character, and anything with Z_TTS_F_SPELL, is spelled
 * with every symbol named -- which is what a key echo or a cursor
 * landing on punctuation wants.
 *
 * Pronunciations are this project's own transcriptions.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "text2ph.h"
#include "lts.h"
#include "pack.h"

static const char *const letters[26] = {
	"EY1", "B IY1", "S IY1", "D IY1", "IY1", "EH1 F", "JH IY1", "EY1 CH",
	"AY1", "JH EY1", "K EY1", "EH1 L", "EH1 M", "EH1 N", "OW1", "P IY1",
	"K Y UW1", "AA1 R", "EH1 S", "T IY1", "Y UW1", "V IY1",
	"D AH1 B AX0 L Y UW0", "EH1 K S", "W AY1", "Z IY1",
};

static const char *const ones[20] = {
	"Z IY1 R OW0", "W AH1 N", "T UW1", "TH R IY1", "F AO1 R",
	"F AY1 V", "S IH1 K S", "S EH1 V AX0 N", "EY1 T", "N AY1 N",
	"T EH1 N", "IX0 L EH1 V AX0 N", "T W EH1 L V", "TH ER1 T IY2 N", "F AO1 R T IY2 N",
	"F IH1 F T IY2 N", "S IH1 K S T IY2 N", "S EH1 V AX0 N T IY2 N", "EY1 T IY2 N", "N AY1 N T IY2 N",
};

static const char *const tens[10] = {
	0, 0, "T W EH1 N T IY0", "TH ER1 D IY0", "F AO1 R T IY0",
	"F IH1 F T IY0", "S IH1 K S T IY0", "S EH1 V AX0 N T IY0", "EY1 T IY0", "N AY1 N T IY0",
};

// Symbols: the name, and whether it is spoken in running text (as
// opposed to only when spelled or on its own).
typedef struct { char c; bool inline_; const char *ph; } sym_t;

static const sym_t syms[] = {
	{ '@',  true,  "AE1 T" },
	{ '&',  true,  "AE1 N D" },
	{ '+',  true,  "P L AH1 S" },
	{ '=',  true,  "IY1 K W AX0 L Z" },
	{ '%',  true,  "P ER0 S EH1 N T" },
	{ '$',  true,  "D AA1 L ER0" },
	{ '#',  true,  "HH AE1 SH" },
	{ '*',  true,  "S T AA1 R" },
	{ '/',  true,  "S L AE1 SH" },
	{ '\\', true,  "B AE1 K S L AE2 SH" },
	{ '|',  true,  "B AA1 R" },
	{ '~',  true,  "T IH1 L D AX0" },
	{ '^',  true,  "K AE1 R AX0 T" },
	{ '<',  true,  "L EH1 S _ DH AE1 N" },
	{ '>',  true,  "G R EY1 T ER0 _ DH AE1 N" },
	{ '_',  false, "AH1 N D ER0 S K AO2 R" },
	{ '-',  false, "D AE1 SH" },
	{ '.',  false, "D AA1 T" },
	{ ',',  false, "K AA1 M AX0" },
	{ ':',  false, "K OW1 L AX0 N" },
	{ ';',  false, "S EH1 M IY0 K OW2 L AX0 N" },
	{ '?',  false, "K W EH1 S CH AX0 N" },
	{ '!',  false, "B AE1 NG" },
	{ '"',  false, "K W OW1 T" },
	{ '\'', false, "AX0 P AA1 S T R AX0 F IY0" },
	{ '`',  false, "B AE1 K T IH2 K" },
	{ '(',  false, "L EH1 F T _ P ER0 EH1 N" },
	{ ')',  false, "R AY1 T _ P ER0 EH1 N" },
	{ '[',  false, "L EH1 F T _ B R AE1 K AX0 T" },
	{ ']',  false, "R AY1 T _ B R AE1 K AX0 T" },
	{ '{',  false, "L EH1 F T _ B R EY1 S" },
	{ '}',  false, "R AY1 T _ B R EY1 S" },
	{ ' ',  false, "S P EY1 S" },
};

static const sym_t *sym_of(char c) {
	for (unsigned i = 0; i < sizeof(syms) / sizeof(syms[0]); i++)
		if (syms[i].c == c) return &syms[i];
	return 0;
}

// The exception dictionary: words the rules get wrong, and interface
// words that must be exact. Sorted, for the binary search. Keep it
// sorted.
static const char *const dict[][2] = {
	{ "a",            "AX0" },
	{ "about",        "AX0 B AW1 T" },
	{ "above",        "AX0 B AH1 V" },
	{ "after",        "AE1 F T ER0" },
	{ "again",        "AX0 G EH1 N" },
	{ "also",         "AO1 L S OW0" },
	{ "alt",          "AO1 L T" },
	{ "always",       "AO1 L W EY2 Z" },
	{ "an",           "AE1 N" },
	{ "and",          "AE1 N D" },
	{ "answer",       "AE1 N S ER0" },
	{ "any",          "EH1 N IY0" },
	{ "app",          "AE1 P" },
	{ "are",          "AA1 R" },
	{ "arrow",        "AE1 R OW0" },
	{ "as",           "AE1 Z" },
	{ "at",           "AE1 T" },
	{ "audio",        "AO1 D IY0 OW0" },
	{ "backspace",    "B AE1 K S P EY2 S" },
	{ "be",           "B IY1" },
	{ "because",      "B IX0 K AO1 Z" },
	{ "been",         "B IH1 N" },
	{ "before",       "B IX0 F AO1 R" },
	{ "both",         "B OW1 TH" },
	{ "break",        "B R EY1 K" },
	{ "browser",      "B R AW1 Z ER0" },
	{ "build",        "B IH1 L D" },
	{ "built",        "B IH1 L T" },
	{ "busy",         "B IH1 Z IY0" },
	{ "button",       "B AH1 T AX0 N" },
	{ "by",           "B AY1" },
	{ "calculator",   "K AE1 L K Y AX0 L EY2 T ER0" },
	{ "calendar",     "K AE1 L AX0 N D ER0" },
	{ "cancel",       "K AE1 N S AX0 L" },
	{ "check",        "CH EH1 K" },
	{ "clipboard",    "K L IH1 P B AO2 R D" },
	{ "close",        "K L OW1 Z" },
	{ "come",         "K AH1 M" },
	{ "control",      "K AX0 N T R OW1 L" },
	{ "copy",         "K AA1 P IY0" },
	{ "could",        "K UH1 D" },
	{ "ctrl",         "K AX0 N T R OW1 L" },
	{ "cursor",       "K ER1 S ER0" },
	{ "delete",       "D IX0 L IY1 T" },
	{ "desktop",      "D EH1 S K T AA2 P" },
	{ "dialog",       "D AY1 AX0 L AO2 G" },
	{ "directory",    "D ER0 EH1 K T ER0 IY0" },
	{ "do",           "D UW1" },
	{ "dock",         "D AA1 K" },
	{ "document",     "D AA1 K Y AX0 M AX0 N T" },
	{ "does",         "D AH1 Z" },
	{ "done",         "D AH1 N" },
	{ "down",         "D AW1 N" },
	{ "editor",       "EH1 D IX0 T ER0" },
	{ "eight",        "EY1 T" },
	{ "email",        "IY1 M EY2 L" },
	{ "empty",        "EH1 M P T IY0" },
	{ "enough",       "IX0 N AH1 F" },
	{ "enter",        "EH1 N T ER0" },
	{ "error",        "EH1 R ER0" },
	{ "escape",       "IX0 S K EY1 P" },
	{ "ever",         "EH1 V ER0" },
	{ "every",        "EH1 V R IY0" },
	{ "eye",          "AY1" },
	{ "file",         "F AY1 L" },
	{ "focused",      "F OW1 K AX0 S T" },
	{ "folder",       "F OW1 L D ER0" },
	{ "four",         "F AO1 R" },
	{ "friend",       "F R EH1 N D" },
	{ "from",         "F R AH1 M" },
	{ "get",          "G EH1 T" },
	{ "give",         "G IH1 V" },
	{ "go",           "G OW1" },
	{ "gone",         "G AO1 N" },
	{ "great",        "G R EY1 T" },
	{ "has",          "HH AE1 Z" },
	{ "have",         "HH AE1 V" },
	{ "he",           "HH IY1" },
	{ "hello",        "HH AX0 L OW1" },
	{ "her",          "HH ER1" },
	{ "here",         "HH IH1 R" },
	{ "his",          "HH IH1 Z" },
	{ "home",         "HH OW1 M" },
	{ "how",          "HH AW1" },
	{ "i",            "AY1" },
	{ "image",        "IH1 M IX0 JH" },
	{ "info",         "IH1 N F OW0" },
	{ "insert",       "IH0 N S ER1 T" },
	{ "into",         "IH1 N T UW0" },
	{ "is",           "IH1 Z" },
	{ "island",       "AY1 L AX0 N D" },
	{ "it",           "IH1 T" },
	{ "its",          "IH1 T S" },
	{ "key",          "K IY1" },
	{ "keyboard",     "K IY1 B AO2 R D" },
	{ "know",         "N OW1" },
	{ "known",        "N OW1 N" },
	{ "laugh",        "L AE1 F" },
	{ "linux",        "L IH1 N AX0 K S" },
	{ "live",         "L IH1 V" },
	{ "love",         "L AH1 V" },
	{ "many",         "M EH1 N IY0" },
	{ "me",           "M IY1" },
	{ "menu",         "M EH1 N Y UW0" },
	{ "move",         "M UW1 V" },
	{ "music",        "M Y UW1 Z IX0 K" },
	{ "my",           "M AY1" },
	{ "never",        "N EH1 V ER0" },
	{ "new",          "N UW1" },
	{ "no",           "N OW1" },
	{ "not",          "N AA1 T" },
	{ "now",          "N AW1" },
	{ "of",           "AH1 V" },
	{ "off",          "AO1 F" },
	{ "ok",           "OW2 K EY1" },
	{ "okay",         "OW2 K EY1" },
	{ "on",           "AA1 N" },
	{ "once",         "W AH1 N S" },
	{ "one",          "W AH1 N" },
	{ "only",         "OW1 N L IY0" },
	{ "open",         "OW1 P AX0 N" },
	{ "or",           "AO1 R" },
	{ "other",        "AH1 DH ER0" },
	{ "our",          "AW1 ER0" },
	{ "over",         "OW1 V ER0" },
	{ "own",          "OW1 N" },
	{ "page",         "P EY1 JH" },
	{ "paste",        "P EY1 S T" },
	{ "people",       "P IY1 P AX0 L" },
	{ "player",       "P L EY1 ER0" },
	{ "put",          "P UH1 T" },
	{ "read",         "R IY1 D" },
	{ "readable",     "R IY1 D AX0 B AX0 L" },
	{ "reader",       "R IY1 D ER0" },
	{ "rough",        "R AH1 F" },
	{ "said",         "S EH1 D" },
	{ "save",         "S EY1 V" },
	{ "says",         "S EH1 Z" },
	{ "screen",       "S K R IY1 N" },
	{ "select",       "S AX0 L EH1 K T" },
	{ "selected",     "S AX0 L EH1 K T IX0 D" },
	{ "settings",     "S EH1 T IX0 NG Z" },
	{ "shall",        "SH AE1 L" },
	{ "she",          "SH IY1" },
	{ "shift",        "SH IH1 F T" },
	{ "should",       "SH UH1 D" },
	{ "show",         "SH OW1" },
	{ "shown",        "SH OW1 N" },
	{ "so",           "S OW1" },
	{ "some",         "S AH1 M" },
	{ "sound",        "S AW1 N D" },
	{ "space",        "S P EY1 S" },
	{ "speech",       "S P IY1 CH" },
	{ "spreadsheet",  "S P R EH1 D SH IY2 T" },
	{ "sugar",        "SH UH1 G ER0" },
	{ "super",        "S UW1 P ER0" },
	{ "sure",         "SH UH1 R" },
	{ "system",       "S IH1 S T AX0 M" },
	{ "tab",          "T AE1 B" },
	{ "terminal",     "T ER1 M IX0 N AX0 L" },
	{ "text",         "T EH1 K S T" },
	{ "than",         "DH AE1 N" },
	{ "that",         "DH AE1 T" },
	{ "the",          "DH AX0" },
	{ "their",        "DH EH1 R" },
	{ "them",         "DH EH1 M" },
	{ "then",         "DH EH1 N" },
	{ "there",        "DH EH1 R" },
	{ "these",        "DH IY1 Z" },
	{ "they",         "DH EY1" },
	{ "this",         "DH IH1 S" },
	{ "those",        "DH OW1 Z" },
	{ "though",       "DH OW1" },
	{ "thought",      "TH AO1 T" },
	{ "through",      "TH R UW1" },
	{ "to",           "T UW0" },
	{ "today",        "T AX0 D EY1" },
	{ "tough",        "T AH1 F" },
	{ "two",          "T UW1" },
	{ "under",        "AH1 N D ER0" },
	{ "untitled",     "AH0 N T AY1 T AX0 L D" },
	{ "up",           "AH1 P" },
	{ "upon",         "AX0 P AA1 N" },
	{ "use",          "Y UW1 Z" },
	{ "used",         "Y UW1 Z D" },
	{ "user",         "Y UW1 Z ER0" },
	{ "very",         "V EH1 R IY0" },
	{ "viewer",       "V Y UW1 ER0" },
	{ "volume",       "V AA1 L Y UW0 M" },
	{ "was",          "W AA1 Z" },
	{ "water",        "W AO1 T ER0" },
	{ "we",           "W IY1" },
	{ "web",          "W EH1 B" },
	{ "were",         "W ER1" },
	{ "what",         "W AH1 T" },
	{ "where",        "W EH1 R" },
	{ "who",          "HH UW1" },
	{ "whole",        "HH OW1 L" },
	{ "whom",         "HH UW1 M" },
	{ "whose",        "HH UW1 Z" },
	{ "why",          "W AY1" },
	{ "window",       "W IH1 N D OW0" },
	{ "with",         "W IH1 DH" },
	{ "woman",        "W UH1 M AX0 N" },
	{ "women",        "W IH1 M IX0 N" },
	{ "word",         "W ER1 D" },
	{ "work",         "W ER1 K" },
	{ "world",        "W ER1 L D" },
	{ "would",        "W UH1 D" },
	{ "yes",          "Y EH1 S" },
	{ "you",          "Y UW1" },
	{ "your",         "Y AO1 R" },
	{ "zeitlos",      "Z AY1 T L OW2 S" },
};

#define NDICT (int)(sizeof(dict) / sizeof(dict[0]))

static const char *lookup(const char *w) {
	int lo = 0, hi = NDICT - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		int c = strcmp(w, dict[mid][0]);
		if (c == 0) return dict[mid][1];
		if (c < 0) hi = mid - 1; else lo = mid + 1;
	}
	return 0;
}

// -- output --

typedef struct { char *p; uint32_t left; bool full; } sink_t;

static void emit(sink_t *s, const char *str) {
	uint32_t n = (uint32_t)strlen(str);
	if (s->full || n + 2 > s->left) { s->full = true; return; }
	memcpy(s->p, str, n);
	s->p[n] = ' ';
	s->p += n + 1;
	s->left -= n + 1;
	*s->p = 0;
}

static bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
static bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
static bool is_alpha(char c) { return is_lower(c) || is_upper(c); }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Any character, by name.
static void spell_char(sink_t *s, char c) {
	if (is_upper(c)) c += 32;
	if (is_lower(c)) emit(s, letters[c - 'a']);
	else if (is_digit(c)) emit(s, ones[c - '0']);
	else { const sym_t *y = sym_of(c); if (y) emit(s, y->ph); else return; }
	emit(s, "_");
}

// -- numbers --

static void say_below_1000(sink_t *s, uint32_t n) {
	if (n >= 100) {
		emit(s, ones[n / 100]);
		emit(s, "HH AH1 N D R IX0 D");
		n %= 100;
		if (!n) return;
	}
	if (n >= 20) {
		emit(s, tens[n / 10]);
		if (n % 10) emit(s, ones[n % 10]);
	} else {
		emit(s, ones[n]);
	}
}

// `d` is a run of `n` digits. Up to 12 digits are read as a number
// ("four thousand and ninety six" style, without the "and"); longer,
// or with a leading zero, they are read one at a time -- a serial
// number or a phone number is heard that way.
static void say_digits(sink_t *s, const char *d, uint32_t n) {

	if (n > 12 || (n > 1 && d[0] == '0')) {
		for (uint32_t i = 0; i < n; i++) emit(s, ones[d[i] - '0']);
		return;
	}

	static const char *const scale[4] = {
		0, "TH AW1 Z AX0 N D", "M IH1 L Y AX0 N", "B IH1 L Y AX0 N",
	};

	uint32_t groups[4] = { 0, 0, 0, 0 };
	int ng = 0;
	for (int end = (int)n; end > 0 && ng < 4; end -= 3) {
		int start = end - 3 < 0 ? 0 : end - 3;
		uint32_t v = 0;
		for (int i = start; i < end; i++) v = v * 10 + (uint32_t)(d[i] - '0');
		groups[ng++] = v;
	}

	bool any = false;
	for (int g = ng - 1; g >= 0; g--) {
		if (!groups[g]) continue;
		say_below_1000(s, groups[g]);
		if (scale[g]) emit(s, scale[g]);
		any = true;
	}
	if (!any) emit(s, ones[0]);

}

// Function words: the small grammatical words English says quickly and
// without stress. Left stressed, they make a sentence sound like a
// list of equally important words, which is most of what makes
// rule-based speech tiring over a paragraph.
//
// Sorted; a word here has every stress digit forced to 0, which both
// flattens its pitch and shortens it (phon.c gives an unstressed vowel
// 55% of its duration).
static const char *const function_words[] = {
	"a", "am", "an", "and", "any", "are", "as", "at", "be", "been",
	"but", "by", "can", "could", "did", "do", "does", "for", "from",
	"had", "has", "have", "he", "her", "him", "his", "how", "i", "if",
	"in", "into", "is", "it", "its", "may", "might", "must", "my",
	"nor", "not", "of", "on", "or", "our", "shall", "she", "should",
	"so", "some", "than", "that", "the", "their", "them", "then",
	"there", "these", "they", "this", "those", "to", "up", "upon",
	"us", "was", "we", "were", "what", "when", "which", "who", "will",
	"with", "would", "you", "your",
};

static bool is_function_word(const char *w) {
	int lo = 0, hi = (int)(sizeof(function_words) / sizeof(function_words[0])) - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		int c = strcmp(w, function_words[mid]);
		if (c == 0) return true;
		if (c < 0) hi = mid - 1; else lo = mid + 1;
	}
	return false;
}

// Flattens every stress digit in a phoneme string.
static void unstress(char *ph) {
	for (; *ph; ph++)
		if (*ph == '1' || *ph == '2') *ph = '0';
}

// -- homographs --
//
// Words spelled the same and said differently. A lexicon cannot
// settle them: it has one entry per spelling, and the right answer
// depends on how the word is being used.
//
//   "the RECord" / "to reCORD"      noun and verb, stress moves
//   "I read it" / "I have read it"  present and past
//   "a lead pipe" / "to lead on"    two different vowels
//
// The choice is made from the word BEFORE, which is where English
// puts most of the evidence: a determiner ("the", "a", "his") means a
// noun is coming; "to" or an auxiliary means a verb is. Wrong
// sometimes -- a full part-of-speech tagger would do better -- but a
// sentence with the right word said the wrong way is far more
// jarring than one said a little flatly, and this catches the common
// cases.
typedef struct {
	const char *word;
	const char *as_noun;
	const char *as_verb;
} homograph_t;

static const homograph_t homographs[] = {
	{ "conduct",  "K AA1 N D AH0 K T",   "K AX0 N D AH1 K T" },
	{ "conflict", "K AA1 N F L IH0 K T", "K AX0 N F L IH1 K T" },
	{ "content",  "K AA1 N T EH0 N T",   "K AX0 N T EH1 N T" },
	{ "contract", "K AA1 N T R AE0 K T", "K AX0 N T R AE1 K T" },
	{ "convert",  "K AA1 N V ER0 T",     "K AX0 N V ER1 T" },
	{ "desert",   "D EH1 Z ER0 T",       "D IH0 Z ER1 T" },
	{ "object",   "AA1 B JH EH0 K T",    "AX0 B JH EH1 K T" },
	{ "permit",   "P ER1 M IH0 T",       "P ER0 M IH1 T" },
	{ "present",  "P R EH1 Z AX0 N T",   "P R IH0 Z EH1 N T" },
	{ "produce",  "P R OW1 D UW0 S",     "P R AX0 D UW1 S" },
	{ "progress", "P R AA1 G R EH0 S",   "P R AX0 G R EH1 S" },
	{ "project",  "P R AA1 JH EH0 K T",  "P R AX0 JH EH1 K T" },
	{ "protest",  "P R OW1 T EH0 S T",   "P R AX0 T EH1 S T" },
	{ "rebel",    "R EH1 B AX0 L",       "R IH0 B EH1 L" },
	{ "record",   "R EH1 K ER0 D",       "R IH0 K AO1 R D" },
	{ "refuse",   "R EH1 F Y UW0 S",     "R IH0 F Y UW1 Z" },
	{ "subject",  "S AH1 B JH EH0 K T",  "S AX0 B JH EH1 K T" },
	{ "suspect",  "S AH1 S P EH0 K T",   "S AX0 S P EH1 K T" },
};

// Words that say "a noun is coming" and words that say "a verb is".
static const char *const noun_before[] = {
	"a", "an", "another", "any", "each", "every", "her", "his", "its",
	"my", "no", "our", "some", "that", "the", "their", "this", "those",
	"your",
};
static const char *const verb_before[] = {
	"and", "can", "cannot", "could", "d", "didn't", "do", "does",
	"i", "let", "may", "might", "must", "shall", "should", "they",
	"to", "we", "will", "won't", "would", "you",
};

static bool in_list(const char *const *list, unsigned n, const char *w) {
	for (unsigned i = 0; i < n; i++)
		if (strcmp(list[i], w) == 0) return true;
	return false;
}

// The word before the one being said, lowercase. Reset per chunk.
static char prev_word[24];

static const char *homograph(const char *w) {

	for (unsigned i = 0; i < sizeof(homographs) / sizeof(homographs[0]); i++) {

		if (strcmp(homographs[i].word, w) != 0) continue;

		if (in_list(noun_before, sizeof(noun_before) / sizeof(noun_before[0]), prev_word))
			return homographs[i].as_noun;
		if (in_list(verb_before, sizeof(verb_before) / sizeof(verb_before[0]), prev_word))
			return homographs[i].as_verb;

		// No evidence: the noun reading. These words are nouns more
		// often than verbs in ordinary prose, and a noun said as a
		// verb sounds like a mistake while the reverse often passes.
		return homographs[i].as_noun;

	}

	return 0;

}

// "read" and "lead" are not stress pairs but vowel changes, and the
// evidence for them is different: a perfect auxiliary before "read"
// makes it past, and anything else makes it present.
static const char *tense_homograph(const char *w) {

	static const char *const perfect[] = { "have", "has", "had", "having" };

	if (strcmp(w, "read") == 0) {
		for (unsigned i = 0; i < 4; i++)
			if (strcmp(perfect[i], prev_word) == 0) return "R EH1 D";
		return "R IY1 D";
	}

	if (strcmp(w, "lead") == 0) {
		// The metal, when a determiner is in front of it.
		if (in_list(noun_before, sizeof(noun_before) / sizeof(noun_before[0]), prev_word))
			return "L EH1 D";
		return "L IY1 D";
	}

	return 0;

}

// -- words --

// `w` is `n` letters, case as written.
static void say_word(sink_t *s, const char *w, uint32_t n) {

	char lw[LTS_WORD_MAX + 1];
	if (n > LTS_WORD_MAX) n = LTS_WORD_MAX;

	bool all_upper = true, has_vowel = false;
	for (uint32_t i = 0; i < n; i++) {
		char c = w[i];
		if (!is_upper(c)) all_upper = false;
		lw[i] = is_upper(c) ? (char)(c + 32) : c;
		if (strchr("aeiouy", lw[i])) has_vowel = true;
	}
	lw[n] = 0;

	// The built-in dictionary first: it is small, curated, and holds
	// the interface words that have to be exactly right. Then the
	// speech pack on the card, which knows about 400,000 words. Then
	// the rules, which know every word and are right about 40% of the
	// time.
	// Homographs first: the dictionary and the pack each hold one
	// pronunciation, and for these that is the wrong question.
	const char *hg = homograph(lw);
	if (!hg) hg = tense_homograph(lw);
	if (hg) {
		emit(s, hg);
		if (n < sizeof(prev_word)) { memcpy(prev_word, lw, n); prev_word[n] = 0; }
		return;
	}

	const char *ph = lookup(lw);
	if (ph) {
		if (is_function_word(lw)) {
			char flat[64];
			uint32_t i = 0;
			while (ph[i] && i < sizeof(flat) - 1) { flat[i] = ph[i]; i++; }
			flat[i] = 0;
			unstress(flat);
			emit(s, flat);
		} else {
			emit(s, ph);
		}
		return;
	}

	char packed[256];
	if (pack_ready() && pack_lookup(lw, packed, sizeof(packed))) {
		if (is_function_word(lw)) unstress(packed);
		emit(s, packed);
		return;
	}

	// Acronyms, vowelless words and lone letters are spelled; "I" and
	// "a" are words, and the dictionary has already caught them.
	if ((all_upper && n >= 2 && n <= 5) || !has_vowel || n == 1) {
		for (uint32_t i = 0; i < n; i++) spell_char(s, lw[i]);
		return;
	}

	// The pack's trained rules if there are any, then the built-in
	// ones. The trained rules get about 46% of unknown words exactly
	// right against the 20% the built-in ones manage, so this order
	// is the whole point of shipping them.
	char out[256];
	if (pack_lts_ready() && pack_lts(lw, out, sizeof(out))) { emit(s, out); return; }
	if (lts_word(lw, out, sizeof(out))) emit(s, out);

}

// -- the chunker --

bool text2ph_chunk(const char *text, uint32_t len, uint32_t *pos, bool spell,
	char *out, uint32_t outsize, bool *more) {

	sink_t s = { out, outsize, false };
	if (outsize) out[0] = 0;
	uint32_t i = *pos;

	// A chunk is a sentence or so; nothing before it is evidence.
	prev_word[0] = 0;

	while (i < len && is_space(text[i])) i++;
	if (i >= len) { *pos = len; *more = false; return false; }

	// Phonemes, verbatim: "[HH AX0 L OW1]".
	if (text[i] == '[' && i == 0 && len > 1 && text[len - 1] == ']') {
		uint32_t n = len - 2;
		if (n + 1 > outsize) n = outsize - 1;
		memcpy(out, &text[1], n);
		out[n] = 0;
		*pos = len;
		*more = false;
		return true;
	}

	// A single character on its own is a name, always: a key echo, or
	// the cursor landing on it.
	uint32_t lo = i, hi = len;
	while (hi > lo && is_space(text[hi - 1])) hi--;
	if (hi - lo == 1) spell = true;

	int words = 0;

	while (i < len && words < 12 && !s.full) {

		char c = text[i];

		if (spell) {
			spell_char(&s, c);
			i++;
			words++;
			continue;
		}

		if (is_space(c)) {
			if (c == '\n') {
				// A blank line is a paragraph break, and is heard as
				// one: a longer pause than a sentence, which is how a
				// listener keeps their place over a page.
				uint32_t k = i + 1;
				while (k < len && (text[k] == ' ' || text[k] == '\r')) k++;
				emit(&s, (k < len && text[k] == '\n') ? "|" : ",");
			}
			i++;
			continue;
		}

		if (is_alpha(c)) {
			// One word, split at camelCase humps: "fileName" is two
			// words, and so is "HTTPServer" (HTTP, Server).
			uint32_t st = i;
			i++;
			while (i < len && is_alpha(text[i])) {
				if (is_lower(text[i - 1]) && is_upper(text[i])) break;
				if (is_upper(text[i - 1]) && is_upper(text[i]) &&
				    i + 1 < len && is_lower(text[i + 1])) break;
				i++;
			}
			uint32_t en = i;
			// An apostrophe inside a word is part of it ("don't").
			if (i + 1 < len && text[i] == '\'' && is_lower(text[i + 1])) {
				char buf[LTS_WORD_MAX + 1];
				uint32_t n = 0;
				for (uint32_t k = st; k < en && n < LTS_WORD_MAX; k++) buf[n++] = text[k];
				i++;
				while (i < len && is_alpha(text[i]) && n < LTS_WORD_MAX) buf[n++] = text[i++];
				say_word(&s, buf, n);
			} else {
				say_word(&s, &text[st], en - st);
			}
			// What was just said becomes the evidence for the next
			// homograph (see homograph()).
			{
				uint32_t n = en - st, k;
				if (n >= sizeof(prev_word)) n = sizeof(prev_word) - 1;
				for (k = 0; k < n; k++) {
					char c = text[st + k];
					prev_word[k] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
				}
				prev_word[n] = 0;
			}
			words++;
			continue;
		}

		if (is_digit(c) || (c == '-' && i + 1 < len && is_digit(text[i + 1]) &&
		    (i == 0 || is_space(text[i - 1])))) {
			if (c == '-') { emit(&s, "M AY1 N AX0 S"); i++; }
			char d[16];
			uint32_t n = 0;
			while (i < len && (is_digit(text[i]) ||
			       (text[i] == ',' && i + 3 < len && is_digit(text[i + 1]) &&
			        is_digit(text[i + 2]) && is_digit(text[i + 3])))) {
				if (text[i] != ',' && n < sizeof(d)) d[n++] = text[i];
				i++;
			}
			say_digits(&s, d, n);
			if (i + 1 < len && text[i] == '.' && is_digit(text[i + 1])) {
				emit(&s, "P OY1 N T");
				i++;
				while (i < len && is_digit(text[i])) emit(&s, ones[text[i++] - '0']);
			}
			if (i < len && text[i] == '%') { emit(&s, "P ER0 S EH1 N T"); i++; }
			words += (int)(n / 3) + 1;
			continue;
		}

		// Punctuation and symbols.
		uint32_t run = 1;
		while (i + run < len && text[i + run] == c) run++;
		if (run >= 3 && !(c == '.' && run == 3)) {		// "-----", "=====": a rule, not text
			emit(&s, ",");
			i += run;
			continue;
		}

		bool between = i > 0 && i + 1 < len && !is_space(text[i - 1]) && !is_space(text[i + 1]);

		if ((c == '.' || c == '!' || c == '?') && !(c == '.' && between)) {
			char t[2] = { c, 0 };
			emit(&s, t);
			i++;
			break;							// a sentence is a chunk
		}
		if (c == '.' && between) { emit(&s, "D AA1 T"); i++; continue; }	// file.txt
		if (c == ',' || c == ';' || c == ':') { emit(&s, ","); i++; continue; }
		if (c == '-') { if (!between) emit(&s, ","); i++; continue; }

		const sym_t *y = sym_of(c);
		if (y && y->inline_) emit(&s, y->ph);
		i++;								// quotes, brackets: nothing
	}

	// Stopped at the word limit with the sentence's own punctuation
	// right after: it belongs to THIS chunk. Left for the next one it
	// is a chunk of one character, and a one-character chunk is spelled
	// (above) -- so a sentence of exactly twelve words ended in "dot".
	// Only punctuation that ends something: "file.txt" is not touched.
	if (!spell && words >= 12) {
		while (i < len && (text[i] == '.' || text[i] == '!' || text[i] == '?' ||
		       text[i] == ',' || text[i] == ';' || text[i] == ':') &&
		       (i + 1 >= len || is_space(text[i + 1]) || text[i + 1] == text[i])) {
			char t[2] = { (text[i] == ',' || text[i] == ';' || text[i] == ':') ? ',' : text[i], 0 };
			emit(&s, t);
			i++;
		}
	}

	*pos = i;
	while (*pos < len && is_space(text[*pos])) (*pos)++;
	*more = *pos < len;
	return true;

}
