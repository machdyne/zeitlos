/*
 * gamedemo -- music and sound effects.
 *
 * See music.h for how a note becomes a sound. This file is the score
 * and the player.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zaudio.h"

#include "music.h"

/* -- THE SCORE ------------------------------------------------------
 *
 * ASCII, like level.c, and read the same way: top to bottom is time,
 * left to right is the four channels.
 *
 *   lead  harmony  bass  percussion
 *
 * Each cell is exactly three characters, separated by one space:
 *
 *   C-4   note C, octave 4
 *   C#4   C sharp, octave 4
 *   ---   hold whatever is playing (no new note)
 *   ===   note off (let the envelope finish immediately)
 *
 * Percussion uses pitch to pick a drum, because the instrument is
 * noise and pitch is the only knob that matters:
 *
 *   C-2   kick    (low, slow decay)
 *   C-4   snare   (mid)
 *   C-6   hat     (high, fast decay)
 *
 * Octave 4 is the one containing A-4 = 440Hz, as usual.
 *
 * Sixteen rows to a pattern, four patterns, at 8 rows per second --
 * so a pattern is two seconds and the loop is eight. Short on purpose:
 * this plays continuously under a game and the failure mode of game
 * music is not being boring, it is being ANNOYING, and a short loop
 * that stays out of the way beats a long one that demands attention.
 *
 * The tune is in A minor, and the bass moves A - F - C - G. Nothing
 * clever; it just has to be inoffensive for the twentieth repeat.
 */

#define MUS_ROWS_PER_PATTERN 16
#define MUS_PATTERNS 4
#define MUS_TOTAL_ROWS (MUS_ROWS_PER_PATTERN * MUS_PATTERNS)

static const char *const song_main[MUS_TOTAL_ROWS] = {
/*   lead harm bass perc */
	"A-4 A-3 A-2 C-2",
	"--- --- --- ---",
	"C-5 C-4 --- C-6",
	"--- --- --- ---",
	"E-5 E-4 A-2 C-4",
	"--- --- --- ---",
	"C-5 C-4 --- C-6",
	"--- --- --- ---",
	"A-4 A-3 A-2 C-2",
	"--- --- --- ---",
	"B-4 D-4 --- C-6",
	"--- --- --- ---",
	"C-5 E-4 A-2 C-4",
	"--- --- --- ---",
	"=== === --- C-6",
	"--- --- --- ---",

	"F-4 A-3 F-2 C-2",
	"--- --- --- ---",
	"A-4 C-4 --- C-6",
	"--- --- --- ---",
	"C-5 F-4 F-2 C-4",
	"--- --- --- ---",
	"A-4 C-4 --- C-6",
	"--- --- --- ---",
	"F-4 A-3 F-2 C-2",
	"--- --- --- ---",
	"G-4 B-3 --- C-6",
	"--- --- --- ---",
	"A-4 C-4 F-2 C-4",
	"--- --- --- ---",
	"=== === --- C-6",
	"--- --- --- ---",

	"C-5 E-4 C-3 C-2",
	"--- --- --- ---",
	"E-5 G-4 --- C-6",
	"--- --- --- ---",
	"G-5 C-5 C-3 C-4",
	"--- --- --- ---",
	"E-5 G-4 --- C-6",
	"--- --- --- ---",
	"C-5 E-4 C-3 C-2",
	"--- --- --- ---",
	"B-4 D-4 --- C-6",
	"--- --- --- ---",
	"A-4 C-4 C-3 C-4",
	"--- --- --- ---",
	"=== === --- C-6",
	"--- --- --- ---",

	"G-4 B-3 G-2 C-2",
	"--- --- --- ---",
	"B-4 D-4 --- C-6",
	"--- --- --- ---",
	"D-5 G-4 G-2 C-4",
	"--- --- --- ---",
	"B-4 D-4 --- C-6",
	"--- --- --- ---",
	"E-5 G-4 G-2 C-2",
	"--- --- --- ---",
	"D-5 B-3 --- C-6",
	"--- --- --- ---",
	"C-5 E-4 G-2 C-4",
	"--- --- --- ---",
	"=== === --- C-6",
	"--- --- --- ---",
};

