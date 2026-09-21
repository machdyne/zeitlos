/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host test for text2ph.c and lts.c: the front end's decisions, which
 * are the part that goes wrong silently. Exact phoneme strings are
 * checked only where they are the point (numbers, names of symbols);
 * elsewhere the test checks WHAT was decided -- spelled or not, a
 * pause or not, where a chunk ends.
 */
#include <stdio.h>
#include <string.h>
#include "../text2ph.h"
#include "../lts.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static char out[4096];

// Whole utterance, chunks joined with " | ".
static const char *ph(const char *t, bool spell) {
	static char all[8192];
	all[0] = 0;
	uint32_t pos = 0, len = (uint32_t)strlen(t);
	bool more = true;
	while (more && text2ph_chunk(t, len, &pos, spell, out, sizeof(out), &more)) {
		if (all[0]) strcat(all, "| ");
		strcat(all, out);
	}
	return all;
}
static int has(const char *t, const char *want) { return strstr(ph(t, false), want) != 0; }

int main(void) {
	// numbers
	CHECK(strcmp(ph("42", false), "F AO1 R T IY0 T UW1 ") == 0);
	CHECK(has("4,096", "F AO1 R TH AW1 Z AX0 N D N AY1 N T IY0 S IH1 K S"));
	CHECK(has("1000000", "W AH1 N M IH1 L Y AX0 N"));
	CHECK(has("0", "Z IY1 R OW0"));
	CHECK(has("3.14", "TH R IY1 P OY1 N T W AH1 N F AO1 R"));
	CHECK(has("-5", "M AY1 N AX0 S F AY1 V"));
	CHECK(has("50%", "P ER0 S EH1 N T"));
	CHECK(has("007", "Z IY1 R OW0 Z IY1 R OW0 S EH1 V AX0 N"));		// leading zero: digits
	CHECK(has("1234567890123", "W AH1 N T UW1 TH R IY1 F AO1 R"));		// too long: digits
	CHECK(has("115", "W AH1 N HH AH1 N D R IX0 D F IH1 F T IY2 N"));
	// words: dictionary, rules, spelling
	CHECK(has("hello", "HH AX0 L OW1"));
	CHECK(has("USB", "Y UW1 _ EH1 S _ B IY1"));				// acronym spelled
	CHECK(!has("Zeitlos", "Z IY1 _"));						// a capitalised word is not
	CHECK(has("fileName", "F AY1 L N EY1 M"));				// camelCase split
	CHECK(has("x", "EH1 K S"));								// lone letter
	CHECK(has("I", "AY1"));
	// symbols
	CHECK(has("a@b", "AE1 T"));
	CHECK(has("x = y", "IY1 K W AX0 L Z"));
	CHECK(!has("(hello)", "P ER0 EH1 N"));					// brackets silent in text...
	CHECK(has(")", "R AY1 T _ P ER0 EH1 N"));				// ...named alone
	CHECK(has(";", "S EH1 M IY0"));
	CHECK(has("readme.txt", "D AA1 T"));						// dot inside a name
	CHECK(strcmp(ph("-----", false), ", ") == 0);			// a rule is a pause
	// sentences end chunks; spelling spells
	CHECK(strstr(ph("One. Two.", false), "| ") != 0);
	CHECK(has("[HH AX0 L OW1]", "HH AX0 L OW1"));
	CHECK(strstr(ph("cat", true), "S IY1 _ EY1 _ T IY1") != 0);
	CHECK(ph("   ", false)[0] == 0);
	// homographs: the same spelling, said by what came before it
	CHECK(has("the record", "R EH1 K ER0 D"));
	CHECK(has("to record", "R IH0 K AO1 R D"));
	CHECK(has("a present", "P R EH1 Z AX0 N T"));
	CHECK(has("they present", "P R IH0 Z EH1 N T"));
	CHECK(has("the subject", "S AH1 B JH EH0 K T"));
	CHECK(has("I object", "AX0 B JH EH1 K T"));
	CHECK(has("I read", "R IY1 D"));
	CHECK(has("I have read", "R EH1 D"));
	CHECK(has("had read", "R EH1 D"));
	CHECK(has("a lead pipe", "L EH1 D"));
	CHECK(has("to lead", "L IY1 D"));
	// no evidence: the noun reading, and a new chunk forgets the
	// previous sentence's last word
	CHECK(has("record", "R EH1 K ER0 D"));
	CHECK(strstr(ph("To lead. Record.", false), "R EH1 K ER0 D") != 0);

	// lts: stress and a few rules that matter for UI words
	char o[256];
	lts_word("return", o, sizeof(o)); CHECK(strstr(o, "T ER1 N") != 0);
	lts_word("making", o, sizeof(o)); CHECK(strstr(o, "M EY1 K") != 0);
	lts_word("files", o, sizeof(o));  CHECK(strcmp(o, "F AY1 L Z") == 0);
	lts_word("notes", o, sizeof(o));  CHECK(strcmp(o, "N OW1 T S") == 0);
	lts_word("wanted", o, sizeof(o)); CHECK(strstr(o, "T IX0 D") != 0);
	lts_word("stopped", o, sizeof(o));CHECK(strstr(o, "P T") != 0);
	lts_word("bytes", o, sizeof(o));  CHECK(strstr(o, "AY1") != 0);
	lts_word("nation", o, sizeof(o)); CHECK(strstr(o, "SH AX0 N") != 0);
	if (fails) { printf("text2ph_test: %d FAILED\n", fails); return 1; }
	printf("text2ph_test: all passed\n");
	return 0;
}
