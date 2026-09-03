#ifndef SYNTH_H
#define SYNTH_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * synth -- the part of sw/apps/midi that decides what a note SOUNDS
 * like. Waveform bank, General MIDI program mapping, envelopes and
 * voice allocation.
 *
 * Like smf.c this touches no hardware and no sw/common: it produces a
 * table of eight voices with a base address, a step, and left/right
 * gains, and the app copies that into rtl/audio_mixer.v's registers.
 * The split is what lets the envelope and pitch arithmetic be checked
 * on the host, and what will let a sampled instrument bank replace the
 * waveforms later without disturbing the sequencer.
 *
 * -- what this can and cannot sound like --
 *
 * Read this before judging the output, because the honest answer to
 * "can it play arbitrary MIDI files" is "yes, and three of the four
 * things that decide whether it sounds good are in this file".
 *
 * WHAT IT CANNOT FIX: there are EIGHT hardware voices. General MIDI
 * files routinely want fifteen to twenty-five notes at once. Voice
 * stealing is audible on dense material and no amount of software
 * changes that.
 *
 * WHAT THIS FILE DECIDES:
 *
 *   Envelopes. Without them every instrument is an organ -- notes
 *   start and stop at full volume and nothing has character. A piano
 *   that decays, a string section that swells and a plucked bass are
 *   the same three waveforms with different ADSR, and the difference
 *   between "chiptune arrangement" and "broken ringtone" is mostly
 *   here.
 *
 *   Noise for percussion. GM channel 10 is about half of most popular
 *   music, and a snare drum made from a square wave is a click. A
 *   pseudo-random table costs 4KB and turns the drum track from
 *   missing into present.
 *
 *   Timbre spread. All 128 GM programs collapsing to one waveform is
 *   what makes a rendering sound like mush: parts that were written to
 *   be distinguishable stop being so. Seven waveforms and sixteen
 *   envelope families is not orchestral, but it keeps the bass, the
 *   lead and the pad apart.
 *
 * The realistic expectation is a good AY-3-8910 or SID-era rendition:
 * recognisable, musical, and obviously not the instruments the file
 * was written for. Files written for chip playback will sound
 * excellent. Orchestral and acoustic material will sound like a
 * transcription, because that is what it is.
 */

#include <stdint.h>
#include <stdbool.h>

#include "smf.h"

/*
 * One period of each waveform, in bytes.
 *
 * 256 is a power of two so the mixer's loop wrap is exact, and short
 * enough that the whole bank is a rounding error in RAM. It is also
 * why pitch is cheap: the mixer's CH_STEP is a phase increment in
 * bytes, so playing a 256-byte table at N bytes per output frame
 * produces a tone of N * out_hz / 256 Hz and the "oscillator" is one
 * register write.
 */
#define SYNTH_WAVE_LEN 256

/*
 * Band-limiting, in mip levels.
 *
 * THIS IS THE FIX FOR THE BACKGROUND NOISE, and the first version of
 * this file had the problem in full.
 *
 * The mixer resamples by dropping samples -- it advances a phase
 * accumulator and fetches whatever byte it lands on, with no
 * interpolation and no filter. A naive saw or pulse table is
 * band-UNLIMITED: 256 samples of hard ramp carry harmonics all the way
 * to the table's own Nyquist, 128 of them. Play that at middle C on a
 * 46875Hz output and only 89 of those harmonics fit below the output
 * Nyquist. The other 39 fold back, and they fold back INHARMONICALLY
 * -- at frequencies with no musical relationship to the note.
 *
 * That is not distortion, which would at least be in tune. It is
 * broadband noise that tracks the music, which is exactly what
 * "there is noise in the background" sounds like. It gets worse the
 * higher the note, and saw and pulse are the worst offenders -- which
 * between them cover strings, ensemble, pad, guitar, organ, brass,
 * reed and lead.
 *
 * The fix is to build each waveform from a SUM OF HARMONICS with the
 * count limited, and to keep several versions at different limits so a
 * high note can use a duller table than a low one. Three levels is
 * enough: the error between the ideal harmonic count and the nearest
 * level is a slight loss of brightness, not a fold-back.
 */
#define SYNTH_MIPS 3

/*
 * The noise table is much longer and deliberately not a power-of-two
 * multiple of anything musical.
 *
 * A short looped noise table does not sound like noise; it sounds like
 * a buzzy tone at the loop frequency, because that is exactly what it
 * is. 4096 bytes at drum pitches is long enough that the period falls
 * below the range where the ear hears it as a pitch.
 */
#define SYNTH_NOISE_LEN 4096