/* Which instrument each channel plays. Fixed per channel rather than
 * settable per row -- a row would then need a fifth field and this
 * demo has no use for one. */
static const uint8_t chan_ins[MUS_CHANNELS] = {
	INS_SQUARE, INS_PULSE, INS_TRI, INS_NOISE
};

/* Decay per frame, as a numerator over 256. Lower decays faster.
 *
 * The lead rings, the bass rings longer (it is holding the harmony
 * together), and percussion is nearly a click. */
static const uint8_t chan_decay[MUS_CHANNELS] = { 244, 240, 250, 200 };

/* Per-channel gains -- the BALANCE between the four instruments, not
 * how loud the music is. Scaled by music_vol before use, so changing
 * the overall music level does not disturb the mix between parts.
 *
 * These sum to 85. That number matters: it is what SFX_GAIN below is
 * chosen against, and it is what keeps the music at roughly 6% of full
 * scale so an effect has somewhere to land above it. */
static const uint8_t chan_vol[MUS_CHANNELS] = { 24, 15, 28, 18 };

/* Sound effect gain, scaled by sfx_vol before use.
 *
 * -- WHY THIS IS NEARLY TWICE THE WHOLE MUSIC MIX --
 *
 * A sound effect is ONE channel. The music is FOUR, sounding together.
 * So the comparison that matters is this gain against the SUM of
 * chan_vol above, not against any single entry in it.
 *
 * The first version had chan_vol summing to 350 against an effect gain
 * of 140 -- so the music was two and a half times louder than any
 * effect, and every jump and coin simply disappeared underneath it.
 * The instruments each looked reasonable next to 140, which is exactly
 * how the mistake survived being looked at.
 *
 * At 255 against a music sum of 85 an effect is three times the whole
 * mix -- about 10dB up -- and in practice better than that, because
 * music channels spend most of their time part-way through an envelope
 * decay while an effect is always heard from its own peak.
 *
 * 255 is also the ceiling: one 8-bit channel simply cannot get loud on
 * this mixer. Full scale needs roughly four channels at full gain
 * (rtl/audio_mixer.v says as much), so a lone effect tops out near 20%
 * of full scale no matter what. That is why the answer here was to
 * bring the MUSIC down rather than push the effects up -- there was no
 * headroom left to push into. */
#define SFX_GAIN 255

/* -- instruments ---------------------------------------------------- */

#define WAVE_LEN 64
#define NOISE_LEN 256

static int8_t wave_square[WAVE_LEN];
static int8_t wave_pulse[WAVE_LEN];
static int8_t wave_tri[WAVE_LEN];
static int8_t wave_noise[NOISE_LEN];

static const int8_t *ins_data[INS_COUNT];
static uint16_t ins_len[INS_COUNT];

/* -- sound effects --------------------------------------------------
 *
 * One-shots, not loops. Same 8-bit signed format; see gamedemo.c's own
 * note on what happens if you make these 16-bit. */
#define SFX_LEN 512
static int8_t sfx_buf[SFX_COUNT][SFX_LEN];

/* -- player state --------------------------------------------------- */

static bool audio_ok;
static bool playing;

static int cur_row;
static uint32_t frame_accum;    /* frames since the last row, x256 */
static uint32_t frames_per_row_x256;

static uint8_t ch_vol[MUS_CHANNELS];

/* Independent music and effect levels, 0..255, where 255 means the
 * gains above exactly as written. Separate from the master volume
 * (MIXVOL) on purpose: the master answers "how loud is the game", and
 * these two answer "how loud is the music AGAINST the effects", which
 * is a different question and the one that actually needed fixing
 * here. */
static uint8_t music_vol = MUS_MUSIC_VOL_DEFAULT;
static uint8_t sfx_vol = MUS_SFX_VOL_DEFAULT;

