/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The formant synthesiser. See synth.h.
 *
 * -- Fixed point --
 *
 * Resonator coefficients are Q13, signals are kept within roughly
 * +/-2^16, and every product fits a 32-bit accumulator with room to
 * spare. "Roughly" is checked, not assumed: a host build with
 * SYNTH_STATS records the largest intermediate value any resonator
 * produced over the whole test set (tests/synth_test.c fails if it
 * gets within 2x of overflow).
 *
 * A two-pole resonator, y[n] = A x[n] + B y[n-1] + C y[n-2], with
 *
 *     r = exp(-pi BW / fs)     B = 2 r cos(2 pi F / fs)
 *     C = -r^2                 A = 1 - B - C
 *
 * is the textbook digital resonator (unity gain at DC); r and cos()
 * come from synth_tables.h.
 */

#include <stdint.h>
#include <stdbool.h>

#include "synth.h"
#include "synth_tables.h"

#if SYNTH_TABLES_FS != SYNTH_FS
#error "synth_tables.h was generated for a different sample rate -- rerun gen_tables.py"
#endif

#ifdef SYNTH_STATS
int32_t synth_peak_internal;
int32_t synth_peak_out;
uint32_t synth_clipped;
#define TRACK(v) do { int64_t _t = (v); if (_t < 0) _t = -_t; \
	if (_t > synth_peak_internal) synth_peak_internal = (int32_t)(_t > 0x7fffffff ? 0x7fffffff : _t); } while (0)
#else
#define TRACK(v) do { } while (0)
#endif

typedef struct {
	int32_t a, b, c;
	int32_t y1, y2;
} res_t;

static void res_set(res_t *r, uint32_t f, uint32_t bw) {

	// Never a resonator that does not decay: a pole radius of 1 (zero
	// bandwidth) rings forever, and at 0 Hz it integrates -- either
	// runs a 32-bit accumulator over within a fraction of a second.
	// Anything asking for these is a bug upstream; this makes it a
	// dull sound instead of an overflow.
	if (bw < 20) bw = 20;
	if (f < 50) f = 50;
	if (f >= SYNTH_FS / 2) f = SYNTH_FS / 2 - 1;
	uint32_t ci = f * (2 * SYNTH_COS_N) / SYNTH_FS;
	uint32_t bi = bw / SYNTH_BW_STEP;
	if (bi >= SYNTH_BW_N) bi = SYNTH_BW_N - 1;

	int32_t rad = synth_radius[bi];				// Q14
	int32_t cs = synth_cos[ci];					// Q14
	int32_t b = (2 * rad * cs) >> 14;			// Q14
	int32_t c = -((rad * rad) >> 14);			// Q14
	int32_t a = 16384 - b - c;

	r->a = a >> 1;								// Q13
	r->b = b >> 1;
	r->c = c >> 1;

}

static inline int32_t res_run(res_t *r, int32_t x) {
#ifdef SYNTH_STATS
	int64_t acc = (int64_t)r->a * x + (int64_t)r->b * r->y1 + (int64_t)r->c * r->y2;
	TRACK(acc);
	TRACK((int64_t)r->b * r->y1);
	int32_t y = (int32_t)(acc >> 13);
#else
	int32_t y = (r->a * x + r->b * r->y1 + r->c * r->y2) >> 13;
#endif
	r->y2 = r->y1;
	r->y1 = y;
	return y;
}

// One resonator over a block, in place: coefficients and state held
// in registers for the whole block. Same arithmetic as res_run().
#define SYNTH_BLOCK	64
static int32_t blk[SYNTH_BLOCK], fric[SYNTH_BLOCK];

// A resonator with nothing going in and nothing left ringing is
// silence, and running it only costs time -- which during a fricative,
// with the whole vowel cascade idle, was most of the time spent. "Nothing
// left" is |y| <= 2, not 0: integer resonators can idle at +/-1 forever
// (a limit cycle, from truncating), which never reaches 0 and is far
// below anything audible. Their state is zeroed when skipped.
static int quiet(const res_t *r) {
	return r->y1 >= -2 && r->y1 <= 2 && r->y2 >= -2 && r->y2 <= 2;
}

static int block_silent(const int32_t *x, int n) {
	for (int i = 0; i < n; i++)
		if (x[i]) return 0;
	return 1;
}

