#ifndef KGSOUND_H
#define KGSOUND_H

/*
 * kidgames -- sound.
 *
 * Three cues, and only three: right, wrong, and level up. The original
 * has no audio at all; this is an addition, and the argument for it is
 * the same one that drives the faces and the star rows -- a lot of the
 * audience cannot read, and a sound says "yes" faster and more
 * certainly than a picture of a smile does.
 *
 * -- WHAT IS DELIBERATELY SILENT --
 *
 * Keystrokes. A click per key sounds like a good idea for the same
 * reason the on-screen keyboard's pulse is, and it is not: a kid
 * hunting for a letter presses a lot of wrong keys on the way to the
 * right one, and a machine that chirps at each one is a machine an
 * adult turns the volume down on. The three cues here mark EVENTS,
 * not input.
 *
 * -- HOW IT PLAYS --
 *
 * Through the hardware mixer (docs/audio.md), one shot: point a
 * channel at a buffer, set its length, trigger it, and the DMA plays
 * it to the end and stops. LOOPLEN of 0 means one-shot, which is
 * exactly this. No CPU is involved after the trigger, which matters --
 * the alternative is pushing samples into the DAC FIFO for the sound's
 * whole duration, from inside a message pump that has other things to
 * do.
 *
 * Everything is optional and silence is a normal outcome: no audio
 * block, no mixer in the bitstream, or no DAC wired on the board all
 * end with kg_sound_available() false and every call here doing
 * nothing. Nothing in the app checks -- a game asks for a sound and
 * either gets one or does not.
 */

#include "kg.h"

typedef enum {
	KG_SND_RIGHT = 0,
	KG_SND_WRONG,
	KG_SND_LEVELUP
} kg_sound_t;

/* Probes, starts the DAC and claims a mixer channel. Safe to call on a
 * board with no audio. */
void kg_sound_init(void);

/* Silences the channel. Call before exiting, or the last cue keeps
 * playing into whatever runs next. */
void kg_sound_shutdown(void);

bool kg_sound_available(void);

/* Render and trigger. Returns immediately; the sound plays by DMA. */
void kg_sound_play(kg_sound_t which);

/*
 * Render a cue into the buffer and return its length in samples,
 * without touching the mixer. Exposed for the host tests.
 *
 * The thing worth testing here is that no cue overruns the buffer.
 * note() appends and stops at the limit, so an overlong cue is not a
 * crash -- it is a sound that is silently cut off mid-note, on
 * whichever board happens to be running a slightly different rate.
 * That is precisely the kind of failure nobody reports and nobody
 * finds.
 */
int kg_sound_render(kg_sound_t which);
int kg_sound_buffer_max(void);

#endif