/* App virtual address -> physical.
 *
 * THE MIXER IS A BUS MASTER AND DOES NOT GO THROUGH THE MTU. Handing
 * it an app pointer does not fail quietly: it requests a physical
 * address nothing decodes, an undecoded address on this bus never
 * acks, and the mixer holds its grant on rtl/arbiter_main.v forever.
 * The CPU is starved of the main bus and the whole machine stops.
 *
 * Same translation sw/apps/track's phys_of() does. */
static uint32_t phys_of(const void *p) {
	uint32_t v = (uint32_t)(uintptr_t)p;
	uint32_t base = reg_mtu_base;
	if (base == 0) return v;
	if ((v & 0xF0000000u) != 0x80000000u) return v;
	return base + (v & 0x0FFFFFFFu);
}

/* -- note names to frequency ---------------------------------------
 *
 * Twelve-tone equal temperament, as a table rather than a pow() call:
 * this runs on a core whose floating point is libgcc, and a table of
 * twelve entries is smaller and exact.
 *
 * These are octave-4 frequencies in centi-hertz (so A-4 = 44000),
 * because integer hertz would put C-2 at 65 and lose enough pitch
 * accuracy in the low octaves to be audible against the bass. Higher
 * octaves shift left, lower shift right. */
static const uint32_t note_chz[12] = {
	26163,  /* C  */
	27718,  /* C# */
	29366,  /* D  */
	31113,  /* D# */
	32963,  /* E  */
	34923,  /* F  */
	36999,  /* F# */
	39200,  /* G  */
	41530,  /* G# */
	44000,  /* A  */
	46616,  /* A# */
	49388   /* B  */
};

/* Parse one three-character cell.
 *
 * Returns the frequency in centi-hertz, 0 for "hold", and (uint32_t)-1
 * for "note off". Three outcomes from one return value because the
 * caller has three cases and a struct here would be more code than the
 * thing it describes. */
#define CELL_HOLD 0u
#define CELL_OFF  0xFFFFFFFFu

static uint32_t parse_cell(const char *c) {

	int semitone;
	int octave;
	uint32_t f;

	if (c[0] == '-') return CELL_HOLD;
	if (c[0] == '=') return CELL_OFF;

	switch (c[0]) {
		case 'C': semitone = 0; break;
		case 'D': semitone = 2; break;
		case 'E': semitone = 4; break;
		case 'F': semitone = 5; break;
		case 'G': semitone = 7; break;
		case 'A': semitone = 9; break;
		case 'B': semitone = 11; break;
		default: return CELL_HOLD;
	}

	if (c[1] == '#') semitone++;
	if (semitone > 11) semitone = 11;

	octave = c[2] - '0';
	if (octave < 0) octave = 0;
	if (octave > 9) octave = 9;

	f = note_chz[semitone];

	/* Shift by octaves. Doubling per octave, so this is exact -- no
	 * rounding accumulates the way repeated multiplication by a
	 * ratio would. */
	if (octave > 4) f <<= (octave - 4);
	else if (octave < 4) f >>= (4 - octave);

	return f;

}

/* -- waveform synthesis --------------------------------------------- */

