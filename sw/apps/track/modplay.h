#ifndef MODPLAY_H
#define MODPLAY_H

/*
 * modplay -- ProTracker MOD playback engine
 *
 * Parses a .mod from a memory buffer and renders 16-bit signed stereo
 * frames into a caller-supplied buffer. That is the whole interface.
 *
 * -- DELIBERATELY FREE OF ZEITLOS --
 *
 * This file and modplay.c include nothing from sw/common and touch no
 * hardware. Not tidiness: it is what lets the mixer be compiled with
 * the host compiler and tested against a known input on a machine
 * where a wrong answer can be printed rather than listened to (see
 * modplay_test.c and `make test`). Every bug found that way is a bug
 * not chased on hardware with an oscilloscope.
 *
 * The app half -- file loading, the audio FIFO, the keyboard -- lives
 * in mod.c and knows about Zeitlos. Keep the split.
 *
 * -- PHASE 2 OF THE AUDIO SUBSYSTEM --
 *
 * All mixing here is in software. On a 48MHz picorv32 four channels at
 * 22kHz costs roughly 15% of the CPU, which is fine for a player and
 * not fine for a game that is also filling a 320x240 back buffer.
 * Phase 3 moves the inner loop into hardware; this engine is the
 * reference it gets designed against, which is the entire reason it
 * comes first. See docs/audio.md.
 *
 * -- WHAT IT SUPPORTS --
 *
 * 31-instrument ProTracker modules: M.K., M!K!, FLT4, and the xCHN
 * family up to 8 channels. 15-instrument (Soundtracker) modules are
 * rejected rather than guessed at -- they have no magic word, so
 * "detecting" one means assuming any file that is not a MOD is one.
 *
 * Effects: 0 arpeggio, 1/2 portamento, 3 tone portamento, 4 vibrato,
 * 5/6 the two combinations, 9 sample offset, A volume slide, B
 * position jump, C set volume, D pattern break, E1/E2 fine
 * portamento, E9 retrigger, EA/EB fine volume slide, EC note cut, EE
 * pattern delay, F speed/tempo.
 *
 * Not supported, and silently ignored: 7 tremolo, 8/E8 panning, E0
 * filter, E3 glissando, E4 vibrato waveform, E5 finetune, E6 pattern
 * loop, ED note delay. Nothing in that list is silent-breaking; a
 * module using them plays, slightly wrong.
 */

#include <stdint.h>

#define MOD_MAX_CHANNELS 8
#define MOD_MAX_SAMPLES  31

/* modplay_init() return codes. Distinct rather than a bare 0/-1
 * because "this file is not a MOD" and "this file is a MOD I cannot
 * play" want different things said to the user. */
#define MOD_OK              0
#define MOD_ERR_TOO_SMALL  -1
#define MOD_ERR_NOT_MOD    -2   /* no recognised magic at offset 1080 */
#define MOD_ERR_CHANNELS   -3   /* more channels than MOD_MAX_CHANNELS */
#define MOD_ERR_TRUNCATED  -4   /* header promises more data than exists */

typedef struct {
	const int8_t *data;      /* into the caller's file buffer, not owned */
	uint32_t length;         /* bytes */
	uint32_t loop_start;     /* bytes */
	uint32_t loop_len;       /* bytes; <= 2 means "no loop" */
	uint8_t volume;          /* 0..64 */
	uint8_t finetune;        /* raw nibble, 0..15; 8..15 are -8..-1 */
	uint32_t offset;         /* byte offset of `data` within the file */
} mod_sample_t;

typedef struct {
	const int8_t *data;
	uint32_t length;
	uint32_t loop_start;
	uint32_t loop_len;

	uint32_t pos;            /* fixed point, MOD_FRAC_BITS fraction */
	uint32_t step;
	int active;

	int16_t period;          /* base period, before vibrato/arpeggio */
	int16_t porta_target;
	uint8_t volume;          /* 0..64 */
	uint8_t sample;          /* 1..31, 0 = none selected yet */
	uint8_t finetune;

	uint16_t pan_l;          /* 0..256 */
	uint16_t pan_r;

	/* volume * pan, precomputed once per tick by update_gains().
	 * The inner mix loop is the hot path on a 48MHz core with no
	 * instruction cache, and folding the pan multiply and its shift
	 * into a per-tick calculation takes one multiply and two shifts
	 * per channel per SAMPLE down to two multiplies -- see the
	 * mixing notes in modplay.c. */
	int32_t gain_l;
	int32_t gain_r;

	/* per-effect memory. ProTracker keeps these per channel and reuses
	 * them when a parameter of 0 is given, which a lot of modules rely
	 * on -- dropping them makes portamentos stop halfway. */
	uint8_t effect;
	uint8_t param;
	uint8_t porta_speed;
	uint8_t vib_speed;
	uint8_t vib_depth;
	uint8_t vib_pos;
	uint8_t offset_mem;
	uint8_t retrig_mem;

	/* set by trigger(), consumed by modplay_hw_channel() */
	uint8_t hw_trigger;
	uint8_t hw_offset;
} mod_channel_t;

