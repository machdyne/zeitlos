#ifndef SMF_H
#define SMF_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * smf -- Standard MIDI File parsing and sequencing for sw/apps/midi.
 *
 * -- what this file is allowed to touch --
 *
 * NOTHING. No hardware, no filesystem, no sw/common, no malloc, no
 * stdio. Same rule sw/apps/track/modplay.c and sw/apps/play/adec.c
 * live under, for the same reason: every timing and parsing decision
 * here can then be checked with the HOST compiler, where a wrong
 * answer prints as a number instead of playing slightly wrong.
 *
 * -- a MIDI file is a SCORE, not audio --
 *
 * This is the whole reason a MIDI player is easy on this machine and
 * sw/apps/play was hard. play had to put 46875 output frames a second
 * in front of a DAC, and measured IPC of 0.08 gave it about 29
 * instructions per frame to do it in. Nothing here touches an output
 * frame at all: this emits note-on and note-off events at a few
 * hundred hertz, and rtl/audio_mixer.v turns them into sound in
 * gateware.
 *
 * The file is small enough to load whole -- tens of KB is typical --
 * so there is no streaming, no ring buffer and no SD card in the
 * playback path.
 *
 * -- what this does NOT do --
 *
 * It does not synthesise. It reports events; the caller decides what
 * they sound like. That split is what lets the whole sequencer be
 * tested against a stopwatch on the host, and what will let a real
 * sampled instrument bank replace the waveform synth later without
 * touching a line of it.
 */

#include <stdint.h>
#include <stdbool.h>

enum {
    SMF_OK = 0,
    SMF_ERR_MAGIC,      /* not a MThd */
    SMF_ERR_FORMAT,     /* format 2 -- see smf_open() */
    SMF_ERR_TRACKS,     /* more tracks than SMF_MAX_TRACKS */
    SMF_ERR_DIVISION,   /* SMPTE timing, or zero */
    SMF_ERR_TRUNC       /* a chunk runs off the end of the buffer */
};

/*
 * Tracks merged at once.
 *
 * A format 1 file is N parallel tracks that must be interleaved by
 * timestamp, so each needs its own cursor -- there is no way to merge
 * them in place without rewriting the file. 32 covers everything
 * ordinary; a 64-track orchestral export is refused with a specific
 * error rather than silently playing half of itself.
 */
#define SMF_MAX_TRACKS 32

/* MIDI's own limits, not ours. */
#define SMF_CHANNELS 16
#define SMF_DRUM_CHANNEL 9      /* "channel 10", counting from 1 */

typedef struct {
    const uint8_t *start;   /* where the track body begins */
    const uint8_t *p;       /* cursor within the track */
    const uint8_t *end;
    uint32_t  next_tick;    /* absolute tick of this track's next event */
    uint8_t   running;      /* running status byte */
    bool      done;
} smf_track_t;

/* What the sequencer hands back. One per call to smf_step(). */
enum {
    SMF_EV_NONE = 0,
    SMF_EV_NOTE_ON,
    SMF_EV_NOTE_OFF,
    SMF_EV_PROGRAM,
    SMF_EV_CONTROL,     /* only the controllers below are reported */
    SMF_EV_PITCH,
    SMF_EV_END
};

/* Controllers worth acting on. Everything else is parsed and dropped:
 * reporting controllers nobody handles just moves the filtering into
 * the caller. */
#define SMF_CC_VOLUME     7
#define SMF_CC_PAN       10
#define SMF_CC_EXPRESSION 11
#define SMF_CC_SUSTAIN   64
#define SMF_CC_ALL_OFF  120
#define SMF_CC_ALL_NOTES_OFF 123

typedef struct {
    int      type;
    uint8_t  channel;
    uint8_t  a;         /* note, program, or controller number */
    uint8_t  b;         /* velocity or controller value */
    int16_t  bend;      /* SMF_EV_PITCH: -8192..8191 */
} smf_event_t;

typedef struct {

    const uint8_t *buf;
    uint32_t   len;

    int        format;
    int        ntracks;
    uint32_t   division;        /* ticks per quarter note */

    smf_track_t track[SMF_MAX_TRACKS];

    uint32_t   tick;            /* current position, in file ticks */
    uint32_t   tempo_us;        /* microseconds per quarter note */

    /*
     * Fractional tick accumulator, 16 bits of fraction.
     *
     * Time advances in whatever unit the caller counts in -- audio
     * frames, or kernel ticks -- and one of those is almost never a
     * whole number of MIDI ticks. Accumulating the remainder rather
     * than rounding is the difference between a file that ends in
     * time and one that drifts a bar over three minutes.
     */
    uint32_t   frac;

    bool       ended;
    uint32_t   total_ticks;     /* filled by smf_scan_length() */

} smf_t;

/*
 * Parse the header and locate every track. `buf` must stay valid for
 * as long as the smf_t is used -- nothing is copied.
 *
 * Format 0 (one track) and format 1 (parallel tracks, one tempo map)
 * are supported. Format 2 is refused: its tracks are INDEPENDENT
 * sequences rather than parallel ones, so "play the file" has no
 * single meaning, and guessing one is worse than saying so.
 *
 * SMPTE division (negative) is also refused. It is rare, it needs a
 * completely different clock, and silently treating it as PPQN would
 * play at an arbitrary wrong tempo -- which sounds like a bug in the
 * sequencer rather than an unsupported timing mode.
 */
int smf_open(smf_t *s, const uint8_t *buf, uint32_t len);

/* Rewind to the start without re-parsing. */
void smf_rewind(smf_t *s);

/*
 * Advance by `us` microseconds and return the next pending event, or
 * SMF_EV_NONE when nothing is due.
 *
 * Call it repeatedly with us = 0 until it returns SMF_EV_NONE to
 * drain everything that fell due in one advance -- a chord is several
 * events at the same tick, and returning only the first would arpeggiate
 * the whole file.
 */
int smf_step(smf_t *s, uint32_t us, smf_event_t *ev);

/*
 * Total length in ticks, by walking every track once without playing.
 *
 * Separate from smf_open() because it is only needed for a progress
 * display, it costs a full pass over the file, and a player that
 * cannot show a total is still a player. smf_duration_ms() converts,
 * but only correctly for a file whose tempo never changes -- which is
 * most of them and not all, so it is documented as an estimate.
 */
void smf_scan_length(smf_t *s);
uint32_t smf_duration_ms(const smf_t *s);

/* Elapsed milliseconds at the current position, same caveat. */
uint32_t smf_elapsed_ms(const smf_t *s);

const char *smf_strerror(int err);

#endif
