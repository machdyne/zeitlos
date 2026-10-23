/*
 * planedemo -- the instruments, synthesized into 16-bit samples at
 * start-up. Integer arithmetic only (the board has no FPU), the same
 * on the host, so the WAV the host writes is what the board plays.
 *
 * Every sample is at PD_RATE (35200 Hz), where A4 is exactly 80
 * samples and every whole number of Hz completes a whole number of
 * cycles in one second: so a one-second loop of several detuned saws
 * at whole-Hz pitches joins without a click. That is how the lead and
 * the pad are fat and still loop.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdlib.h>
#include <string.h>

#include "pd_music.h"

pd_sample_t pd_samples[PD_I_COUNT];

/* -- tools -- */

#define SIN_N 4096
static int16_t sin_t[SIN_N];          /* Q15 */

/* One cycle of sine by rotation, in 64-bit fixed point: no libm. */
static void sin_init(void) {
	/* cos and sin of 2*pi/4096 in Q30 */
	const int64_t c = 1073740561LL, s = 1647099LL;
	int64_t x = 1LL << 30, y = 0;
	int i;
	for (i = 0; i < SIN_N; i++) {
		int64_t nx, ny;
		sin_t[i] = (int16_t)((y * 32767) >> 30);
		nx = (x * c - y * s) >> 30;
		ny = (x * s + y * c) >> 30;
		x = nx; y = ny;
	}
}

/* sine of a phase in 1/65536 cycle */
static int sinp(uint32_t ph) { return sin_t[(ph >> 4) & (SIN_N - 1)]; }

static uint32_t lfsr = 0xACE1u;
static int noise(void) {
	lfsr ^= lfsr << 13; lfsr ^= lfsr >> 17; lfsr ^= lfsr << 5;
	return (int)(int16_t)(lfsr >> 8);
}

/* Q16 multiplier for a decay to 1/e over t samples */
static int32_t decay_k(int t) {
	if (t < 1) t = 1;
	return 65536 - 65536 / t;
}

