#ifndef LTS_H
#define LTS_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Letter-to-sound rules: an English word's spelling to phonemes, for
 * words the exception dictionary (text2ph.c) does not know. See
 * lts.c for the rule format and docs/tts.md, "Letters to sounds".
 */

#include <stdint.h>
#include <stdbool.h>

#define LTS_WORD_MAX	40

// Writes the phonemes for `word` (letters only; anything else is
// ignored) to `out`, with stress digits on the vowels. False if the
// word had no letters.
bool lts_word(const char *word, char *out, uint32_t size);

// The stress pass on its own: puts a digit on every vowel of `out`,
// given the word it came from. Shared with the trained rules in a
// speech pack (pack.c), which learn the phonemes but not the stress --
// stress is about syllables and suffixes rather than letters, and the
// same heuristic serves both.
void lts_add_stress(char *out, uint32_t size, const char *word, int n);

#endif
