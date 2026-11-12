/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The other languages' rules (lang.c, sw/data/tts/lang/<code>.ttl):
 *
 *   - every file loads, and every phoneme its rules write is one the
 *     synthesiser has (phon.c) -- a typo there is a silent sound;
 *   - words whose phonemes were checked by hand still come out so;
 *   - every translation in tools/translations/<lang>.json (the words
 *     the windows show, and so what the voice reads) is said, and none
 *     comes out empty.
 *
 * Run from sw/apps/tts (make test).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../lang.h"
#include "../phon.h"

static int fails, checks;

static void expect(bool ok, const char *what) {
	checks++;
	if (!ok) { fails++; printf("  FAIL: %s\n", what); }
}

static const char *const codes[] = { "es", "it", "pt", "de", "nl", "sv", "pl", "cs", "ru", "uk", "fr" };

static char file[32 * 1024];

// Every phoneme a rule writes, through phon_begin(): one it does not
// know makes no frames.
static void check_rules(const char *code) {
	char path[96];
	snprintf(path, sizeof path, "../../data/tts/lang/%s.ttl", code);
	FILE *f = fopen(path, "rb");
	if (!f) { expect(false, path); return; }
	size_t n = fread(file, 1, sizeof file - 1, f);
	fclose(f);
	file[n] = 0;
	int rules = 0, bad = 0;
	for (char *line = strtok(file, "\n"); line; line = strtok(0, "\n")) {
		bool is_rule = !strncmp(line, "r ", 2), is_word = !strncmp(line, "word ", 5);
		if (!is_rule && !is_word) continue;
		char *eq = strstr(line, " = ");
		if (!eq) continue;
		rules += is_rule;
		char out[128];
		snprintf(out, sizeof out, "%s", eq + 3);
		char *save = 0;
		for (char *t = strtok_r(out, " ", &save); t; t = strtok_r(0, " ", &save)) {
			char tok[8];
			snprintf(tok, sizeof tok, "%s", t);
			size_t l = strlen(tok);
			while (l && (tok[l - 1] == '\'' || (tok[l - 1] >= '0' && tok[l - 1] <= '2'))) tok[--l] = 0;
			if (!strcmp(tok, "-")) continue;
			phon_opts_t o = { 180, 110, false, 0, 0 };
			if (!phon_begin(tok, &o)) {
				printf("  %s: '%s' is not a phoneme (%s)\n", code, tok, line);
				bad++;
			}
		}
	}
	char what[64];
	snprintf(what, sizeof what, "%s: %d rules, every phoneme known", code, rules);
	expect(rules > 30 && bad == 0, what);
}

static char said[4096];

static const char *say(const char *text) {
	char out[1024];
	uint32_t pos = 0, len = (uint32_t)strlen(text);
	bool more = true;
	said[0] = 0;
	while (more && lang_chunk(text, len, &pos, false, out, sizeof out, &more)) {
		if (said[0]) strcat(said, " ");
		if (strlen(said) + strlen(out) < sizeof said - 2) strcat(said, out);
	}
	return said;
}

static void same(const char *code, const char *text, const char *want) {
	char what[256];
	if (!lang_load(code)) { expect(false, code); return; }
	const char *got = say(text);
	snprintf(what, sizeof what, "%s: \"%s\" -> %s", code, text, want);
	expect(!strcmp(got, want), what);
	if (strcmp(got, want)) printf("        got %s\n", got);
}

// The translations: a JSON object of "key": "value" (or [forms]).
// Only the values matter here.
static void check_catalog(const char *code) {
	char path[96];
	snprintf(path, sizeof path, "../../../tools/translations/%s.json", code);
	FILE *f = fopen(path, "rb");
	if (!f) { printf("  (no %s)\n", path); return; }
	static char json[256 * 1024];
	size_t n = fread(json, 1, sizeof json - 1, f);
	fclose(f);
	json[n] = 0;
	if (!lang_load(code)) { expect(false, code); return; }
	int strings = 0, empty = 0;
	// every second string on a line, roughly: values follow ": "
	for (char *p = json; (p = strstr(p, "\": ")); ) {
		p += 3;
		char *q = p;
		while (*q == '[' || *q == ' ') q++;
		for (;;) {
			if (*q != '"') break;
			q++;
			static char v[4096];
			size_t k = 0;
			while (*q && *q != '"' && k < sizeof v - 1) {
				if (*q == '\\' && q[1]) {
					q++;
					v[k++] = *q == 'n' ? '\n' : *q == 't' ? ' ' : *q;
					q++;
					continue;
				}
				v[k++] = *q++;
			}
			v[k] = 0;
			if (*q) q++;
			strings++;
			bool letters = false;
			for (size_t i = 0; i < k; i++)
				if ((unsigned char)v[i] >= 0xC0 || (v[i] | 32) >= 'a') letters = true;
			const char *ph = say(v);
			if (letters && !ph[0]) {
				empty++;
				if (empty <= 3) printf("  %s: nothing said for \"%.60s\"\n", code, v);
			}
			while (*q == ',' || *q == ' ') q++;
		}
		p = q;
	}
	char what[96];
	snprintf(what, sizeof what, "%s: all %d translations said", code, strings);
	expect(strings > 300 && empty == 0, what);
}

int main(void) {

	printf("lang: the rules\n");
	for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; i++) {
		check_rules(codes[i]);
		char what[32];
		snprintf(what, sizeof what, "%s loads", codes[i]);
		expect(lang_load(codes[i]) && lang_active() && lang_name()[0], what);
	}
	expect(!lang_load("ja") && !lang_active(), "ja: no rules -- English stays");
	expect(!lang_load("en") && !lang_active(), "en unloads");

	printf("lang: words checked by hand\n");
	same("es", "La canción del niño, mañana.",
		"L A0 K A2 N S Y O1 N D E0 L N I1 NY O2 , M A2 NY A1 N A2 .");
	same("es", "1.024 y 21%",
		"M I1 L B E2 Y N T I2 K W A1 T DX O2 I0 B E2 Y N T I2 U1 N O2 P O0 DX S Y E1 N T O2");
	same("de", "Nicht gefunden", "N IH1 XJ T G AX0 F UH1 N D AX0 N");
	same("de", "zwanzig", "T S V A1 N T S I0 XJ");
	same("de", "bericht.txt", "B AX0 RU IH1 XJ T P UH1 NG K T T E1 IH1 K S T E1");
	same("it", "Foglio pieno", "F O1 LY O2 P Y E1 N O2");
	same("pt", "Não encontrado", "N AN1 W EN2 K ON2 T DX A1 D U2");
	same("fr", "quelque point vingt", "K EH1 L K P W EN1 V EN1");
	same("pl", "Coś poszło nie tak.", "T S O1 SH P O1 SH W O2 NY E0 T A1 K .");
	same("cs", "Vnější odkaz", "V NY E1 Y SH I2 O1 D K A2 S");
	same("sv", "Något gick fel.", "N O1 G U2 T Y I1 K F E1 L .");
	same("ru", "Удалить файл", "U2 D A2 L I1 T F A1 Y L");
	same("uk", "п'ять", "P Y A1 T");

	printf("lang: every translation\n");
	for (unsigned i = 0; i < sizeof codes / sizeof codes[0]; i++) check_catalog(codes[i]);

	printf("lang: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;

}