static void build_waves(void) {

	int i;

	for (i = 0; i < WAVE_LEN; i++) {

		/* 50% duty. Peak 100 rather than 127 so four channels summing
		 * at these gains cannot clip the mixer's accumulator. */
		wave_square[i] = (i < WAVE_LEN / 2) ? 100 : -100;

		/* 25% duty -- same fundamental, different harmonics, which is
		 * what makes it audibly a different instrument rather than a
		 * quieter copy of the square. */
		wave_pulse[i] = (i < WAVE_LEN / 4) ? 100 : -100;

		/* Triangle: up for the first half, down for the second. Few
		 * harmonics, so it stays out of the lead's way in the mix. */
		if (i < WAVE_LEN / 2)
			wave_tri[i] = (int8_t)(-100 + (200 * i) / (WAVE_LEN / 2));
		else
			wave_tri[i] = (int8_t)(100 - (200 * (i - WAVE_LEN / 2)) /
				(WAVE_LEN / 2));

	}

	/* 16-bit maximal LFSR, taps 16/14/13/11 -- period 65535, so a
	 * 256-byte window of it has no audible repeat at the rates this
	 * plays back at. A rand() here would be both larger and less
	 * predictable across builds. */
	{
		uint16_t lfsr = 0xACE1u;
		for (i = 0; i < NOISE_LEN; i++) {
			uint16_t bit = ((lfsr >> 0) ^ (lfsr >> 2) ^
			                (lfsr >> 3) ^ (lfsr >> 5)) & 1u;
			lfsr = (uint16_t)((lfsr >> 1) | (bit << 15));
			wave_noise[i] = (int8_t)((lfsr & 0xFF) - 128);
		}
	}

	ins_data[INS_SQUARE] = wave_square; ins_len[INS_SQUARE] = WAVE_LEN;
	ins_data[INS_PULSE]  = wave_pulse;  ins_len[INS_PULSE]  = WAVE_LEN;
	ins_data[INS_TRI]    = wave_tri;    ins_len[INS_TRI]    = WAVE_LEN;
	ins_data[INS_NOISE]  = wave_noise;  ins_len[INS_NOISE]  = NOISE_LEN;

}

static void build_sfx(void) {

	int s, i;

	for (s = 0; s < SFX_COUNT; s++) {

		int phase = 0;
		int sign = 1;

		for (i = 0; i < SFX_LEN; i++) {

			/* Period in samples, swept over the effect's length. A
			 * jump sweeps UP and a hit sweeps DOWN, which is the
			 * whole of the character here -- the waveform is the same
			 * square in every case. */
			int p;
			switch (s) {
				case SFX_JUMP: p = 60 - (i * 40) / SFX_LEN; break;
				case SFX_COIN: p = 24; break;
				case SFX_HIT:  p = 40 + (i * 80) / SFX_LEN; break;
				default:       p = 60; break;
			}
			if (p < 4) p = 4;

			if (++phase >= p) { phase = 0; sign = -sign; }

			{
				int amp = 100 - (100 * i) / SFX_LEN;
				sfx_buf[s][i] = (int8_t)(sign * amp);
			}

		}

	}

}

/* -- mixer plumbing ------------------------------------------------- */

static void ch_silence(int ch) {
	Z_AUDIO_CH_CTRL(ch) = z_audio_ch_ctrl(0, 0, false, false, 0);
}

/* Start a note on a music channel.
 *
 * The step is what turns a looped single cycle into a pitch. One cycle
 * is ins_len samples, so to hear `freq` the mixer must consume
 * freq * ins_len samples per second -- that is the "sample rate" to
 * hand z_audio_step(), which converts it to the 18.14 phase increment
 * the hardware wants against the board's ACTUAL output rate.
 *
 * Reading the output rate back rather than assuming it is what lets
 * this play in tune on a board running 22kHz or S/PDIF's 16 divider. */
static void ch_note(int ch, uint32_t freq_chz) {

	int ins = chan_ins[ch];
	uint32_t rate;

	if (freq_chz == 0) return;

	/* centi-hertz * length / 100, ordered to keep the intermediate in
	 * range: 49388 * 256 is 12.6M, comfortably inside 32 bits, and
	 * dividing last preserves the precision the centi-hertz table
	 * exists to provide. */
	rate = (freq_chz * ins_len[ins]) / 100u;

	Z_AUDIO_CH_BASE(ch)    = phys_of(ins_data[ins]);
	Z_AUDIO_CH_LEN(ch)     = ins_len[ins];
	Z_AUDIO_CH_LOOPST(ch)  = 0;
	Z_AUDIO_CH_LOOPLEN(ch) = ins_len[ins];   /* loop forever */
	Z_AUDIO_CH_STEP(ch)    = z_audio_step(rate, z_audio_rate_hz());

	ch_vol[ch] = (uint8_t)(((uint32_t)chan_vol[ch] * music_vol) >> 8);
	if (ch_vol[ch] == 0 && music_vol != 0) ch_vol[ch] = 1;

	Z_AUDIO_CH_CTRL(ch) = z_audio_ch_ctrl(ch_vol[ch], ch_vol[ch],
		true, true, 0);

}