enum {
    SYNTH_W_SINE = 0,
    SYNTH_W_TRI,
    SYNTH_W_SAW,
    SYNTH_W_PULSE50,
    SYNTH_W_PULSE25,
    SYNTH_W_PULSE12,
    SYNTH_W_NOISE,
    SYNTH_W_COUNT
};

/*
 * Bank layout: every periodic waveform at every mip level, then the
 * noise table once (noise has no harmonics to limit).
 *
 * Sample width is chosen at build time by synth_build_bank(): 16-bit
 * if rtl/audio_mixer.v has CH_CTRL[18], 8-bit otherwise. 16-bit is
 * worth having here for a reason that only applies once the waveforms
 * are band-limited -- a band-limited saw has a much lower peak-to-RMS
 * ratio than a hard ramp, so quantising it to 8 bits throws away
 * proportionally more of it. The two changes belong together.
 */
#define SYNTH_PERIODIC (SYNTH_W_COUNT - 1)
#define SYNTH_BANK_SAMPLES \
    (SYNTH_PERIODIC * SYNTH_MIPS * SYNTH_WAVE_LEN + SYNTH_NOISE_LEN)
#define SYNTH_BANK_BYTES (SYNTH_BANK_SAMPLES * 2)

#define SYNTH_VOICES 8

/* Envelope stages. Held separately from "is the voice sounding"
 * because a voice in release is still audible but is the first thing
 * that should be stolen. */
enum { ENV_OFF = 0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {

    bool      active;
    uint8_t   channel;
    uint8_t   note;
    uint8_t   velocity;
    uint8_t   wave;
    uint8_t   mip;

    int       stage;
    int32_t   level;        /* 0..65535, envelope output */
    uint32_t  age;          /* allocation order, for stealing */

    /* what the app copies into the mixer */
    uint32_t  base;         /* byte offset into the waveform bank */
    uint32_t  length;
    uint32_t  loop_start;
    uint32_t  loop_len;
    uint32_t  step;         /* 18.14 bytes per output frame */
    uint8_t   gain_l;
    uint8_t   gain_r;
    bool      retrigger;    /* set on note-on; cleared by the caller */

} synth_voice_t;

typedef struct {

    uint32_t  out_hz;
    int       bits;         /* 8 or 16, sample width of the bank */

    synth_voice_t voice[SYNTH_VOICES];
    uint32_t  age;

    /* per-MIDI-channel state */
    uint8_t   program[SMF_CHANNELS];
    uint8_t   volume[SMF_CHANNELS];
    uint8_t   expression[SMF_CHANNELS];
    uint8_t   pan[SMF_CHANNELS];
    int16_t   bend[SMF_CHANNELS];
    bool      muted[SMF_CHANNELS];

    uint8_t   master;       /* 0..64, the user's volume control */

    uint32_t  stolen;       /* voices taken from a sounding note */

} synth_t;

/*
 * Fill `bank` (SYNTH_BANK_BYTES) with the waveform tables.
 *
 * Generated rather than shipped: seven tables are a few lines of
 * arithmetic, and a generated table cannot drift out of step with the
 * offsets synth.c computes from the same constants.
 */
/*
 * Fill `bank` (SYNTH_BANK_BYTES) with the waveform tables.
 *
 * `bits` is 8 or 16 and must match how the mixer channels will be
 * configured -- check z_audio_mixer_fmt16() before asking for 16.
 * Generated rather than shipped: the tables are arithmetic, and a
 * generated table cannot drift out of step with the offsets synth.c
 * computes from the same constants.
 */
void synth_build_bank(void *bank, int bits);

/* Byte offset and byte length of one waveform at one mip level. */
uint32_t synth_wave_offset(int wave, int mip, int bits);
uint32_t synth_wave_length(int wave, int bits);

/* Harmonic limit of each mip level, for the display and the tests. */
int synth_mip_harmonics(int mip);

void synth_init(synth_t *sy, uint32_t out_hz, int bits);

/* Everything off, all channel state back to defaults. */
void synth_reset(synth_t *sy);

/* Feed one sequencer event. */
void synth_event(synth_t *sy, const smf_event_t *ev);

/*
 * Advance every envelope by `ms` and recompute gains.
 *
 * Called at a few hundred hertz, not per audio frame -- the mixer has
 * no envelope generator, so this IS the envelope, and it is the only
 * per-note work the CPU does at all.
 */
void synth_tick(synth_t *sy, uint32_t ms);

void synth_set_master(synth_t *sy, uint8_t vol_0_64);
void synth_all_off(synth_t *sy);

/* How many voices are sounding, for the display. */
int synth_active(const synth_t *sy);

/* Program name family, for the display. */
const char *synth_family_name(uint8_t program);

#endif
