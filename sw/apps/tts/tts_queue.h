#ifndef TTS_QUEUE_H
#define TTS_QUEUE_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The tts service's utterance queue: what to say next, what to throw
 * away, and who to tell. No I/O and no syscalls -- the two things it
 * has to do to the outside world (notify a sender, stop the audio)
 * are callbacks -- so tests/tts_test.c runs it on the build machine.
 * See docs/tts.md, "The queue".
 */

#include <stdint.h>
#include <stdbool.h>

// Utterances waiting, including the one being spoken.
#define TTSQ_ITEMS		32

// Bytes of text they share. Two full clipboards' worth
// (Z_TTS_UTTER_MAX, ztts.h); everything else is a line or less.
#define TTSQ_ARENA		8192

// What Super+R can repeat. Longer utterances repeat truncated.
#define TTSQ_LAST_MAX	1024

typedef struct {
	// Tell `to` that its utterance `mark` was spoken or cancelled.
	// subject is Z_TTS_MARK_DONE or Z_TTS_MARK_CANCELLED.
	void (*notify)(uint32_t to, uint32_t subject, uint16_t mark);
	// Abandon the utterance currently being spoken, immediately.
	void (*stop)(void);
} ttsq_ops_t;

void ttsq_init(const ttsq_ops_t *ops);

#define TTSQ_QUEUED		0
#define TTSQ_DROPPED	1

// A Z_TTS_SAY. Copies (and sanitises) `text` before returning, so the
// sender's buffer is free the moment this returns. Applies
// Z_TTS_F_INTERRUPT and Z_TTS_F_LOW. A marked utterance that is
// dropped is reported Z_TTS_MARK_CANCELLED before this returns.
int ttsq_say(uint32_t from, const char *text, uint32_t flags, uint16_t mark);

// Z_TTS_STOP: stop the current utterance, discard the queue, and
// report every marked one CANCELLED.
void ttsq_cancel_all(void);

// Z_TTS_REPEAT: cancel everything and queue the last utterance that
// was started. False if nothing has been said yet.
bool ttsq_repeat(void);

// -- driving it --
//
// The service loop:
//
//     if (!ttsq_speaking() && ttsq_start_next()) backend_start(...);
//     if (ttsq_speaking() && backend finished)   ttsq_done();

// Starts the next queued utterance, if there is one and nothing is
// being spoken. True if it started one.
bool ttsq_start_next(void);

// The utterance being spoken: its text (NUL-terminated, `*len` bytes)
// and flags. NULL if none.
const char *ttsq_current(uint32_t *len, uint32_t *flags);

bool ttsq_speaking(void);

// The current utterance finished: report it DONE and drop it.
void ttsq_done(void);

// Anything being spoken or waiting?
bool ttsq_busy(void);

uint32_t ttsq_count(void);

#endif
