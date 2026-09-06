#ifndef CHIP8_SOUND_H
#define CHIP8_SOUND_H

/*
 * chip8 -- the buzzer, and XO-CHIP's audio.
 *
 * CHIP-8 has one sound: a tone, on whenever ST is non-zero. XO-CHIP
 * replaces it with a 16-byte (128-bit) pattern buffer played back at a
 * programmable rate. Both are the same thing here -- 128 samples in a
 * looping buffer on one hardware mixer channel -- which is why there
 * is no separate square-wave generator.
 *
 * That mapping is unusually clean and worth stating: the mixer walks a
 * buffer forever given LOOPST/LOOPLEN, and CH_STEP sets the rate. So
 * XO-CHIP's pattern register IS a mixer buffer, its pitch register IS
 * CH_STEP, and a program rewriting its pattern mid-note becomes a
 * memcpy with the channel still running. There is no per-sample work
 * for the CPU at any point.
 *
 * Everything here is a no-op on a board with no audio or no hardware
 * mixer, so a caller needs no #ifdef and no capability check of its
 * own.
 */

#include <stdbool.h>

#include "core.h"

/* Probe the hardware and prepare the sample buffer. Safe to call on a
 * board with no audio. */
void c8_sound_init(void);

bool c8_sound_available(void);

/* Call once per frame. Starts, stops and retunes the channel from the
 * guest's ST, pattern buffer and pitch. */
void c8_sound_update(const c8_t *c);

/* Silence the channel. MUST be called before exiting: a channel left
 * enabled keeps fetching from a buffer that is about to be freed with
 * the process, and the mixer is a bus master. */
void c8_sound_shutdown(void);

#endif
