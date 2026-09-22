#ifndef DSYN_H
#define DSYN_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The recorded voice: diphones from a speech pack, joined by TD-PSOLA.
 *
 * A diphone runs from the middle of one sound to the middle of the next,
 * cut from real recordings (tools/speech builds them from LJSpeech; its
 * README says how they were chosen and measured). The phoneme layer
 * (phon.c) still decides everything but the sound -- which phones, how
 * long, what pitch -- and this replaces the formant synthesiser for
 * the sound itself. With no DIPHONE section in the pack, none of this
 * runs and the formant voice speaks, as it always has.
 *
 * Cost: a few multiply-adds per output sample and one small card read
 * per diphone, against the formant voice's resonator cascade.
 */

#include <stdint.h>
#include <stdbool.h>

// Called by pack.c when a pack with a DIPHONE section opens, and with
// the section's place in the file. False if the section is unusable.
bool dsyn_open(uint32_t off, uint32_t len);
void dsyn_close(void);
bool dsyn_ready(void);

// After phon_begin(): takes the utterance's phones, timing and pitch
// from the phoneme layer (it drains phon_next()) and plans the units.
// False if there is nothing to say.
bool dsyn_begin(void);

// The next up to `n` samples at SYNTH_FS. Returns how many; 0 when the
// utterance is finished.
int dsyn_render(int16_t *out, int n);

// Output gain, 0..255, like synth_set_volume().
void dsyn_set_volume(uint32_t v);

// Diphones the last utterances needed and the pack lacked (stood in for
// by halves of others), for diagnostics.
uint32_t dsyn_missing(void);

#endif