/* -- public --------------------------------------------------------- */

bool music_init(void) {

	int i;

	audio_ok = false;
	playing = false;

	if (!z_audio_present() || !z_audio_mixer_present()) return false;

	build_waves();
	build_sfx();

	/* ENABLE THE OUTPUT. This is not optional and its absence is
	 * silent: z_audio_start() is what sets Z_AUDIO_CTRL_EN, and
	 * without EN the DAC is muted no matter how many channels the
	 * mixer is happily summing. An earlier version of this file
	 * dropped the call in order to avoid overriding the board's rate,
	 * and the result was a game that looked like it had no audio
	 * hardware at all.
	 *
	 * The rate is read back and written straight out again, so the
	 * board keeps whatever `AUDIO_RATE_RESET gave it -- which matters,
	 * because an S/PDIF board needs 16 and a constant here would break
	 * exactly those boards. Enable the output; do not second-guess
	 * the rate. */
	z_audio_start(reg_audio_rate & 0xFFu);

	z_audio_mixer_enable(true);

	music_set_volume(MUS_VOL_DEFAULT);
	music_vol = MUS_MUSIC_VOL_DEFAULT;
	sfx_vol = MUS_SFX_VOL_DEFAULT;

	for (i = 0; i < MUS_CHANNELS + SFX_CHANNELS; i++) ch_silence(i);
	for (i = 0; i < MUS_CHANNELS; i++) ch_vol[i] = 0;

	/* Rows per second -> frames per row, against the DISPLAY's real
	 * frame rate. A PAL composite board runs at 50Hz, and deriving
	 * tempo from a fixed frame count would make every song 20% slow
	 * there. x256 so the accumulator below keeps the fraction. */
	{
		uint32_t hz = z_video_frame_hz();
		if (hz == 0) hz = 60;
		frames_per_row_x256 = (hz * 256u) / 8u;    /* 8 rows/second */
	}

	cur_row = 0;
	frame_accum = 0;

	audio_ok = true;
	return true;

}

/* -- master volume --------------------------------------------------
 *
 * THE one volume knob. Scales everything the mixer produces -- music
 * and effects together -- because it is the mixer's own output stage:
 *
 *     out = clamp16((sum_of_channels * mixvol) >> 10)
 *
 * The hardware resets this to 128, which rtl/audio_mixer.v's header
 * describes as safe for all eight channels (255 is the setting a
 * 4-channel MOD wants). So MUS_VOL_DEFAULT of 64 is half the hardware
 * default, which is the 50% cut without touching a single per-channel
 * gain.
 *
 * READ-MODIFY-WRITE, and this is not optional. Bits [11:8] of the same
 * register hold the S/PDIF fs_code (see docs/audio.md's register
 * table), so writing a bare volume here would zero the sample-rate
 * code and break digital output on every board that has it -- while
 * sounding perfectly fine on the analogue board it was tested on,
 * which is the worst way for a bug like that to behave.
 */
void music_set_music_volume(uint8_t vol) {
	music_vol = vol;
	/* Takes effect on the next note rather than immediately. The
	 * envelope of a note already sounding runs to its end at the old
	 * level, which is at most a fraction of a second and is far less
	 * jarring than every voice jumping mid-note. */
}

uint8_t music_get_music_volume(void) { return music_vol; }

void music_set_sfx_volume(uint8_t vol) { sfx_vol = vol; }
uint8_t music_get_sfx_volume(void) { return sfx_vol; }

void music_set_volume(uint8_t vol) {
	uint32_t v = reg_audio_mixvol;
	reg_audio_mixvol = (v & ~0xFFu) | vol;
}

