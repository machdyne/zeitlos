#ifndef ZSFX_H
#define ZSFX_H

/*
 * Zeitlos -- sound effects and music for games, on the hardware mixer.
 * See docs/sfx.md.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * gamedemo's music.c, made general: the same trick (an instrument is
 * one looped cycle of a waveform; pitch is the playback rate), the same
 * ASCII scores, the same per-frame envelopes -- plus effects that are
 * little programs instead of pre-rendered buffers, so they sweep, step
 * through arpeggios and pan, and cost no RAM per effect.
 *
 * -- channels --
 *
 *   0..3   music (a song has up to four channels)
 *   4..6   effects, and 7 as well unless speech is running
 *          (Z_AUDIO_CH_SPEECH, docs/tts.md)
 *
 * Like track, play and midi, a game takes the mixer while it runs: it
 * clears the channels it uses and leaves channel 7 alone for speech.
 *
 * -- effects --
 *
 * An effect is up to eight segments; each holds one waveform for some
 * frames and slides linearly from (f0, v0) to (f1, v1):
 *
 *     static const zsfx_seg_t eat_seg[] = {
 *         { ZSFX_SQUARE, 3,  659,  659, 200, 160 },   // E5
 *         { ZSFX_SQUARE, 3,  784,  784, 180, 140 },   // G5
 *         { ZSFX_SQUARE, 6, 1047, 1047, 160,   0 },   // C6, fading
 *     };
 *     static const zsfx_fx_t eat = { 2, 3, eat_seg };
 *     zsfx_play(&eat, pan);       // pan -128 (left) .. 127 (right)
 *
 * Frequencies are in Hz (noise: the pitch of the noise); levels 0..255.
 * A segment with frames 0 is skipped. Priority decides which effect a
 * new one may cut off when every effect channel is busy.
 *
 * -- music --
 *
 * Patterns of 16 rows, one string per row, one 3-character cell per
 * channel separated by spaces, as in sw/apps/gamedemo/music.c:
 *
 *     "C-4 E-4 A-2 K--"
 *
 *   C-4 / C#4    a note (octave 4 holds A-4 = 440Hz)
 *   ---          hold
 *   ===          note off
 *   K-- S-- H-- O--    drums, on a ZSFX_DRUMS channel: kick, snare,
 *                      closed hat, open hat
 *
 * An order string lists the patterns to play, as digits and letters
 * ("0011223" -- pattern 'a' is 10); it loops.
 *
 * Silent, never failing, on a board without the mixer: every call is
 * then a no-op and zsfx_init() says so.
 */

#include <stdint.h>
#include <stdbool.h>

enum {
	ZSFX_SQUARE = 0,   /* 50% -- leads, beeps */
	ZSFX_PULSE25,      /* thinner */
	ZSFX_PULSE12,      /* thinner still, nasal */
	ZSFX_TRI,          /* soft: bass, flutes */
	ZSFX_SAW,          /* bright, buzzy */
	ZSFX_SINE,         /* pure */
	ZSFX_NOISE,        /* hiss, crashes, drums */
	ZSFX_WAVES,
	ZSFX_DRUMS = 0x80  /* instrument flag: cells are drum letters */
};

typedef struct {
	uint8_t wave;
	uint8_t frames;
	uint16_t f0, f1;     /* Hz */
	uint8_t v0, v1;      /* 0..255 */
} zsfx_seg_t;

typedef struct {
	uint8_t prio;        /* higher wins a channel */
	uint8_t n;           /* segments, 1..8 */
	const zsfx_seg_t *seg;
} zsfx_fx_t;

typedef struct {
	uint8_t wave;        /* ZSFX_*, or ZSFX_DRUMS */
	uint8_t gain;        /* balance against the other channels */
	uint8_t decay;       /* per frame, x/256; 255 rings, 200 is a pluck */
	uint8_t vib;         /* vibrato depth, 0 none, ~6 gentle */
} zsfx_ins_t;

typedef struct {
	uint16_t rows_per_min;     /* tempo: 480 = 8 rows a second */
	uint8_t nch;               /* 1..4 */
	zsfx_ins_t ins[4];
	const char *const *rows;   /* npat * 16 row strings */
	uint8_t npat;
	const char *order;         /* "0123..." */
} zsfx_song_t;

/* Probe and take the mixer. False if there is none (then silent). */
bool zsfx_init(void);
bool zsfx_ok(void);

/* Once a frame, with the frames elapsed. zarcade's ar_frame() calls
 * it; a game that is not on zarcade calls it itself. */
void zsfx_tick(uint32_t frames);

void zsfx_play(const zsfx_fx_t *fx, int pan);
void zsfx_stop_all(void);

/* Pan for an x position across a 320-wide picture. */
int zsfx_pan_x(int x);

void zsfx_music_play(const zsfx_song_t *s);
void zsfx_music_stop(void);
/* The M key: music on or off, remembered while the game runs. A song
 * started while off waits silently and starts when turned on. */
void zsfx_music_enable(bool on);
bool zsfx_music_enabled(void);
/* 100 = as written; 150 half as fast again. */
void zsfx_music_tempo(int percent);
/* 0..255: music against effects (255 = the gains as written). */
void zsfx_music_volume(uint8_t v);
/* Rows played since the song started (-1 with no song playing): a beat
 * to pulse a picture to. Advances whether or not the music is muted. */
int32_t zsfx_music_rows(void);

/* Silence everything and hold (help box, pause); false resumes. */
void zsfx_suspend(bool on);

/* Silence and release the mixer. Call before exiting: a channel left
 * running goes on reading memory the process no longer owns. */
void zsfx_shutdown(void);

/* -- for the host tests --
 *
 * Note cells and drum letters as the player reads them. Frequencies in
 * centi-hertz; 0 hold, ZSFX_CELL_OFF note off, ZSFX_CELL_DRUM+n drum n.
 */
#define ZSFX_CELL_HOLD 0u
#define ZSFX_CELL_OFF  0xFFFFFFFFu
#define ZSFX_CELL_DRUM 0xFFFFFF00u
uint32_t zsfx_parse_cell(const char *c);
int zsfx_order_len(const char *order);
int zsfx_order_at(const char *order, int i);

#endif
