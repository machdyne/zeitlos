/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zi18n.c on the host, against catalogs tools/i18n.py writes
 * (mk_i18n_fixture.py). docs/i18n.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zi18n.h"
#include "zlang.h"

static int run, failed;
#define CHECK(c, ...) do { run++; if (!(c)) { failed++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint8_t *slurp(const char *dir, const char *name, uint32_t *len) {
	char p[512];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	FILE *f = fopen(p, "rb");
	if (!f) { printf("no %s\n", p); exit(2); }
	uint8_t *b = malloc(1 << 16);
	*len = (uint32_t)fread(b, 1, 1 << 16, f);
	fclose(f);
	return b;
}

// a function, so each call is one call site with its own cache
static const char *save_as(void) { return Z_("Save as..."); }
static const char *files_small(void) { return ZS_("Files"); }
static const char *cancel(void) { return Z_("Cancel"); }
static const char *open_verb(void) { return Z_C("verb", "Open"); }
static const char *untranslated(void) { return Z_("Quit"); }

int main(int argc, char **argv) {

	const char *dir = argc > 1 ? argv[1] : ".";
	uint32_t dl, rl, jl, sl;
	uint8_t *de = slurp(dir, "de.zlc", &dl), *ru = slurp(dir, "ru.zlc", &rl);
	uint8_t *ja = slurp(dir, "ja.zlc", &jl), *s58 = slurp(dir, "s58.zfn", &sl);

	// -- before z_tr_init(): English, and the answer is final --
	const char *a = save_as();
	CHECK(!strcmp(a, "Save as...") && save_as() == a, "not initialised: the English, cached");

	// -- the pure lookup --
	CHECK(z_tr_find(de, dl, z_tr_hash("text"), z_tr_hash("Save as...")) &&
		!strcmp(z_tr_find(de, dl, z_tr_hash("text"), z_tr_hash("Save as...")), "Speichern unter..."),
		"z_tr_find: a key in a domain");
	CHECK(!z_tr_find(de, dl, z_tr_hash("files"), z_tr_hash("Save as...")), "not in another domain");
	CHECK(!z_tr_find(de, 10, z_tr_hash("text"), z_tr_hash("Save as...")), "a short file finds nothing");

	// -- German, for the app "text" --
	z_tr_use(de, dl, "de", "text");
	CHECK(!strcmp(z_tr_lang(), "de"), "the language");
	CHECK(!strcmp(z_tr("Save as..."), "Speichern unter..."), "the app's own before common's");
	CHECK(!strcmp(cancel(), "Abbrechen"), "common's, when the app has none");
	CHECK(!strcmp(open_verb(), "Öffnen") && !strcmp(z_tr("Open"), "Open"), "context");
	CHECK(!strcmp(untranslated(), "Quit"), "a string with no translation: English");
	CHECK(!strcmp(Z_N("%d file", "%d files", 1), "%d Datei") &&
		!strcmp(Z_N("%d file", "%d files", 5), "%d Dateien") &&
		!strcmp(Z_N("%d file", "%d files", 0), "%d Dateien"), "plurals, n != 1");
	CHECK(!strcmp(files_small(), "Dateien"), "ZS_: Latin-9 is small enough");

	// -- Russian: three plural forms; Cyrillic small only with the 5x8 glyphs --
	z_lang_use(Z_LANG_FONT_5X8, 0, 0);
	z_tr_use(ru, rl, "ru", "text");
	CHECK(!strcmp(Z_N("%d file", "%d files", 1), "%d файл") &&
		!strcmp(Z_N("%d file", "%d files", 3), "%d файла") &&
		!strcmp(Z_N("%d file", "%d files", 11), "%d файлов") &&
		!strcmp(Z_N("%d file", "%d files", 22), "%d файла") &&
		!strcmp(Z_N("%d file", "%d files", 25), "%d файлов"), "plurals, Russian");
	CHECK(!z_tr_fits_small("Файлы"), "no 5x8 Cyrillic: does not fit");
	CHECK(!strcmp(z_tr_small("Files"), "Files"), "so the small title stays English");
	z_lang_use(Z_LANG_FONT_5X8, s58, sl);
	CHECK(z_tr_fits_small("Файлы") && !strcmp(z_tr_small("Files"), "Файлы"), "with them: Cyrillic");

	// -- Japanese: never small --
	z_tr_use(ja, jl, "ja", "text");
	CHECK(!strcmp(z_tr("Save as..."), "名前を付けて保存"), "Japanese");
	CHECK(!strcmp(z_tr_small("Files"), "Files"), "Japanese in the small font: English");
	CHECK(z_tr_fits_small("Ünïcödé € Š") && z_tr_fits_small("plain"), "Latin-9 always fits");

	// -- the plural rules on their own --
	CHECK(z_tr_plural(Z_TR_PLURAL_FR, 0) == 0 && z_tr_plural(Z_TR_PLURAL_FR, 2) == 1, "French: 0 is singular");
	CHECK(z_tr_plural(Z_TR_PLURAL_PL, 1) == 0 && z_tr_plural(Z_TR_PLURAL_PL, 3) == 1 &&
		z_tr_plural(Z_TR_PLURAL_PL, 12) == 2 && z_tr_plural(Z_TR_PLURAL_PL, 23) == 1 &&
		z_tr_plural(Z_TR_PLURAL_PL, 21) == 2, "Polish");
	CHECK(z_tr_plural(Z_TR_PLURAL_CS, 4) == 1 && z_tr_plural(Z_TR_PLURAL_CS, 5) == 2, "Czech");

	// -- English: the call sites cache the literal itself --
	z_tr_use(0, 0, "en", "text");
	CHECK(!strcmp(z_tr("Save as..."), "Save as...") && !strcmp(z_tr_lang(), "en"), "English");

	printf("test_zi18n: %d checks, %d failed\n", run, failed);
	return failed != 0;

}
