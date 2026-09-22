#ifndef PHON_H
#define PHON_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * From phonemes to synthesiser frames: the phoneme table, expansion
 * into acoustic segments (a stop is a closure, a burst and maybe some
 * aspiration; a diphthong is a glide between two targets), the
 * transitions between neighbours, and the pitch contour. See
 * docs/tts.md, "The voice".
 *
 * Input is a phoneme string in ARPAbet symbols separated by spaces,
 * vowels optionally followed by a stress digit (0 unstressed,
 * 1 primary, 2 secondary):
 *
 *     "HH AX0 L OW1"               hello
 *     "S EY1 V , B AH1 T AX0 N"    save, button
 *
 * plus "_" (short pause), "," (phrase pause), "." (sentence end) and
 * "?" (question: the pitch rises at the end). Unknown symbols are
 * skipped.
 */

#include <stdint.h>
#include <stdbool.h>

#include "synth.h"

typedef struct {
	uint32_t	rate_wpm;		// 80..450, default 180
	uint32_t	pitch_hz;		// base pitch, default 110
	bool		continues;		// Z_TTS_F_CONTINUES: no final fall or pause
	// The size of the vocal tract, as a % of the default (a man's).
	// A woman's is about 17% shorter, which raises every formant by
	// the same proportion: pitch alone does not make a female voice,
	// it makes a squeaky male one. 0 means 100.
	uint32_t	formant_pct;		// 85..120, measured safe at every pitch
	// How much the pitch moves: 100 is as fitted, 0 is a monotone,
	// 200 twice as lively. Many screen-reader users turn this down at
	// high speed, where intonation becomes a distraction. 0 is a
	// valid setting here, so the field is offset: 0 means 100 and
	// the real value is expression_pct - 1 when set.
	uint32_t	expression_pct;		// 0 = default, else 1 + (0..200)
} phon_opts_t;

// Parses `ph` and prepares to generate. Returns the number of frames
// the utterance will take (0 if it has no sound in it). Replaces any
// utterance in progress.
uint32_t phon_begin(const char *ph, const phon_opts_t *opts);

// Next frame; false when the utterance is finished.
bool phon_next(synth_frame_t *f);

// The token index and phoneme name ("" for a pause) behind the frame
// phon_next() just returned. Used to dump phone boundaries for
// alignment (tools/speech); -1 before the first frame.
int phon_current(const char **name);

// -- prosody parameters --
//
// The numbers behind timing and pitch. Defaults are the hand-set
// rules; a speech pack's PROSODY section replaces them with values
// fitted to a real speaker (the speech-data pipeline docs, published with it). Values are clamped.
enum {
	PHON_PRO_UNSTRESSED,	// unstressed vowel, % of stressed
	PHON_PRO_FINAL_COMMA,	// phrase-final lengthening before a comma, %
	PHON_PRO_FINAL_STOP,	// ...before a full stop, %
	PHON_PRO_PAUSE_COMMA,	// ms
	PHON_PRO_PAUSE_STOP,	// ms
	PHON_PRO_PAUSE_PARA,	// ms
	PHON_PRO_TOP,		// phrase start, % of base pitch
	PHON_PRO_DECL,		// decline across a phrase, % points
	PHON_PRO_STEP,		// each phrase starts this much lower, % points
	PHON_PRO_STEP_MAX,	// ...at most
	PHON_PRO_STRESS1,	// lift on a primary-stressed vowel, % points
	PHON_PRO_STRESS2,	// ...secondary
	PHON_PRO_COMMA_RISE,	// rise at a comma, % points
	PHON_PRO_STOP_FALL,	// fall at a full stop, % points
	PHON_PRO_OPEN,		// glottal open quotient, % of the period
	PHON_PRO_TILT,		// source spectral tilt, dB at 3kHz
	PHON_PRO_N
};

void phon_prosody_reset(void);
void phon_prosody_set(int field, int value);
int phon_prosody_get(int field);
bool phon_prosody_dur(const char *name, uint32_t ms);

// Formants for one phoneme from a pack: F1-F3 then a diphthong's end
// F1-F3, in Hz, 0 to keep the table's. Values more than 40% from the
// table's are refused one by one.
bool phon_prosody_formants(const char *name, const uint16_t f[6]);

// -- experiments --
//
// Candidate changes to the phonetic rules, each off by default and
// measured before any is adopted (tools/speech: `speech compare
// --session1`). Only the rendering harness turns them on, from
// ZTTS_EXP; the device never does. An experiment that proves itself
// becomes the rule, and its bit goes.
enum {
	PHON_EXP_VOWEL_VOICING	= 1u << 0,	// shorter vowel before a voiceless coda
	// 1u << 1 was f1-cutback: measured to change nothing, removed.
	PHON_EXP_VOT		= 1u << 2,	// longer aspiration after P, T, K
	PHON_EXP_VELAR		= 1u << 3,	// K and G's locus follows the vowel
	PHON_EXP_NASAL		= 1u << 4,	// abrupt nasal releases
};

void phon_set_experiments(uint32_t bits);

// An experiment's bit from its name ("vowel-voicing", ...), 0 if none.
uint32_t phon_experiment(const char *name);

#endif