static void res_block(res_t *r, int32_t *x, int n) {
	int32_t a = r->a, b = r->b, c = r->c, y1 = r->y1, y2 = r->y2;
	for (int i = 0; i < n; i++) {
#ifdef SYNTH_STATS
		int64_t acc = (int64_t)a * x[i] + (int64_t)b * y1 + (int64_t)c * y2;
		TRACK(acc);
		TRACK((int64_t)b * y1);
		int32_t y = (int32_t)(acc >> 13);
#else
		int32_t y = (a * x[i] + b * y1 + c * y2) >> 13;
#endif
		y2 = y1;
		y1 = y;
		x[i] = y;
	}
	r->y1 = y1;
	r->y2 = y2;
}

// A parallel resonator: its input is the frication source scaled by
// `amp`, and its output is added to (or subtracted from) `acc`.
static void res_block_add(res_t *r, const int32_t *src, int32_t amp, int32_t *acc,
	int n, int sign) {
	int32_t a = r->a, b = r->b, c = r->c, y1 = r->y1, y2 = r->y2;
	for (int i = 0; i < n; i++) {
		int32_t x = (src[i] * amp) >> 12;
#ifdef SYNTH_STATS
		int64_t t = (int64_t)a * x + (int64_t)b * y1 + (int64_t)c * y2;
		TRACK(t);
		TRACK((int64_t)b * y1);
		int32_t y = (int32_t)(t >> 13);
#else
		int32_t y = (a * x + b * y1 + c * y2) >> 13;
#endif
		y2 = y1;
		y1 = y;
		acc[i] += (sign > 0) ? y : -y;
	}
	r->y1 = y1;
	r->y2 = y2;
}


static void res_clear(res_t *r) {
	r->y1 = r->y2 = 0;
}

// -- state --

static res_t r1, r2, r3, r4, r5;			// cascade
static res_t p2, p3, p4, p5, p6;			// parallel (frication)
static bool par_live;

static uint32_t noise_state = 0x1234567u;

static int32_t gl_pos;			// samples into the current pitch period
static int32_t gl_period;		// this period's length, samples
static int32_t gl_open;			// open phase, samples
static int32_t gl_inv_open;		// 65536 / gl_open
static uint32_t gl_frac;		// fractional period carry, Q8

static int32_t volume = 200;

// Source scales, in bits. Separate because the three sources reach
// the output through very different gains: voicing goes through five
// cascaded resonators tuned to its own harmonics, frication through
// one parallel resonator each. With a single scale, /s/ came out 20dB
// above the vowels around it (measured: RMS ~5000 against ~400), and
// speech whose vowels are barely there is speech nobody can follow.
// Set so a vowel at av=60 and a strong fricative at af=60 land within
// a few dB of each other, fricative below -- tests/tts_wav.c checks
// the headroom this leaves.
#define VOICE_SHIFT	0
#define ASP_SHIFT	0
#define FRIC_SHIFT	4
#define OUT_SHIFT	8

// The upper formants, scaled with the rest of the vocal tract. A
// shorter tract raises EVERY resonance, F4 and F5 included; leaving
// them fixed while F1-F3 rise crowds F3 into F4, and two resonators
// that close multiply each other's peaks -- a female voice came out
// 12dB louder on aspiration before this, and clipped. Capped below
// Nyquist. The fricative-only p6 stays where it is.
// A short description of how this synthesiser is built, for the
// service's startup line.
const char *synth_build(void) {
	return "block-processed, silence skipped, ring 1.5s";
}

void synth_set_tract(uint32_t pct) {
	if (pct < 85) pct = 85;
	if (pct > 120) pct = 120;
	uint32_t f4 = 3300 * pct / 100, f5 = 3850 * pct / 100, p5f = 3900 * pct / 100;
	if (f4 > 4600) f4 = 4600;
	if (f5 > 5000) f5 = 5000;
	if (p5f > 5000) p5f = 5000;
	res_set(&r4, f4, 250);
	res_set(&r5, f5, 300);
	res_set(&p4, f4, 250);
	res_set(&p5, p5f, 300);
}

void synth_set_volume(uint32_t v) {
	volume = (int32_t)(v > 255 ? 255 : v);
}

