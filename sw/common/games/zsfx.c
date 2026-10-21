/*
 * Zeitlos -- sound effects and music for games. See zsfx.h and
 * docs/sfx.md.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- one engine, eight voices --
 *
 * Each mixer channel has a VOICE here, and a voice is doing one of two
 * things: running an effect program (segments, stepped once a frame),
 * or holding a music note (an envelope that decays once a frame). Drum
 * hits in a song are effect programs too, run on the music channel the
 * drum cell is in -- so a kick is a triangle sweeping down from 150Hz,
 * not a sample, and costs nothing to store.
 *
 * Everything the hardware is told goes through four small functions
 * (hw_*), so the host tests (ZSFX_HOSTED) can watch the register
 * traffic instead of a DAC.
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "zsfx.h"

#define NCH       8
#define MUS_CH    4
#define SFX_FIRST 4
#define WAVE_LEN  64
#define NOISE_LEN 4096

/* ===================================================================
 * the hardware
 * =================================================================== */

#ifdef ZSFX_HOSTED

/* tests/: a record of what would have been written */
bool zsfxh_present(void);
bool zsfxh_speech(void);
uint32_t zsfxh_out_hz(void);
void zsfxh_note(int ch, const int8_t *wave, uint32_t len, uint32_t step,
	uint8_t gl, uint8_t gr);
void zsfxh_gain(int ch, uint32_t step, uint8_t gl, uint8_t gr);
void zsfxh_off(int ch);
void zsfxh_release(void);

#define hw_present  zsfxh_present
#define hw_speech   zsfxh_speech
#define hw_out_hz   zsfxh_out_hz
#define hw_note     zsfxh_note
#define hw_gain     zsfxh_gain
#define hw_off      zsfxh_off
#define hw_release  zsfxh_release

static void hw_take(void) { }

#else

#include "../zeitlos.h"
#include "../zsoc.h"
#include "../zaudio.h"
#include "../ztts.h"

/* App virtual address -> physical. THE MIXER IS A BUS MASTER AND DOES
 * NOT GO THROUGH THE MTU: an app pointer handed to it is an address
 * nothing decodes, which never acks, and the whole machine stops
 * (sw/apps/track; mausrun learnt it first, in the music.c it had). */
static uint32_t phys_of(const void *p) {
	uint32_t v = (uint32_t)(uintptr_t)p;
	uint32_t base = reg_mtu_base;
	if (base == 0) return v;
	if ((v & 0xF0000000u) != 0x80000000u) return v;
	return base + (v & 0x0FFFFFFFu);
}

static bool hw_present(void) {
	return z_audio_present() && z_audio_mixer_present();
}

static bool hw_speech(void) {
	uint32_t pid;
	return z_pid_lookup(Z_TTS_SERVICE, &pid);
}

static uint32_t hw_out_hz(void) {
	return z_audio_rate_hz();
}

static void hw_take(void) {
	/* Enable the output if nothing has: without EN the DAC is muted
	 * however busy the mixer is. The rate is left exactly as the board
	 * set it -- an S/PDIF board needs its own (mausrun's old music.c). If
	 * speech is already playing, EN is on and is not touched. */
	if (!(reg_audio_ctrl & Z_AUDIO_CTRL_EN))
		z_audio_start(reg_audio_rate & 0xFFu);
	z_audio_mixer_enable(true);
	/* The mixer's output scale at its top: one 8-bit channel cannot
	 * get loud on this mixer, so levels are set by keeping the music
	 * gains low (as mausrun's old music.h did). Read-modify-write: the high
	 * bits hold the S/PDIF rate code. The system volume (wm's) is
	 * separate and untouched. */
	reg_audio_mixvol = (reg_audio_mixvol & ~0xFFu) | 0xFFu;
}

static void hw_note(int ch, const int8_t *wave, uint32_t len, uint32_t step,
	uint8_t gl, uint8_t gr) {
	Z_AUDIO_CH_BASE(ch)    = phys_of(wave);
	Z_AUDIO_CH_LEN(ch)     = len;
	Z_AUDIO_CH_LOOPST(ch)  = 0;
	Z_AUDIO_CH_LOOPLEN(ch) = len;
	Z_AUDIO_CH_STEP(ch)    = step;
	Z_AUDIO_CH_CTRL(ch)    = z_audio_ch_ctrl(gl, gr, true, true, 0);
}

