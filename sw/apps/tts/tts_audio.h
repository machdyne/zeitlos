#ifndef TTS_AUDIO_H
#define TTS_AUDIO_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The tts service's voice: text -> phonemes (text2ph.c) -> frames
 * (phon.c) -> samples (synth.c) -> a ring buffer that one hardware
 * mixer channel plays by itself. See docs/tts.md, "Audio".
 */

#include <stdint.h>
#include <stdbool.h>

#include "phon.h"

#define TA_CHANNEL	7

// True if this bitstream has the hardware mixer. Without it the
// service falls back to its transcript and timing alone.
bool ta_init(void);

// Start saying `text`. The text must stay valid until ta_busy() goes
// false or ta_stop() is called (it lives in the queue's arena, which
// guarantees exactly that).
void ta_start(const char *text, uint32_t len, bool spell, const phon_opts_t *o);

// Render ahead and notice the end. Call at least every ta_wait_ticks().
void ta_pump(void);

bool ta_busy(void);

// Silence the channel now.
void ta_stop(void);

void ta_set_volume(uint32_t v);

uint32_t ta_wait_ticks(void);

// Speak the following utterances with the recorded voice (dsyn.c)
// rather than the formant synthesiser.
void ta_use_recorded(bool on);

#endif