static int16_t clip16(int v) {
	return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

static int16_t *alloc_frames(uint32_t n) {
	return (int16_t *)calloc(n + 2, sizeof(int16_t));
}

static bool make(int i, uint32_t n, int base) {
	pd_sample_t *s = &pd_samples[i];
	s->d = alloc_frames(n);
	if (!s->d) return false;
	s->n = n;
	s->loop_st = s->loop_n = 0;
	s->base_note = base;
	return true;
}

/* Scale a sample so its peak is `peak`. */
static void normalize(pd_sample_t *s, int peak) {
	uint32_t i;
	int m = 1;
	for (i = 0; i < s->n; i++) {
		int v = s->d[i] < 0 ? -s->d[i] : s->d[i];
		if (v > m) m = v;
	}
	/* 32-bit: |d| * peak < 2^31 (no 64-bit division: a library call,
	 * hundreds of cycles, on the board) */
	for (i = 0; i < s->n; i++) s->d[i] = clip16(s->d[i] * peak / m);
}

/* -- drums -- */

static bool kick(void) {
	pd_sample_t *s;
	uint32_t i, n = PD_RATE * 45 / 100, ph = 0;
	int32_t f = 160 << 16, amp = 32767 << 15, kf = decay_k(PD_RATE / 22), ka = decay_k(PD_RATE / 5);
	if (!make(PD_I_KICK, n, 60)) return false;
	s = &pd_samples[PD_I_KICK];
	for (i = 0; i < n; i++) {
		int hz = 44 + ((f >> 16) - 44);
		int v = (int)(((int64_t)sinp(ph) * (amp >> 15)) >> 15);
		if (i < 60) v += noise() >> 3;                  /* the beater's click */
		s->d[i] = clip16(v);
		ph += (uint32_t)(hz * 65536 / PD_RATE);
		f = (int32_t)(((int64_t)(f - (44 << 16)) * kf >> 16) + (44 << 16));
		amp = (int32_t)((int64_t)amp * ka >> 16);
	}
	normalize(s, 30000);
	return true;
}

static bool snare(void) {
	pd_sample_t *s;
	uint32_t i, n = PD_RATE * 28 / 100, ph = 0;
	int32_t at = 32767, an = 32767, kt = decay_k(PD_RATE / 30), kn = decay_k(PD_RATE / 14);
	int lp = 0, prev = 0;
	if (!make(PD_I_SNARE, n, 60)) return false;
	s = &pd_samples[PD_I_SNARE];
	for (i = 0; i < n; i++) {
		int x = noise(), hp = x - prev;            /* bright noise */
		int v;
		prev = x;
		lp += (hp - lp) >> 1;
		v = (sinp(ph) * at >> 16) + (lp * an >> 15);
		s->d[i] = clip16(v);
		ph += (uint32_t)(185 * 65536 / PD_RATE);
		at = (int32_t)((int64_t)at * kt >> 16);
		an = (int32_t)((int64_t)an * kn >> 16);
	}
	normalize(s, 26000);
	return true;
}

static bool clap(void) {
	pd_sample_t *s;
	uint32_t i, n = PD_RATE * 25 / 100;
	int lp = 0, prev = 0, bp;
	if (!make(PD_I_CLAP, n, 60)) return false;
	s = &pd_samples[PD_I_CLAP];
	for (i = 0; i < n; i++) {
		int x = noise(), env;
		uint32_t ms = i * 1000 / PD_RATE;
		/* three quick slaps, then the room */
		if (ms < 30) env = 32767 - (int)((ms % 10) * 3000);
		else env = (int)(32767 * 8 / (8 + (ms - 30) / 6));
		lp += (x - lp) >> 2;
		bp = lp - prev; prev = lp;
		s->d[i] = clip16((bp * 3) * env >> 15);
	}
	normalize(s, 22000);
	return true;
}

static bool hat(int which, int ms_len, int tau) {
	pd_sample_t *s;
	uint32_t i, n = (uint32_t)(PD_RATE * ms_len / 1000);
	int32_t a = 32767, k = decay_k(PD_RATE * tau / 1000);
	int prev = 0, prev2 = 0;
	if (!make(which, n, 60)) return false;
	s = &pd_samples[which];
	for (i = 0; i < n; i++) {
		int x = noise();
		int hp = x - 2 * prev + prev2;              /* second difference: hiss */
		prev2 = prev; prev = x;
		s->d[i] = clip16((hp >> 1) * a >> 15);
		a = (int32_t)((int64_t)a * k >> 16);
	}
	normalize(s, 16000);
	return true;
}

/* -- tones -- */

/* A naive saw (whole Hz): its value at phase p (0 .. PD_RATE-1, in
 * samples) over the number of saws summed, a table made once. A phase
 * a saw rather than (i * hz) % PD_RATE: no division a sample (on the
 * board a 64-bit one is a library call, hundreds of cycles), the same
 * values. */
static int16_t *saw_t;
static int saw_nhz;

static bool saw_table(int nhz) {
	uint32_t p;
	if (saw_t && saw_nhz == nhz) return true;
	free(saw_t);
	saw_t = (int16_t *)malloc(PD_RATE * sizeof(int16_t));
	if (!saw_t) return false;
	for (p = 0; p < PD_RATE; p++) saw_t[p] = (int16_t)(((int)(p * 65535u / PD_RATE) - 32768) / nhz);
	saw_nhz = nhz;
	return true;
}

/* Sum of detuned saws over one second, low-passed: loops seamlessly. */
static bool saws(int which, const int *hz, int nhz, int lp_shift, int base) {
	pd_sample_t *s;
	uint32_t i, n = PD_RATE, ph[8] = { 0 };
	int32_t y1 = 0, y2 = 0;
	if (nhz > 8 || !saw_table(nhz) || !make(which, n, base)) return false;
	s = &pd_samples[which];
	/* run the filter over a second first, so the loop start is the
	 * filter's steady state too */
	for (i = 0; i < 2 * n; i++) {
		int k, v = 0;
		for (k = 0; k < nhz; k++) {
			v += saw_t[ph[k]];
			ph[k] += (uint32_t)hz[k];
			if (ph[k] >= PD_RATE) ph[k] -= PD_RATE;
		}
		y1 += (v - y1) >> lp_shift;
		y2 += (y1 - y2) >> lp_shift;
		if (i >= n) s->d[i - n] = clip16(y2);
	}
	s->loop_st = 0;
	s->loop_n = n;
	normalize(s, 24000);
	return true;
}

/* A plucked tone: `cycle` samples a period (a whole number), harmonics
 * from a pulse of the given duty, the brightness falling over
 * `ring` samples; it loops its last period. */
static bool pluck(int which, int cycle, int duty_pct, int rings, int base, int sub) {
	pd_sample_t *s;
	uint32_t i, n = (uint32_t)(cycle * rings);
	int32_t y = 0;
	int shift0 = 1, a = 0;
	if (!make(which, n, base)) return false;
	s = &pd_samples[which];
	for (i = 0; i < n; i++) {
		int ph = (int)(i % (uint32_t)cycle);
		int x = ph < cycle * duty_pct / 100 ? 22000 : -22000;
		int sh;
		if (sub) x = x / 2 + (ph < cycle / 2 ? 12000 : -12000) * ((i / (uint32_t)cycle) & 1 ? 1 : 1);
		/* the filter closes as it rings: shift grows from 1 to 6 */
		sh = shift0 + (int)(i * 5 / n);
		y += (x - y) >> sh;
		s->d[i] = clip16((int)y);
		(void)a;
	}
	s->loop_st = n - (uint32_t)cycle;
	s->loop_n = (uint32_t)cycle;
	normalize(s, 24000);
	return true;
}

static bool thunder(void) {
	pd_sample_t *s;
	uint32_t i, n = PD_RATE * 18 / 10;
	int64_t l1 = 0, l2 = 0;
	int32_t a = 32767, k = decay_k(PD_RATE * 6 / 10), crack = 32767, kc = decay_k(PD_RATE / 25);
	if (!make(PD_I_THUNDER, n, 60)) return false;
	s = &pd_samples[PD_I_THUNDER];
	for (i = 0; i < n; i++) {
		int x = noise();
		/* a rumble that rolls: slow beats of loudness */
		/* i * 3 * 65536 / 35200 is i * 1536 / 275: 32 bits */
		int roll = 20000 + (sinp(i * 1536u / 275u) >> 2) +
			(sinp(i * 3584u / 275u) >> 3);
		l1 += ((int64_t)x - l1) >> 5;
		l2 += (l1 - l2) >> 5;
		s->d[i] = clip16((int)((l2 * 6 * roll >> 15) * a >> 15) + (x * crack >> 17));
		a = (int32_t)((int64_t)a * k >> 16);
		crack = (int32_t)((int64_t)crack * kc >> 16);
	}
	normalize(s, 30000);
	return true;
}

static bool tick(void) {
	pd_sample_t *s;
	uint32_t i, n = PD_RATE / 40, ph = 0;
	int32_t a = 32767, k = decay_k(PD_RATE / 400);
	if (!make(PD_I_TICK, n, 60)) return false;
	s = &pd_samples[PD_I_TICK];
	for (i = 0; i < n; i++) {
		s->d[i] = clip16(((sinp(ph) + (noise() >> 2)) * a) >> 15);
		ph += (uint32_t)(2600 * 65536 / PD_RATE);
		a = (int32_t)((int64_t)a * k >> 16);
	}
	normalize(s, 20000);
	return true;
}

/* Metal hitting the floor: four inharmonic partials, struck three
 * times as the hands bounce. */
static bool clatter(void) {
	static const int part[4] = { 1230, 1810, 2550, 3370 };
	static const int hits[3][2] = { { 0, 32767 }, { 140, 16000 }, { 240, 7000 } };
	pd_sample_t *s;
	uint32_t i, n = PD_RATE * 8 / 10;
	int h, k;
	if (!make(PD_I_CLATTER, n, 60)) return false;
	s = &pd_samples[PD_I_CLATTER];
	for (h = 0; h < 3; h++) {
		uint32_t at = (uint32_t)(hits[h][0] * PD_RATE / 1000);
		int32_t a = hits[h][1], kd = decay_k(PD_RATE / 9);
		uint32_t ph[4] = { 0, 0, 0, 0 };
		for (i = at; i < n; i++) {
			int v = 0;
			for (k = 0; k < 4; k++) {
				v += sinp(ph[k]) >> 2;
				ph[k] += (uint32_t)((part[k] + h * 37) * 65536 / PD_RATE);
			}
			if (i - at < 40) v += noise() >> 2;
			s->d[i] = clip16(s->d[i] + (v * a >> 15));
			a = (int32_t)((int64_t)a * kd >> 16);
		}
	}
	normalize(s, 22000);
	return true;
}

static bool noise_loop(void) {
	pd_sample_t *s;
	uint32_t i, n = PD_RATE / 2;
	if (!make(PD_I_NOISE, n, 69)) return false;
	s = &pd_samples[PD_I_NOISE];
	/* darkened noise: played higher as the riser climbs, its brightness
	 * climbs with it -- the sweep comes from the pitch */
	{
		int64_t y1 = 0, y2 = 0;
		for (i = 0; i < 2 * n; i++) {
			y1 += ((int64_t)noise() - y1) >> 3;
			y2 += (y1 - y2) >> 3;
			if (i >= n) s->d[i - n] = clip16((int)(y2 * 3));
		}
	}
	normalize(s, 20000);
	s->loop_st = 0;
	s->loop_n = n;
	return true;
}

/* A bell: a sine and a partial at 2.76, struck and fading. A5. */
static bool bell(void) {
	pd_sample_t *s;
	uint32_t i, n = PD_RATE * 6 / 10, p1 = 0, p2 = 0;
	int32_t a = 32767, k = decay_k(PD_RATE / 6), a2 = 32767, k2 = decay_k(PD_RATE / 20);
	if (!make(PD_I_BELL, n, 81)) return false;
	s = &pd_samples[PD_I_BELL];
	for (i = 0; i < n; i++) {
		s->d[i] = clip16((sinp(p1) * a >> 15) + (sinp(p2) * a2 >> 16));
		p1 += (uint32_t)(880 * 65536 / PD_RATE);
		p2 += (uint32_t)(2429 * 65536 / PD_RATE);
		a = (int32_t)((int64_t)a * k >> 16);
		a2 = (int32_t)((int64_t)a2 * k2 >> 16);
	}
	normalize(s, 22000);
	return true;
}

bool pd_synth_all(void) {
	/* the lead: A4 and A3 as five saws, a few Hz apart (whole Hz, so
	 * the second loops); the pad: A3 and A2, softer */
	static const int lead_hz[6] = { 440, 437, 443, 435, 445, 220 };
	static const int pad_hz[6] = { 220, 218, 222, 330, 331, 110 };   /* A3, E4 (2 cents sharp), A2 */

	bool ok;
	sin_init();
	ok = kick() && snare() && clap() &&
		hat(PD_I_HATC, 60, 14) && hat(PD_I_HATO, 380, 110) &&
		pluck(PD_I_BASS, 640, 50, 24, 33, 1) &&        /* A1: 640 samples */
		pluck(PD_I_PLUCK, 80, 25, 160, 69, 0) &&       /* A4 */
		saws(PD_I_LEAD, lead_hz, 6, 2, 69) &&
		saws(PD_I_PAD, pad_hz, 6, 4, 57) &&
		thunder() && tick() && clatter() && noise_loop() && bell();
	free(saw_t);
	saw_t = NULL;
	return ok;
}