/* EN without TRIG: new gain and pitch, the waveform keeps its phase --
 * which is what makes a sweep a sweep and not a string of clicks. */
static void hw_gain(int ch, uint32_t step, uint8_t gl, uint8_t gr) {
	Z_AUDIO_CH_STEP(ch) = step;
	Z_AUDIO_CH_CTRL(ch) = z_audio_ch_ctrl(gl, gr, true, false, 0);
}

static void hw_off(int ch) {
	Z_AUDIO_CH_CTRL(ch) = z_audio_ch_ctrl(0, 0, false, false, 0);
}

static void hw_release(void) {
	uint32_t pid;
	/* With speech running the mixer stays on: it is speaking through
	 * it. Otherwise give the DAC back to the FIFO, as found. */
	if (!z_pid_lookup(Z_TTS_SERVICE, &pid)) z_audio_mixer_enable(false);
}

#endif

/* ===================================================================
 * waveforms
 * =================================================================== */

static int8_t waves[ZSFX_NOISE][WAVE_LEN];
static int8_t noise[NOISE_LEN];

/* A quarter sine, 17 points, 0..100: the rest by symmetry. */
static const uint8_t qsin[17] = {
	0, 10, 20, 29, 38, 47, 56, 63, 71, 77, 83, 88, 92, 96, 98, 100, 100
};

