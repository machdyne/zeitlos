#ifndef ZLANGS_H
#define ZLANGS_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The languages a person can choose (system.language, docs/i18n.md),
 * for the two places that offer the choice: settings and welcome. Each
 * by its own name, in UTF-8 -- drawn with jfont's glyphs when it is
 * running -- and by its English name, for when it is not.
 *
 * A language is offered when the card has its shared catalog,
 * /data/jfont/lang/<code>.zlc (z_lang_on_card()); English always.
 */

#include <stdbool.h>
#include <stdio.h>

#include "zpaths.h"
#include "zfsapp.h"
#include "zlang.h"

typedef struct { const char *code, *own, *english; } z_lang_name_t;

static const z_lang_name_t z_lang_names[] = {
	{ "en", "English", "English" },
	{ "de", "Deutsch", "German" },
	{ "fr", "Fran\xc3\xa7" "ais", "French" },
	{ "es", "Espa\xc3\xb1ol", "Spanish" },
	{ "it", "Italiano", "Italian" },
	{ "pt", "Portugu\xc3\xaas", "Portuguese" },
	{ "nl", "Nederlands", "Dutch" },
	{ "pl", "Polski", "Polish" },
	{ "cs", "\xc4\x8c" "e\xc5\xa1tina", "Czech" },
	{ "sv", "Svenska", "Swedish" },
	{ "ru", "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9", "Russian" },
	{ "uk", "\xd0\xa3\xd0\xba\xd1\x80\xd0\xb0\xd1\x97\xd0\xbd\xd1\x81\xd1\x8c\xd0\xba\xd0\xb0", "Ukrainian" },
	{ "ja", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "Japanese" },
	{ "xx", "[Ps\xc3\xa9\xc3\xba" "d\xc3\xb3]", "the test language" },
};
#define Z_LANG_NAME_COUNT (int)(sizeof(z_lang_names) / sizeof(z_lang_names[0]))

// The card has language i (English: always).
static inline bool z_lang_on_card(int i) {
	char path[64];
	if (i == 0) return true;
	snprintf(path, sizeof path, Z_DIR_LANG "/%s.zlc", z_lang_names[i].code);
	return fs_size(path) > 0;
}

// jfont is running: a language chosen now reaches every window opened
// from now on (each reads system.language as it starts). It holds the
// glyphs whatever the language, so they are what is looked for.
static inline bool z_lang_service_up(void) {
	uint32_t len = 0;
	return z_lang_data(Z_LANG_FONT_5X8, &len) != 0;
}

#endif
