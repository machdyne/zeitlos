#ifndef LANG_H
#define LANG_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Speaking other languages: text to phonemes for a language other than
 * English, from rules that are data -- /data/tts/lang/<code>.ttl, one
 * file per language, loaded for the language in use only. The formant
 * voice speaks them; the recorded voice is an English speaker's. See
 * docs/tts.md, "Other languages", for the file's format.
 *
 * Same contract as text2ph_chunk(): a sentence or a dozen words at a
 * time. A word the language's rules do not cover at all -- a Latin
 * word in Russian text, an app's name -- is said by the English front
 * end, which is what it most likely is.
 */

#include <stdint.h>
#include <stdbool.h>

#define LANG_FILE_MAX   (24 * 1024)

// Loads the rules for `code` ("de"), from the card. False, and English
// stays, if there are none. "en" unloads.
bool lang_load(const char *code);

// The same from a file of rules in memory (host tests, tools): `text`
// is kept and parsed in place, so it must outlive the language.
bool lang_load_text(char *text, uint32_t len);

// A language other than English is loaded.
bool lang_active(void);

// Its name in English, from the file ("German"), or "".
const char *lang_name(void);

bool lang_chunk(const char *text, uint32_t len, uint32_t *pos, bool spell,
	char *out, uint32_t outsize, bool *more);

#endif