typedef struct {
	const uint8_t *file;
	uint32_t file_len;

	char name[21];
	int channels;
	int song_len;            /* entries used in order[] */
	uint8_t order[128];
	int num_patterns;
	const uint8_t *patterns; /* base of pattern data */
	uint32_t pattern_bytes;  /* one pattern, = 64 * channels * 4 */

	mod_sample_t samples[MOD_MAX_SAMPLES + 1];  /* 1-based */

	uint32_t rate;           /* output sample rate, Hz */
	int speed;               /* ticks per row (Fxx below 32) */
	int bpm;                 /* Fxx 32 and above */
	uint32_t samples_per_tick;

	int tick;                /* tick within the current row */
	int row_ticks;           /* speed, times pattern delay */
	int row;
	int order_pos;
	uint32_t tick_remaining; /* frames left in this tick */

	int break_row;           /* Dxx pending, or -1 */
	int jump_order;          /* Bxx pending, or -1 */

	int master;              /* mix scale, see modplay.c */
	int separation;          /* 0..256 */
	uint32_t loops;          /* incremented each time the song wraps */

	mod_channel_t ch[MOD_MAX_CHANNELS];
} mod_player_t;

/*
 * Parse `file` (which the caller owns and must keep alive for as long
 * as the player is used -- sample data is referenced in place, never
 * copied) and prepare to play at `rate` Hz.
 *
 * Returns MOD_OK or one of the MOD_ERR_* codes above.
 */
int modplay_init(mod_player_t *m, const void *file, uint32_t len,
	uint32_t rate);

/*
 * Render `frames` stereo frames into `out`, interleaved left then
 * right. Always fills the whole buffer; a finished song loops rather
 * than stopping, and m->loops counts how many times it has.
 */
void modplay_render(mod_player_t *m, int16_t *out, int frames);

/*
 * Stereo separation, 0 (mono) to 100 (hard Amiga LRRL panning).
 * Defaults to 65, because hard panning is what the Amiga did through
 * speakers across a room and is unpleasant on headphones.
 */
void modplay_set_separation(mod_player_t *m, int percent);

/*
 * Recompute everything that depends on the output sample rate, after
 * the caller has changed m->rate.
 *
 * Exists so the rate can be changed DURING playback without
 * reinitialising -- which would restart the module. Both the tick
 * length and every channel's current step depend on the rate, and
 * updating only one of them is the kind of half-change that produces
 * correct pitch at the wrong tempo.
 */
void modplay_retempo(mod_player_t *m);

/*
 * ---- HARDWARE MIXER MODE (phase 3) ----
 *
 * The same engine, driving rtl/audio_mixer.v instead of mixing into a
 * buffer. Tracker logic -- patterns, effects, tempo -- is identical
 * and shared; only the last step differs.
 *
 * That sharing is the point. The alternative, a second player written
 * against the hardware, would mean every effect implemented twice and
 * two things to keep in agreement; a bug fixed in one would live on in
 * the other. Here the SAME do_tick() runs, and the only question is
 * what happens to the resulting channel state.
 *
 * modplay_advance() runs the tracker forward by one tick and returns
 * non-zero if anything changed that the hardware needs to be told
 * about. The caller (mod.c) then writes the channel registers. There
 * is no per-sample work anywhere in this path.
 *
 * Call it once per tick. m->samples_per_tick frames elapse per call,
 * so the caller paces it off the hardware's own frame counter rather
 * than a wall clock -- see mod.c.
 */
void modplay_advance(mod_player_t *m);

/*
 * What the hardware needs for one channel, after modplay_advance().
 *
 * `trigger` is set only on a real note start. It is separate from
 * `enable` because a tracker changes volume mid-note far more often
 * than it starts one, and restarting the sample on every volume change
 * is the classic way a hardware channel ends up sounding clicky.
 */
typedef struct {
	uint32_t base;        /* byte offset of sample data within the file */
	uint32_t length;
	uint32_t loop_start;
	uint32_t loop_len;
	uint32_t sample_hz;   /* playback rate implied by the current period */
	uint8_t gain_l;       /* 0..255 */
	uint8_t gain_r;
	uint8_t enable;
	uint8_t trigger;      /* consumed by the caller; cleared on read */
	uint8_t offset;       /* 9xx start offset, units of 256 bytes */
} mod_hw_channel_t;

/*
 * Fill `out` for channel `c` and clear its pending trigger.
 *
 * `base` is a byte offset into the module file, NOT an address: the
 * engine has no idea where the caller put the file. mod.c adds its own
 * buffer address. Keeping it that way is what lets the host tests run
 * this path with no hardware and no address space at all.
 */
void modplay_hw_channel(mod_player_t *m, int c, mod_hw_channel_t *out);

/* Human-readable form of a MOD_ERR_* code. */
const char *modplay_strerror(int err);

/*
 * One decoded pattern cell, for a tracker-style display.
 *
 * The engine is the only thing in this app that knows the on-disk
 * layout, and it stays that way: the UI asks for a cell rather than
 * indexing pattern bytes itself. That is not ceremony -- a second
 * copy of "sample number is split across two nibbles in bytes 0 and
 * 2" is exactly the kind of duplicate that rots.
 */
typedef struct {
	int note;        /* 0..35 (C-1..B-3), or -1 for no note */
	int sample;      /* 1..31, or 0 for none */
	int effect;      /* 0..15 */
	int param;       /* 0..255 */
} mod_cell_t;

/*
 * Decode one cell. `order_pos` indexes the ORDER table, not the
 * pattern table, so this takes the same coordinates the player
 * reports in m->order_pos / m->row. Out-of-range arguments yield an
 * empty cell rather than reading past the buffer.
 */
void modplay_get_cell(const mod_player_t *m, int order_pos, int row,
	int chan, mod_cell_t *out);

/*
 * Three-character note name for a cell's note field ("C-2", "A#3"),
 * or "---" for no note. Writes 4 bytes including the terminator.
 */
void modplay_note_name(int note, char *out4);

#endif