// -- the voice source's quality --
//
// Two numbers shape a voice as much as its formants do, and both were
// fixed: the OPEN QUOTIENT, the share of each period the vocal folds
// are open, and the spectral TILT, how fast the source falls off with
// frequency. A longer open phase and a steeper tilt are a softer,
// breathier voice; a shorter phase and no tilt, a pressed and buzzy one.
// tools/speech measures both in a real speaker (lib/source.py).
static uint32_t oq_pct = 50;
static int32_t tilt_a;			// one-pole low-pass coefficient, Q15
static int32_t tilt_y;

// The coefficient for each 3dB of attenuation at 3kHz, 0..24dB.
static const uint16_t tilt_coef[9] = {
	0, 8112, 14032, 18624, 22208, 24968, 27064, 28632, 29784,
};

void synth_set_source(uint32_t open_pct, uint32_t tilt_db) {
	if (open_pct < 35) open_pct = 35;
	if (open_pct > 80) open_pct = 80;
	oq_pct = open_pct;
	if (tilt_db > 24) tilt_db = 24;
	// Between table entries, linearly.
	uint32_t i = tilt_db / 3, f = tilt_db % 3;
	int32_t a = tilt_coef[i];
	if (f && i < 8) a += (int32_t)(tilt_coef[i + 1] - tilt_coef[i]) * (int32_t)f / 3;
	tilt_a = a;
}

void synth_init(void) {

	res_clear(&r1); res_clear(&r2); res_clear(&r3);
	res_clear(&r4); res_clear(&r5);
	res_clear(&p2); res_clear(&p3); res_clear(&p4);
	res_clear(&p5); res_clear(&p6);

	// The fixed upper formants, cascade and parallel.
	res_set(&r4, 3300, 250);
	res_set(&r5, 3850, 300);
	res_set(&p4, 3300, 250);
	res_set(&p5, 3900, 300);
	res_set(&p6, 4900, 500);

	par_live = false;
	tilt_y = 0;
	gl_pos = 0;
	gl_period = 0;
	gl_frac = 0;

}

static inline int32_t noise(void) {
	noise_state = noise_state * 1103515245u + 12345u;
	return (int32_t)noise_state >> 19;			// +/-4096, Q12
}

static inline int32_t lin(uint8_t db) {
	return synth_db[db < SYNTH_DB_N ? db : SYNTH_DB_N - 1];	// Q12
}

// Starts a new pitch period at `f0` (Hz * 16). The period carries its
// fractional part forward, so the average pitch is exact even though
// each period is a whole number of samples.
static void glottal_period(uint32_t f0) {

	if (f0 < 40 * 16) f0 = 40 * 16;

	uint32_t p8 = (uint32_t)SYNTH_FS * 16u * 256u / f0 + gl_frac;	// Q8 samples
	gl_period = (int32_t)(p8 >> 8);
	gl_frac = p8 & 0xff;

	gl_open = gl_period * (int32_t)oq_pct / 100;
	if (gl_open < 2) gl_open = 2;
	gl_inv_open = 65536 / gl_open;

	gl_pos = 0;

}

// One sample of the glottal flow DERIVATIVE: during the open phase the
// flow is t^2 - t^3 (t = 0..1), whose derivative 2t - 3t^2 rises, then
// swings negative and ends at -1, where the folds snap shut and the
// flow stops -- that abrupt return to zero is the main excitation of
// every pitch period. Q12.
static inline int32_t glottal(uint32_t f0) {

	if (gl_pos >= gl_period) glottal_period(f0);

	int32_t e = 0;
	if (gl_pos < gl_open) {
		int32_t t = gl_pos * gl_inv_open;			// Q16, 0..65535
		int32_t t2 = (t >> 1) * (t >> 1) >> 14;		// t^2, Q16
		e = (2 * t - 3 * t2) >> 4;					// Q12
	}

	gl_pos++;
	return e;

}

