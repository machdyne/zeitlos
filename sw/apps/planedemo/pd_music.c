/*
 * planedemo -- the sequencer: the song's notes onto the mixer's eight
 * channels, a frame at a time. See pd_music.h.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The mixer plays the samples by itself (docs/audio.md, "The hardware
 * mixer"); all that is left here is, once a frame, each channel's pitch
 * and loudness: a note's envelope, a slide, a little vibrato on the
 * lead. On the build host (PD_HOST) the same register writes go to a
 * model of the mixer instead, which the host test turns into a WAV.
 */

#include <stdlib.h>
#include <string.h>

#include "pd_music.h"

/* -- the mixer, or its model -- */

#ifdef PD_HOST

typedef struct {
	const int16_t *d;
	uint32_t len, loop_st, loop_n;   /* bytes, as the hardware counts */
	uint32_t step;                   /* 18.14, bytes */
	uint64_t pos;
	uint8_t gl, gr;
	bool on;
} host_ch_t;

static host_ch_t hch[PD_CHANNELS];

static bool hw_take(void) { return true; }
static void hw_release(void) { }
static uint32_t hw_out_hz(void) { return 48000; }
static bool hw_speech(void) { return false; }

static void hw_note(int c, const pd_sample_t *s, uint32_t step, uint8_t gl, uint8_t gr) {
	host_ch_t *h = &hch[c];
	h->d = s->d;
	h->len = s->n * 2;
	h->loop_st = s->loop_st * 2;
	h->loop_n = s->loop_n * 2;
	h->step = step;
	h->pos = 0;
	h->gl = gl; h->gr = gr;
	h->on = true;
}

static void hw_set(int c, uint32_t step, uint8_t gl, uint8_t gr) {
	hch[c].step = step; hch[c].gl = gl; hch[c].gr = gr;
}

static void hw_off(int c) { hch[c].on = false; }

/* The mixer's arithmetic, for the host's WAV: nearest sample, no
 * interpolation, the loop wrapped as the hardware wraps it. */
