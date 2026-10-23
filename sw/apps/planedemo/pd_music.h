#ifndef PD_MUSIC_H
#define PD_MUSIC_H

/*
 * planedemo -- the music: instruments synthesized at start-up, a song
 * of 32 bars at 128 BPM, and a small sequencer that drives the hardware
 * mixer once a frame. See docs/planedemo.md, "The music".
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Time is the film's frame counter, 60 a second: the film and the music
 * are on one clock, so a dropped frame never puts them out of step. A
 * row is a sixteenth note; 128 BPM is 32 rows in 225 frames exactly.
 */

#include <stdint.h>
#include <stdbool.h>

#define PD_FPS           60
#define PD_RATE          35200      /* every instrument's sample rate: A4 is 80 samples */
#define PD_ROWS_PER_BAR  16
#define PD_BARS          32
#define PD_SONG_ROWS     (PD_BARS * PD_ROWS_PER_BAR)
#define PD_CHANNELS      8

/* frame <-> row, at 128 BPM */
static inline int pd_row_of(uint32_t frame) { return (int)(frame * 32u / 225u); }
static inline uint32_t pd_frame_of_row(int row) { return ((uint32_t)row * 225u + 31u) / 32u; }
static inline uint32_t pd_frame_of_bar(int bar) { return pd_frame_of_row(bar * PD_ROWS_PER_BAR); }

/* The song's last frame, the final chord's ring included. */
#define PD_END_FRAME (pd_frame_of_row(PD_SONG_ROWS) + 150u)

/* The instruments. */
enum {
	PD_I_KICK, PD_I_SNARE, PD_I_CLAP, PD_I_HATC, PD_I_HATO,
	PD_I_BASS, PD_I_PLUCK, PD_I_LEAD, PD_I_PAD,
	PD_I_THUNDER, PD_I_TICK, PD_I_CLATTER, PD_I_NOISE, PD_I_BELL,
	PD_I_COUNT
};

/* Synthesize the instruments and take the mixer. False with no mixer
 * (the film plays silently); the instruments are made either way. */
bool pdm_init(void);
void pdm_shutdown(void);

/* Bring the music to film frame `frame`: every row due since the last
 * call, then this frame's envelopes. Call once a frame. */
void pdm_frame(uint32_t frame);

/* For the pictures: how loud channel ch is now (0..255), and the frame
 * the last note on it started. */
int pdm_level(int ch);
uint32_t pdm_last_hit(int ch);

/* The channels' parts, for the pictures */
enum { PD_CH_KICK, PD_CH_SNARE, PD_CH_HAT, PD_CH_BASS, PD_CH_ARP,
	PD_CH_LEAD, PD_CH_PAD, PD_CH_FX };

/* -- what the synth and the song share (pd_synth.c, pd_song.c) -- */

typedef struct {
	int16_t *d;
	uint32_t n;             /* frames */
	uint32_t loop_st, loop_n;   /* frames; loop_n 0: once */
	int base_note;          /* MIDI note it sounds at, played at PD_RATE */
} pd_sample_t;

extern pd_sample_t pd_samples[PD_I_COUNT];
bool pd_synth_all(void);

/* One note of the song. */
typedef struct {
	uint16_t row;
	uint8_t ch, inst;
	int8_t note;            /* MIDI */
	uint8_t vel;            /* 0..127 */
	uint8_t len;            /* rows; 0: as long as the sample */
	int8_t slide;           /* semitones to slide by over the note */
} pd_note_t;

/* The song, built at start-up (pd_song.c): sorted by row. */
int pd_song_build(pd_note_t *out, int cap);

#endif