uint8_t music_get_volume(void) {
	return (uint8_t)(reg_audio_mixvol & 0xFFu);
}

void music_play(int song) {
	(void)song;              /* one song for now */
	if (!audio_ok) return;
	cur_row = 0;
	frame_accum = 0;
	playing = true;
}

void music_stop(void) {
	int i;
	if (!audio_ok) return;
	playing = false;
	for (i = 0; i < MUS_CHANNELS; i++) {
		ch_vol[i] = 0;
		ch_silence(i);
	}
}

void music_tick(uint32_t frames) {

	int ch;

	if (!audio_ok) return;

	/* Envelopes decay every frame whether or not a row lands, which is
	 * what makes a note fade rather than step. Written with EN but NOT
	 * TRIG -- docs/audio.md is explicit that this changes gain without
	 * restarting the sample, which is exactly what is wanted here and
	 * the reason a volume envelope costs one register write. */
	for (ch = 0; ch < MUS_CHANNELS; ch++) {

		uint32_t v;

		if (ch_vol[ch] == 0) continue;

		v = ((uint32_t)ch_vol[ch] * chan_decay[ch]) >> 8;

		/* Anything below this is inaudible and would otherwise crawl
		 * toward zero for hundreds of frames, holding a channel that
		 * a later note wants. */
		if (v < 3) v = 0;

		ch_vol[ch] = (uint8_t)v;

		if (v == 0) ch_silence(ch);
		else Z_AUDIO_CH_CTRL(ch) =
			z_audio_ch_ctrl((uint8_t)v, (uint8_t)v, true, false, 0);

	}

	if (!playing) return;

	/* Advance by the frames that ACTUALLY elapsed, not by one. A frame
	 * the game overran then advances the music by the right amount
	 * instead of dragging the tempo down with the frame rate. */
	frame_accum += frames * 256u;

	while (frame_accum >= frames_per_row_x256) {

		const char *row;

		frame_accum -= frames_per_row_x256;

		row = song_main[cur_row];

		for (ch = 0; ch < MUS_CHANNELS; ch++) {

			uint32_t f = parse_cell(row + ch * 4);

			if (f == CELL_HOLD) continue;

			if (f == CELL_OFF) {
				ch_vol[ch] = 0;
				ch_silence(ch);
				continue;
			}

			ch_note(ch, f);

		}

		cur_row++;
		if (cur_row >= MUS_TOTAL_ROWS) cur_row = 0;

	}

}

void sfx_play(int which) {

	int ch;

	if (!audio_ok) return;
	if (which < 0 || which >= SFX_COUNT) return;

	/* One channel per effect, so a jump and a coin overlap rather than
	 * cutting each other off. With four effects and four channels
	 * there is nothing to allocate. */
	ch = SFX_CH_BASE + which;

	Z_AUDIO_CH_BASE(ch)    = phys_of(sfx_buf[which]);
	Z_AUDIO_CH_LEN(ch)     = SFX_LEN;
	Z_AUDIO_CH_LOOPST(ch)  = 0;
	Z_AUDIO_CH_LOOPLEN(ch) = 0;      /* one-shot -- see docs/audio.md */
	Z_AUDIO_CH_STEP(ch)    = z_audio_step(11025, z_audio_rate_hz());

	/* TRIG restarts from the beginning, so retriggering a sound that
	 * is still playing cuts it off and starts again -- which is what a
	 * game wants from a jump sound. */
	{
		uint8_t g = (uint8_t)(((uint32_t)SFX_GAIN * sfx_vol) >> 8);
		Z_AUDIO_CH_CTRL(ch) = z_audio_ch_ctrl(g, g, true, true, 0);
	}

}

void music_shutdown(void) {
	int i;
	if (!audio_ok) return;
	playing = false;
	for (i = 0; i < MUS_CHANNELS + SFX_CHANNELS; i++) ch_silence(i);
	z_audio_mixer_enable(false);
	audio_ok = false;
}
