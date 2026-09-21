#ifndef PACK_H
#define PACK_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Reading a speech pack (ZSPK) from the sdcard: the pronunciation
 * lexicon that tools/speech builds. See the speech-data pipeline docs, published with it for the
 * format and where the file comes from.
 *
 * NOTHING IS RESIDENT. The lexicon is ~400,000 words and several
 * megabytes; the index is searched by SEEKING, sixteen bytes a probe,
 * and one small block is read at the end. About twenty short reads per
 * word, against a service that speaks a few words a second.
 *
 * A pack is optional in every sense: absent, unreadable, the wrong
 * version, or naming a phoneme this build does not have, and the
 * service carries on with its built-in dictionary and letter-to-sound
 * rules. Speech never depends on the card.
 */

#include <stdint.h>
#include <stdbool.h>

#define PACK_PATH	"/speech/en.spk"

// Opens the pack and checks it over. False (quietly) if there is none.
bool pack_open(const char *path);

void pack_close(void);

bool pack_ready(void);

// The word, lowercase, letters only. Writes its pronunciation as
// ARPAbet with stress digits ("W IH1 N D OW0") and returns true, or
// returns false if the pack does not have it.
bool pack_lookup(const char *word, char *out, uint32_t size);

// Entries in the open pack, for diagnostics.
uint32_t pack_words(void);

// The pack's trained letter-to-sound rules, for a word the lexicon
// does not have. Unlike the lexicon, this model IS resident: it is
// consulted per letter, and seeking for every node would be far too
// slow. It is loaded at pack_open() if it fits PACK_LTS_MAX, and
// silently skipped if not -- lts.c's hand-written rules are the
// fallback either way.
//
// Writes ARPAbet with stress digits, like lts_word(). False if there
// is no model or the word has no letters.
bool pack_lts(const char *word, char *out, uint32_t size);

bool pack_lts_ready(void);

// How many prosody parameters the open pack supplied (0: none, and
// phon.c is using its defaults).
int pack_prosody_fields(void);

// How many phonemes' formants the open pack supplied.
int pack_formant_phones(void);

// The trained model's ceiling, in bytes of .bss.
//
// THIS COSTS MEMORY WHETHER OR NOT A PACK EXISTS, because it is a
// static buffer and this service has no heap worth the name. 96KB
// holds the model tools/speech's default budget produces (91KB) with
// a little room; a bigger one is skipped with a message rather than
// growing every board's memory use. On a machine with 1MB of main
// memory this is a real cost to weigh -- see the speech-data pipeline docs, published with it, which
// lists what smaller budgets buy.
#define PACK_LTS_MAX	(96 * 1024)

#endif
