#ifndef SYNTH_H
#define SYNTH_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The formant synthesiser: turns a stream of parameter frames into
 * samples. Integer only (rv32im has no FPU), no allocation, the same
 * code on the target and on the build machine.
 *
 * The structure follows the cascade/parallel formant synthesiser
 * described in D. H. Klatt, "Software for a cascade/parallel formant
 * synthesizer", JASA 67(3), 1980 -- implemented from that published
 * description, not from any existing program:
 *
 *   voicing ----+
 *               +--> R1 -> R2 -> R3 -> R4 -> R5 --+
 *   aspiration -+                                  |
 *                                                  +--> out
 *   frication ---> R2p, R3p, R4p, R5p, R6p, bypass-+
 *                  (each with its own amplitude)
 *
 * Voicing is the derivative of a polynomial glottal flow pulse, which
 * already includes the radiation characteristic, so there is no
 * separate output differentiator. There is no nasal pole/zero pair:
 * nasals are made with a low, damped first formant and their
 * transitions, which avoids an antiresonator whose coefficients would
 * not fit comfortably in 32-bit fixed point.
 *
 * See docs/tts.md, "The voice".
 */

#include <stdint.h>

#define SYNTH_FS		11025	// output sample rate; gen_tables.py must agree
#define SYNTH_FRAME		55		// samples per parameter frame, ~5ms

// One frame's parameters. Frequencies and bandwidths in Hz,
// amplitudes in dB (0 = off, 60 = unity).
typedef struct {
	uint16_t	f0;			// pitch, Hz * 16
	uint8_t		av;			// voicing
	uint8_t		ah;			// aspiration (noise through the cascade)
	uint8_t		af;			// frication (noise through the parallel branch)
	uint16_t	f1, f2, f3;
	uint16_t	b1, b2, b3;
	uint8_t		a2, a3, a4, a5, a6, ab;	// parallel branch amplitudes
} synth_frame_t;

void synth_init(void);

// Renders `n` samples of frame `f` into `out`. Call once per frame,
// normally with n = SYNTH_FRAME; state carries across calls so the
// joins are seamless.
void synth_render(const synth_frame_t *f, int16_t *out, int n);

// Vocal tract size, % of the default (85..120): moves the fixed upper
// formants F4 and F5 with the rest. phon_begin() sets it.
void synth_set_tract(uint32_t pct);

// The source's quality: open quotient, % of each period the folds are
// open (35..80, default 50), and spectral tilt, dB of attenuation at
// 3kHz (0..24, default 0). phon_begin() sets them.
void synth_set_source(uint32_t open_pct, uint32_t tilt_db);

// How this synthesiser is built, for the service's startup line.
const char *synth_build(void);

// Output gain, 0..255 (volume). Default 200.
void synth_set_volume(uint32_t v);

#ifdef SYNTH_STATS
// Host builds: the largest magnitude seen inside any resonator, so a
// test can prove there is headroom before the 32-bit accumulators.
extern int32_t synth_peak_internal;
extern int32_t synth_peak_out;
extern uint32_t synth_clipped;
#endif

#endif