void synth_render(const synth_frame_t *f, int16_t *out, int n) {

	res_set(&r1, f->f1, f->b1);
	res_set(&r2, f->f2, f->b2);
	res_set(&r3, f->f3, f->b3);

	int32_t av = lin(f->av);
	int32_t ah = lin(f->ah);
	int32_t af = lin(f->af);

	bool par = f->af != 0;
	int32_t a2 = 0, a3 = 0, a4 = 0, a5 = 0, a6 = 0, ab = 0;
	if (par) {
		res_set(&p2, f->f2, f->b2);
		res_set(&p3, f->f3, f->b3);
		a2 = lin(f->a2); a3 = lin(f->a3); a4 = lin(f->a4);
		a5 = lin(f->a5); a6 = lin(f->a6); ab = lin(f->ab);
		par_live = true;
	} else if (par_live) {
		// Frication has ended: let the parallel resonators start from
		// silence next time rather than from whatever they last held.
		res_clear(&p2); res_clear(&p3); res_clear(&p4);
		res_clear(&p5); res_clear(&p6);
		par_live = false;
	}

	bool voiced = f->av != 0;

	// Block processing: the frame is built stage by stage over a
	// buffer, each resonator run across the whole block with its
	// coefficients and state in registers, instead of every sample
	// being carried through every stage in turn. The arithmetic is the
	// same, in the same order -- the output is identical to the
	// sample-by-sample version, bit for bit -- but each resonator costs
	// one load and one store per sample instead of a call and seven
	// memory operations. On the target, where memory is most of the
	// cost of an instruction, that was the difference between speech
	// taking 80% of the CPU and a fraction of it.
	for (int base = 0; base < n; base += SYNTH_BLOCK) {

		int cnt = n - base;
		if (cnt > SYNTH_BLOCK) cnt = SYNTH_BLOCK;

		// The sources. Aspiration noise is drawn before frication
		// noise for each sample, exactly as before, so the noise
		// sequence -- and so the output -- is unchanged.
		for (int i = 0; i < cnt; i++) {
			int32_t x = 0;
			// A voiceless frame still runs the pitch clock, so voicing
			// resumes in phase rather than with a click.
			int32_t g = glottal(f->f0);
			// Tilt: a one-pole low-pass on the source alone, so it
			// softens the voice without touching the noise sources.
			if (tilt_a) {
				tilt_y = g + (int32_t)(((int64_t)(tilt_y - g) * tilt_a) >> 15);
				g = tilt_y;
			}
			if (voiced) x = (g * av) >> (12 + VOICE_SHIFT);
			if (f->ah) x += (noise() * ah) >> (12 + ASP_SHIFT);
			blk[i] = x;
			if (par) fric[i] = (noise() * af) >> (12 + FRIC_SHIFT);
		}

		// The cascade -- unless there is nothing in it: silence in, and
		// every resonator finished ringing.
		if (block_silent(blk, cnt) && quiet(&r1) && quiet(&r2) && quiet(&r3) &&
		    quiet(&r4) && quiet(&r5)) {
			r1.y1 = r1.y2 = r2.y1 = r2.y2 = r3.y1 = r3.y2 = 0;
			r4.y1 = r4.y2 = r5.y1 = r5.y2 = 0;
		} else {
			res_block(&r1, blk, cnt);
			res_block(&r2, blk, cnt);
			res_block(&r3, blk, cnt);
			res_block(&r4, blk, cnt);
			res_block(&r5, blk, cnt);
		}

		// The parallel branch, alternating signs as in Klatt's, so
		// adjacent resonators' skirts do not cancel into holes.
		// Each one only if it has input or is still ringing: a fricative
		// typically drives two or three of the five.
		if (par) {
			if (a2 || !quiet(&p2)) res_block_add(&p2, fric, a2, blk, cnt, +1);
			if (a3 || !quiet(&p3)) res_block_add(&p3, fric, a3, blk, cnt, -1);
			if (a4 || !quiet(&p4)) res_block_add(&p4, fric, a4, blk, cnt, +1);
			if (a5 || !quiet(&p5)) res_block_add(&p5, fric, a5, blk, cnt, -1);
			if (a6 || !quiet(&p6)) res_block_add(&p6, fric, a6, blk, cnt, +1);
			for (int i = 0; i < cnt; i++) blk[i] += (fric[i] * ab) >> 12;
		}

		for (int i = 0; i < cnt; i++) {

			int32_t s = (blk[i] * volume) >> OUT_SHIFT;

#ifdef SYNTH_STATS
			{ int32_t m = s < 0 ? -s : s; if (m > synth_peak_out) synth_peak_out = m; }
#endif

			if (s > 32767) {
				s = 32767;
#ifdef SYNTH_STATS
				synth_clipped++;
#endif
			} else if (s < -32768) {
				s = -32768;
#ifdef SYNTH_STATS
				synth_clipped++;
#endif
			}

			out[base + i] = (int16_t)s;

		}

	}

}