static void build_waves(void) {
	int i;
	uint16_t lfsr = 0xACE1u;
	for (i = 0; i < WAVE_LEN; i++) {
		int q = i & 15, s;
		/* Peaks of 100, not 127, so several channels together do not
		 * clip the mixer's sum (mausrun's old music.c). */
		waves[ZSFX_SQUARE][i]  = (int8_t)(i < 32 ? 100 : -100);
		waves[ZSFX_PULSE25][i] = (int8_t)(i < 16 ? 100 : -100);
		waves[ZSFX_PULSE12][i] = (int8_t)(i < 8 ? 100 : -100);
		waves[ZSFX_TRI][i] = (int8_t)(i < 32 ? -100 + (200 * i) / 32
			: 100 - (200 * (i - 32)) / 32);
		waves[ZSFX_SAW][i] = (int8_t)(-100 + (200 * i) / 63);
		switch (i >> 4) {
		case 0: s = qsin[q]; break;
		case 1: s = qsin[16 - q]; break;
		case 2: s = -qsin[q]; break;
		default: s = -qsin[16 - q]; break;
		}
		waves[ZSFX_SINE][i] = (int8_t)s;
	}
	/* 16-bit maximal LFSR, as mausrun's: period 65535, and a 4KB
	 * window of it has no audible repeat. */
	for (i = 0; i < NOISE_LEN; i++) {
		uint16_t bit = ((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^
			(lfsr >> 5)) & 1u;
		lfsr = (uint16_t)((lfsr >> 1) | (bit << 15));
		noise[i] = (int8_t)(((int)(lfsr & 0xFF) - 128) * 100 / 128);
	}
}

static const int8_t *wave_data(int w, uint32_t *len) {
	if (w == ZSFX_NOISE) { *len = NOISE_LEN; return noise; }
	if (w < 0 || w >= ZSFX_NOISE) w = ZSFX_SQUARE;
	*len = WAVE_LEN;
	return waves[w];
}

/* ===================================================================
 * state
 * =================================================================== */

enum { V_IDLE = 0, V_PROG, V_NOTE };

typedef struct {
	uint8_t kind;
	uint8_t prio;
	const zsfx_seg_t *seg;
	uint8_t nseg, si, sf;
	int8_t cur_wave;
	int16_t pan;
	uint32_t age;
	/* V_NOTE */
	uint32_t chz;
	uint16_t vol;      /* 8.8 */
	uint8_t decay, vib, vphase;
} voice_t;

static voice_t v[NCH];
static bool ok, suspended, speech;
static uint32_t out_hz;
static uint32_t clock_age;

static const zsfx_song_t *song;
static bool music_on = true, music_running;
static int order_pos, row_in_pat;
static uint32_t row_accum;         /* frames x 256 since the last row */
static uint32_t frames_per_row_x256;
static int tempo_pct = 100;
static uint8_t music_vol = 255;
/* zsfx_music_layers(): the channels heard, and the drums heard on a
 * drum channel (bit d = drum d: kick, snare, hat, open hat). */
static uint8_t layer_ch = 0x0f, layer_drums = 0x0f;
/* zsfx_master_volume(): everything, music and effects together */
static uint8_t master = 255;
#define MV(g) ((uint8_t)(((uint32_t)(g) * (master + 1u)) >> 8))
static int32_t rows_played = -1;

static uint8_t sfx_last;           /* the highest effect channel */

/* ===================================================================
 * pitch and gain
 * =================================================================== */

/* Phase step for a waveform of `len` samples at `chz` centi-hertz. The
 * mixer wants (sample rate << 14) / out_hz, which must stay under 2^32:
 * sample rates are capped at 250kHz, far above anything audible here
 * (4kHz at 64 samples a cycle). */
static uint32_t step_for(uint32_t chz, uint32_t len) {
	uint32_t rate;
	if (!out_hz) return 0;
	if (len == NOISE_LEN) len = WAVE_LEN;   /* noise: "pitch" is rate/64 */
	rate = (chz / 100u) * len + ((chz % 100u) * len) / 100u;
	if (rate > 250000u) rate = 250000u;
	return (rate << 14) / out_hz;
}

static void pan_gains(int g, int pan, uint8_t *gl, uint8_t *gr) {
	int l = g, r = g;
	if (g < 0) g = 0;
	if (g > 255) g = 255;
	if (pan > 0) l = g * (256 - 2 * pan) / 256;
	else if (pan < 0) r = g * (256 + 2 * pan) / 256;
	if (pan == 0) { l = g; r = g; }
	if (l < 0) l = 0;
	if (r < 0) r = 0;
	*gl = (uint8_t)(l > 255 ? 255 : l);
	*gr = (uint8_t)(r > 255 ? 255 : r);
}

/* ===================================================================
 * effect programs
 * =================================================================== */

static void prog_frame(int ch, voice_t *vo, bool first) {

	const zsfx_seg_t *s;
	uint32_t f;
	int g;
	uint8_t gl, gr;
	uint32_t len;
	const int8_t *wd;

	/* Skip empty segments; finish when past the last. */
	while (vo->si < vo->nseg && vo->seg[vo->si].frames == 0) vo->si++;
	if (vo->si >= vo->nseg) {
		vo->kind = V_IDLE;
		hw_off(ch);
		return;
	}

	s = &vo->seg[vo->si];
	f = (uint32_t)s->f0 + (uint32_t)(((int32_t)s->f1 - (int32_t)s->f0) *
		(int32_t)vo->sf / (int32_t)s->frames);
	g = s->v0 + ((s->v1 - s->v0) * vo->sf) / s->frames;
	if (f > 4000) f = 4000;

	pan_gains(g, vo->pan, &gl, &gr);
	wd = wave_data(s->wave, &len);

	if (first || vo->cur_wave != (int8_t)s->wave) {
		vo->cur_wave = (int8_t)s->wave;
		hw_note(ch, wd, len, step_for(f * 100u, len), MV(gl), MV(gr));
	} else {
		hw_gain(ch, step_for(f * 100u, len), MV(gl), MV(gr));
	}

	if (++vo->sf >= s->frames) {
		vo->sf = 0;
		vo->si++;
	}
}

static void prog_start(int ch, const zsfx_seg_t *seg, int n, int prio,
	int pan) {
	voice_t *vo = &v[ch];
	vo->kind = V_PROG;
	vo->prio = (uint8_t)prio;
	vo->seg = seg;
	vo->nseg = (uint8_t)(n > 8 ? 8 : n);
	vo->si = 0;
	vo->sf = 0;
	vo->cur_wave = -1;
	vo->pan = (int16_t)pan;
	vo->age = ++clock_age;
	prog_frame(ch, vo, true);
}

void zsfx_play(const zsfx_fx_t *fx, int pan) {

	int ch, best = -1;

	if (!ok || suspended || !fx || fx->n == 0) return;

	/* A free effect channel; else the oldest effect of no higher
	 * priority. An effect never takes a music channel. */
	for (ch = SFX_FIRST; ch <= sfx_last; ch++)
		if (v[ch].kind == V_IDLE) { best = ch; break; }

	if (best < 0) {
		uint32_t oldest = 0xffffffffu;
		for (ch = SFX_FIRST; ch <= sfx_last; ch++)
			if (v[ch].prio <= fx->prio && v[ch].age < oldest) {
				oldest = v[ch].age;
				best = ch;
			}
	}

	if (best < 0) return;

	if (pan < -128) pan = -128;
	if (pan > 127) pan = 127;
	prog_start(best, fx->seg, fx->n, fx->prio, pan);
}

void zsfx_stop_all(void) {
	int ch;
	if (!ok) return;
	for (ch = SFX_FIRST; ch <= sfx_last; ch++) {
		v[ch].kind = V_IDLE;
		hw_off(ch);
	}
}

int zsfx_pan_x(int x) {
	int p = (x - 160) * 3 / 4;
	if (p < -100) p = -100;
	if (p > 100) p = 100;
	return p;
}

/* ===================================================================
 * drums
 * =================================================================== */

static const zsfx_seg_t drum_kick[] = {
	{ ZSFX_TRI,   2, 160, 110, 255, 230 },
	{ ZSFX_TRI,   6, 110,  45, 230,   0 },
};
static const zsfx_seg_t drum_snare[] = {
	{ ZSFX_NOISE, 2, 2400, 2000, 200, 150 },
	{ ZSFX_NOISE, 6, 2000, 1600, 150,   0 },
};
static const zsfx_seg_t drum_hat[] = {
	{ ZSFX_NOISE, 3, 4000, 4000, 110,   0 },
};
static const zsfx_seg_t drum_ohat[] = {
	{ ZSFX_NOISE, 10, 3800, 3800, 100,  0 },
};

/* ===================================================================
 * music
 * =================================================================== */

static const uint32_t note_chz[12] = {
	26163, 27718, 29366, 31113, 32963, 34923,
	36999, 39200, 41530, 44000, 46616, 49388
};

uint32_t zsfx_parse_cell(const char *c) {
	int semi, oct;
	uint32_t f;
	switch (c[0]) {
	case '-': case '.': case ' ': case 0: return ZSFX_CELL_HOLD;
	case '=': return ZSFX_CELL_OFF;
	case 'K': return ZSFX_CELL_DRUM + 0;
	case 'S': return ZSFX_CELL_DRUM + 1;
	case 'H': return ZSFX_CELL_DRUM + 2;
	case 'O': return ZSFX_CELL_DRUM + 3;
	case 'C': semi = 0; break;
	case 'D': semi = 2; break;
	case 'E': semi = 4; break;
	case 'F': semi = 5; break;
	case 'G': semi = 7; break;
	case 'A': semi = 9; break;
	case 'B': semi = 11; break;
	default: return ZSFX_CELL_HOLD;
	}
	if (c[1] == '#') semi++;
	oct = c[2] - '0';
	if (oct < 0 || oct > 8) return ZSFX_CELL_HOLD;
	if (semi > 11) { semi -= 12; oct++; }
	f = note_chz[semi];
	if (oct > 4) f <<= (oct - 4);
	else if (oct < 4) f >>= (4 - oct);
	return f;
}

static int order_val(char ch) {
	if (ch >= '0' && ch <= '9') return ch - '0';
	if (ch >= 'a' && ch <= 'z') return ch - 'a' + 10;
	return -1;
}

int zsfx_order_len(const char *order) {
	int n = 0;
	for (; order && *order; order++) if (order_val(*order) >= 0) n++;
	return n;
}

int zsfx_order_at(const char *order, int i) {
	for (; order && *order; order++) {
		int p = order_val(*order);
		if (p < 0) continue;
		if (i-- == 0) return p;
	}
	return -1;
}

static void update_tempo(void) {
	uint32_t rpm;
	if (!song) return;
	rpm = (uint32_t)song->rows_per_min * (uint32_t)tempo_pct / 100u;
	if (rpm < 30) rpm = 30;
	/* frames per row = 60 * frame_hz / rpm; x256 for the fraction.
	 * The frame rate is 60, or 50 on a PAL composite board -- the
	 * tempo must not change with it (mausrun's old music.c). */
	{
#ifdef ZSFX_HOSTED
		uint32_t hz = 60;
#else
		uint32_t hz = z_video_frame_hz();
		if (!hz) hz = 60;
#endif
		frames_per_row_x256 = (60u * hz * 256u) / rpm;
	}
}

static void music_silence(void) {
	int ch;
	for (ch = 0; ch < MUS_CH; ch++) {
		v[ch].kind = V_IDLE;
		if (ok) hw_off(ch);
	}
}

static void note_on(int ch, const zsfx_ins_t *ins, uint32_t chz) {
	voice_t *vo = &v[ch];
	uint32_t len;
	const int8_t *wd = wave_data(ins->wave, &len);
	uint32_t g = ((uint32_t)ins->gain * music_vol) >> 8;
	vo->kind = V_NOTE;
	vo->chz = chz;
	vo->vol = (uint16_t)(g << 8);
	vo->decay = ins->decay;
	vo->vib = ins->vib;
	vo->vphase = 0;
	vo->cur_wave = (int8_t)ins->wave;
	vo->pan = 0;
	hw_note(ch, wd, len, step_for(chz, len), MV(g), MV(g));
}

static void note_frame(int ch, voice_t *vo) {
	uint32_t len, chz = vo->chz;
	uint32_t g;
	wave_data(vo->cur_wave, &len);
	vo->vol = (uint16_t)(((uint32_t)vo->vol * vo->decay) >> 8);
	g = vo->vol >> 8;
	if (g < 2) {
		vo->kind = V_IDLE;
		hw_off(ch);
		return;
	}
	if (vo->vib) {
		/* A triangle LFO of about 6Hz, depth vib/512 of the pitch. */
		int ph = vo->vphase++ & 15;
		int t = ph < 8 ? ph - 4 : 12 - ph;     /* -4..4 */
		chz = chz + (uint32_t)((int32_t)(chz / 512u) * vo->vib * t / 4);
	}
	hw_gain(ch, step_for(chz, len), MV(g), MV(g));
}

static void music_row(void) {

	const char *row;
	int ch, pat = zsfx_order_at(song->order, order_pos);

	if (pat < 0 || pat >= song->npat) pat = 0;
	row = song->rows[pat * 16 + row_in_pat];

	for (ch = 0; ch < song->nch && ch < MUS_CH; ch++) {

		const zsfx_ins_t *ins = &song->ins[ch];
		uint32_t f;
		int i;

		if (!(layer_ch & (1u << ch))) continue;

		/* Cells at 0, 4, 8, 12; a short row string is "hold". */
		for (i = 0; i < ch * 4 && row[i]; i++) ;
		if (!row[i]) continue;
		f = zsfx_parse_cell(row + i);

		if (f == ZSFX_CELL_HOLD) continue;
		if (f == ZSFX_CELL_OFF) { v[ch].kind = V_IDLE; hw_off(ch); continue; }

		if (f >= ZSFX_CELL_DRUM) {
			static const zsfx_seg_t *const kit[4] = {
				drum_kick, drum_snare, drum_hat, drum_ohat
			};
			static const uint8_t kitn[4] = { 2, 2, 1, 1 };
			int d = (int)(f - ZSFX_CELL_DRUM) & 3;
			if (!(layer_drums & (1u << d))) continue;
			prog_start(ch, kit[d], kitn[d], 0, 0);
			/* Drums follow the music level too: scaled at start by
			 * running the program at the channel's gain. Programs
			 * have their own levels, so a song's drum channel gain
			 * of 255 means "as the kit is". */
			continue;
		}

		if (ins->wave & ZSFX_DRUMS) continue;
		note_on(ch, ins, f);
	}

	if (++row_in_pat >= 16) {
		row_in_pat = 0;
		if (++order_pos >= zsfx_order_len(song->order)) order_pos = 0;
	}
}

void zsfx_music_play(const zsfx_song_t *s) {
	song = s;
	order_pos = 0;
	row_in_pat = 0;
	row_accum = 0;
	update_tempo();
	music_running = (s != NULL);
	rows_played = -1;
	/* The first row lands on the next tick, at once. */
	row_accum = frames_per_row_x256;
	if (ok) music_silence();
}

int32_t zsfx_music_rows(void) {
	return music_running ? rows_played : -1;
}

void zsfx_music_stop(void) {
	music_running = false;
	song = NULL;
	music_silence();
}

void zsfx_music_enable(bool on) {
	music_on = on;
	if (!on) music_silence();
}

bool zsfx_music_enabled(void) { return music_on; }

void zsfx_music_tempo(int percent) {
	if (percent < 25) percent = 25;
	if (percent > 400) percent = 400;
	tempo_pct = percent;
	update_tempo();
}

void zsfx_music_volume(uint8_t vol) { music_vol = vol; }

void zsfx_master_volume(uint8_t vol) { master = vol; }
uint8_t zsfx_master_get(void) { return master; }

void zsfx_music_layers(uint8_t channels, uint8_t drums) {
	int ch;
	/* A channel just left out stops now, rather than ringing on to
	 * the end of its note. */
	for (ch = 0; ch < MUS_CH; ch++)
		if ((layer_ch & ~channels) & (1u << ch)) {
			v[ch].kind = V_IDLE;
			if (ok) hw_off(ch);
		}
	layer_ch = channels & 0x0f;
	layer_drums = drums & 0x0f;
}

/* The arrangement grows with the game: lead and bass alone at first,
 * then the harmony, then the hats, the kick and last the snare; and
 * the tempo from 70% to 110% of the song as written. */
void zsfx_music_intensity(int level) {
	static const uint8_t drums[ZSFX_INTENSITY_FULL + 1] = {
		0x0, 0x0, 0x4, 0xc, 0xd, 0xd, 0xf, 0xf, 0xf, 0xf, 0xf
	};
	if (level < 0) level = 0;
	if (level > ZSFX_INTENSITY_FULL) level = ZSFX_INTENSITY_FULL;
	zsfx_music_layers(level == 0 ? 0x5 : 0xf, drums[level]);
	zsfx_music_tempo(70 + 4 * level);
}

/* ===================================================================
 * the frame
 * =================================================================== */

void zsfx_tick(uint32_t frames) {

	int ch;

	if (!ok || suspended) return;
	if (frames == 0) return;
	if (frames > 8) frames = 8;

	/* Envelopes and programs step once per elapsed frame, so a frame
	 * the game overran costs no tempo. */
	{
		uint32_t f;
		for (f = 0; f < frames; f++)
			for (ch = 0; ch <= sfx_last; ch++) {
				voice_t *vo = &v[ch];
				if (vo->kind == V_PROG) prog_frame(ch, vo, false);
				else if (vo->kind == V_NOTE) note_frame(ch, vo);
			}
	}

	if (!music_running || !song) return;

	row_accum += frames * 256u;
	while (row_accum >= frames_per_row_x256) {
		row_accum -= frames_per_row_x256;
		rows_played++;
		if (music_on) music_row();
		else if (++row_in_pat >= 16) {
			/* muted: keep the place, so turning it back on resumes
			 * in time rather than from the top */
			row_in_pat = 0;
			if (++order_pos >= zsfx_order_len(song->order)) order_pos = 0;
		}
	}
}

void zsfx_suspend(bool on) {
	int ch;
	if (!ok || on == suspended) return;
	suspended = on;
	if (on) {
		for (ch = 0; ch <= sfx_last; ch++) {
			v[ch].kind = V_IDLE;
			hw_off(ch);
		}
	}
}

/* ===================================================================
 * setup
 * =================================================================== */

bool zsfx_init(void) {
	int ch;
	ok = false;
	memset(v, 0, sizeof(v));
	if (!hw_present()) return false;
	build_waves();
	speech = hw_speech();
	sfx_last = speech ? NCH - 2 : NCH - 1;
	hw_take();
	out_hz = hw_out_hz();
	if (!out_hz) return false;
	for (ch = 0; ch <= sfx_last; ch++) hw_off(ch);
	ok = true;
	suspended = false;
	update_tempo();
	return true;
}

bool zsfx_ok(void) { return ok; }

void zsfx_shutdown(void) {
	int ch;
	if (!ok) return;
	for (ch = 0; ch <= sfx_last; ch++) hw_off(ch);
	hw_release();
	ok = false;
}