void pdm_host_mix(int16_t *lr, int frames) {
	int i, c;
	for (i = 0; i < frames; i++) {
		int64_t l = 0, r = 0;
		for (c = 0; c < PD_CHANNELS; c++) {
			host_ch_t *h = &hch[c];
			uint32_t at;
			int v;
			if (!h->on) continue;
			at = (uint32_t)(h->pos >> 14);
			if (at >= h->len) {
				if (!h->loop_n) { h->on = false; continue; }
				h->pos -= (uint64_t)h->loop_n << 14;
				at = (uint32_t)(h->pos >> 14);
			}
			v = h->d[at >> 1];
			l += (int64_t)v * h->gl;
			r += (int64_t)v * h->gr;
			h->pos += h->step;
		}
		/* eight channels at full gain could reach 8 x 255: the board's
		 * mixer scales the same way, and the levels below keep it clear */
		l = l * 3 >> 11; r = r * 3 >> 11;
		lr[2 * i]     = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
		lr[2 * i + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
	}
}

#else

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zaudio.h"
#include "../../common/ztts.h"

/* The mixer is a bus master and does not go through the MTU: it needs
 * physical addresses (as zsfx.c). */
static uint32_t phys_of(const void *p) {
	uint32_t v = (uint32_t)(uintptr_t)p, base = reg_mtu_base;
	if (base == 0) return v;
	if ((v & 0xF0000000u) != 0x80000000u) return v;
	return base + (v & 0x0FFFFFFFu);
}

static bool hw_take(void) {
	if (!z_audio_present() || !z_audio_mixer_present() || !z_audio_mixer_fmt16())
		return false;
	if (!(reg_audio_ctrl & Z_AUDIO_CTRL_EN)) z_audio_start(reg_audio_rate & 0xFFu);
	z_audio_mixer_enable(true);
	reg_audio_mixvol = (reg_audio_mixvol & ~0xFFu) | 0xFFu;
	return true;
}

static void hw_release(void) {
	uint32_t pid;
	if (!z_pid_lookup(Z_TTS_SERVICE, &pid)) z_audio_mixer_enable(false);
}

static uint32_t hw_out_hz(void) { return z_audio_rate_hz(); }

static bool hw_speech(void) {
	uint32_t pid;
	return z_pid_lookup(Z_TTS_SERVICE, &pid);
}

static void hw_note(int c, const pd_sample_t *s, uint32_t step, uint8_t gl, uint8_t gr) {
	Z_AUDIO_CH_BASE(c)    = phys_of(s->d);
	Z_AUDIO_CH_LEN(c)     = s->n * 2;
	Z_AUDIO_CH_LOOPST(c)  = s->loop_st * 2;
	Z_AUDIO_CH_LOOPLEN(c) = s->loop_n * 2;
	Z_AUDIO_CH_STEP(c)    = step;
	Z_AUDIO_CH_CTRL(c)    = z_audio_ch_ctrl(gl, gr, true, true, 0) | Z_AUDIO_CH_FMT16;
}

/* EN without TRIG: the sample keeps its place */
static void hw_set(int c, uint32_t step, uint8_t gl, uint8_t gr) {
	Z_AUDIO_CH_STEP(c) = step;
	Z_AUDIO_CH_CTRL(c) = z_audio_ch_ctrl(gl, gr, true, false, 0) | Z_AUDIO_CH_FMT16;
}

static void hw_off(int c) {
	Z_AUDIO_CH_CTRL(c) = z_audio_ch_ctrl(0, 0, false, false, 0);
}

#endif

/* -- the instruments' envelopes and levels -- */

typedef struct {
	uint8_t attack, decay, sustain, release;   /* frames; sustain 0..255 */
	uint8_t gain;                               /* 0..255 */
	int8_t pan;                                 /* -64 left .. 64 right */
	uint8_t vib;                                /* vibrato depth, 1/16 semitone */
} inst_t;

static const inst_t inst[PD_I_COUNT] = {
	/* KICK    */ { 0,  0, 255, 6,  255,   0, 0 },
	/* SNARE   */ { 0,  0, 255, 6,  170,   4, 0 },
	/* CLAP    */ { 0,  0, 255, 6,  150,  -8, 0 },
	/* HATC    */ { 0,  0, 255, 3,   90,  20, 0 },
	/* HATO    */ { 0,  0, 255, 8,   80,  24, 0 },
	/* BASS    */ { 0, 18, 150, 5,  200,   0, 0 },
	/* PLUCK   */ { 0,  9,  60, 8,  120, -28, 0 },
	/* LEAD    */ { 2, 20, 190, 14, 140,  10, 3 },
	/* PAD     */ { 45, 30, 200, 50, 110, -14, 0 },
	/* THUNDER */ { 0,  0, 255, 30, 255,   0, 0 },
	/* TICK    */ { 0,  0, 255, 2,  130,  30, 0 },
	/* CLATTER */ { 0,  0, 255, 20, 220,  16, 0 },
	/* NOISE   */ { 30, 0, 255, 20,  90,   0, 0 },
	/* BELL    */ { 0,  0, 255, 20, 140, -20, 0 },
};

/* 2^(i/192), Q16, i = 0..191: a sixteenth of a semitone a step */
static uint32_t pow2_t[192];

static void pow2_init(void) {
	/* by repeated multiplication with 2^(1/192) in Q30, renormalized
	 * each octave: no libm */
	const uint64_t k = 1077625190ULL;     /* 2^(1/192) * 2^30 */
	uint64_t v = 1ULL << 30;
	int i;
	for (i = 0; i < 192; i++) {
		pow2_t[i] = (uint32_t)(v >> 14);
		v = (v * k) >> 30;
	}
}

typedef struct {
	bool on, released;
	uint8_t inst;
	int pitch;              /* 1/16 semitone, MIDI * 16 */
	int slide_to, slide_frames;
	uint8_t vel;
	uint32_t start, off;    /* film frames: started, released (0: not yet) */
	int env;                /* 0..255, this frame */
	uint32_t base_step;
} chan_t;

static chan_t ch[PD_CHANNELS];
static pd_note_t song[2048];
static int nsong, next_note;
static int last_row = -1;
static bool have_hw, speech;
static uint32_t base_step_unit;   /* the step for a sample at PD_RATE, unshifted */

static uint32_t step_of(int c) {
	const pd_sample_t *s = &pd_samples[ch[c].inst];
	int off = ch[c].pitch - s->base_note * 16, oct = 0;
	uint64_t st;
	while (off < 0) { off += 192; oct--; }
	while (off >= 192) { off -= 192; oct++; }
	st = (uint64_t)base_step_unit * pow2_t[off] >> 16;
	if (oct > 0) st <<= oct;
	else st >>= -oct;
	return (uint32_t)st;
}

static void gains(int c, int level, uint8_t *gl, uint8_t *gr) {
	int pan = inst[ch[c].inst].pan;
	int l = level * (64 - pan) / 64, r = level * (64 + pan) / 64;
	if (l > 255) l = 255;
	if (r > 255) r = 255;
	*gl = (uint8_t)l;
	*gr = (uint8_t)r;
}

static int level_of(int c) {
	return ch[c].env * ch[c].vel / 127 * inst[ch[c].inst].gain / 255;
}

static void start_note(const pd_note_t *n, uint32_t frame) {
	chan_t *h = &ch[n->ch];
	uint8_t gl, gr;
	if (n->ch >= PD_CHANNELS) return;
	if (speech && n->ch == 7) return;          /* speech's channel stays speech's */
	h->on = true;
	h->released = false;
	h->inst = n->inst;
	h->pitch = n->note * 16;
	h->vel = n->vel;
	h->start = frame;
	h->off = n->len ? frame + (uint32_t)(n->len * 225 + 31) / 32 : 0;
	h->slide_to = n->slide ? h->pitch + n->slide * 16 : h->pitch;
	h->slide_frames = n->len ? (int)((n->len * 225 + 31) / 32) : 60;
	h->env = inst[n->inst].attack ? 0 : 255;
	if (!have_hw) return;
	gains(n->ch, level_of(n->ch), &gl, &gr);
	hw_note(n->ch, &pd_samples[n->inst], step_of(n->ch), gl, gr);
}

/* One frame of a channel: the envelope, the slide, the vibrato. */
static void step_chan(int c, uint32_t frame) {
	chan_t *h = &ch[c];
	const inst_t *in = &inst[h->inst];
	uint32_t t = frame - h->start;
	int pitch = h->pitch;
	uint8_t gl, gr;

	if (!h->on) return;
	if (h->off && frame >= h->off) h->released = true;
	if (!h->released) {
		if (t < in->attack) h->env = (int)(t * 255 / in->attack);
		else if (t < (uint32_t)in->attack + in->decay)
			h->env = 255 - (int)((t - in->attack) * (255 - in->sustain) / in->decay);
		else h->env = in->decay ? in->sustain : 255;
	} else {
		int dec = in->release ? 255 / in->release + 1 : 255;
		h->env -= dec;
		if (h->env <= 0) {
			h->env = 0;
			h->on = false;
			if (have_hw) hw_off(c);
			return;
		}
	}
	/* a one-shot sample ends by itself: forget it then */
	if (in->sustain == 255 && in->decay == 0 && !pd_samples[h->inst].loop_n) {
		uint32_t frames = pd_samples[h->inst].n * PD_FPS / PD_RATE;
		if (t > frames) h->on = false;
	}
	if (h->slide_to != h->pitch && h->slide_frames > 0) {
		int k = (int)(t < (uint32_t)h->slide_frames ? t : (uint32_t)h->slide_frames);
		pitch = h->pitch + (h->slide_to - h->pitch) * k / h->slide_frames;
	}
	if (in->vib && t > 12) {
		/* a gentle triangle, 5 Hz or so, after the note has spoken */
		int ph = (int)((t * 5 * 4 / PD_FPS) % 4), v;
		int fr = (int)((t * 5 * 4) % PD_FPS);
		v = (ph == 0) ? fr : (ph == 1) ? PD_FPS - fr : (ph == 2) ? -fr : fr - PD_FPS;
		pitch += v * in->vib / PD_FPS;
	}
	if (!have_hw || !h->on) return;
	{
		int save = h->pitch;
		h->pitch = pitch;
		gains(c, level_of(c), &gl, &gr);
		hw_set(c, step_of(c), gl, gr);
		h->pitch = save;
	}
}

bool pdm_init(void) {
	uint32_t out;
	pow2_init();
	/* the instruments: from the film's data, else made here */
	if (!pd_samples[0].d && !pd_synth_all()) return false;
	nsong = pd_song_build(song, (int)(sizeof(song) / sizeof(song[0])));
	next_note = 0;
	last_row = -1;
	memset(ch, 0, sizeof(ch));
	have_hw = hw_take();
	speech = have_hw && hw_speech();
	out = have_hw ? hw_out_hz() : 48000;
	if (!out) out = 48000;
	/* bytes a frame for a 16-bit sample at PD_RATE, in 18.14 */
	base_step_unit = (uint32_t)(((uint64_t)PD_RATE * 2u << 14) / out);
	return have_hw;
}

void pdm_shutdown(void) {
	int c;
	if (!have_hw) return;
	for (c = 0; c < PD_CHANNELS; c++) if (!(speech && c == 7)) hw_off(c);
	hw_release();
	have_hw = false;
}

void pdm_frame(uint32_t frame) {
	int row = pd_row_of(frame), c;
	if (row >= PD_SONG_ROWS) row = PD_SONG_ROWS - 1;
	while (last_row < row) {
		last_row++;
		while (next_note < nsong && song[next_note].row == last_row) {
			/* a note starts on the frame its row begins */
			start_note(&song[next_note], pd_frame_of_row(last_row) > frame ? frame : pd_frame_of_row(last_row));
			next_note++;
		}
	}
	for (c = 0; c < PD_CHANNELS; c++) step_chan(c, frame);
}

int pdm_level(int c) {
	if (c < 0 || c >= PD_CHANNELS || !ch[c].on) return 0;
	return level_of(c);
}

uint32_t pdm_last_hit(int c) {
	return (c >= 0 && c < PD_CHANNELS) ? ch[c].start : 0;
}
