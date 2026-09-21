#ifndef TEXT2PH_H
#define TEXT2PH_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Text to phonemes -- PHASE 2 STAND-IN. See docs/tts.md, "The voice".
 *
 * Just enough to hear the synthesiser speak real messages before the
 * phase 3 front end exists:
 *
 *   - "[HH AX0 L OW1]": text in brackets IS phonemes, passed through.
 *   - Words in a small dictionary (the interface vocabulary wm and
 *     the tests use) are spoken as words.
 *   - Digits are spoken one at a time.
 *   - Anything else is SPELLED, letter by letter. That is the part
 *     phase 3's letter-to-sound rules replace.
 *   - Z_TTS_F_SPELL spells everything.
 *
 * Works a chunk at a time, so an utterance of any length never needs
 * more than one chunk's worth of phonemes in memory.
 */

#include <stdint.h>
#include <stdbool.h>

// Converts from text[*pos] up to about `max_words` words or the end of
// a sentence, whichever is first, writing a NUL-terminated phoneme
// string to `out` and advancing *pos. Returns false when nothing is
// left. *more is set if text remains after this chunk.
bool text2ph_chunk(const char *text, uint32_t len, uint32_t *pos, bool spell,
	char *out, uint32_t outsize, bool *more);

#endif
