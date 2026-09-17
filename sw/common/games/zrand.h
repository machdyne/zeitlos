#ifndef ZGAMES_RAND_H
#define ZGAMES_RAND_H

/*
 * Zeitlos -- an unbiased bounded draw, for games.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- why this exists when sw/common/zrng.h already has one --
 *
 * z_rng_below() does not return when `n` is a power of two. It computes
 * its accept bound as 2^32 - (2^32 % n), which for such an `n` is
 * exactly 2^32, and that truncates to 0 in the uint32_t holding it --
 * so `while (v >= limit)` on an unsigned never ends.
 *
 * It is not a hypothetical. A 52-card Fisher-Yates asks for bounds of
 * 52 down to 2 and hits 32, 16, 8, 4 and 2 on every shuffle;
 * sw/apps/poker hung on its first deal before it had drawn anything,
 * which presented as a blank window and wm timing out on a redraw.
 * sw/apps/repl's `(random 16)` has the same exposure.
 *
 * So the bound is computed here instead, once, with the case that
 * broke it handled explicitly: WHEN n DIVIDES 2^32 THERE IS NOTHING TO
 * REJECT, and the accept bound would be 2^32, which is not
 * representable in the type it has to live in.
 *
 * If z_rng_below() is fixed, this should stay anyway. Having one
 * tested implementation that every game shares is the point; the bug
 * is what made it urgent, not what makes it worth having.
 *
 * -- the source is injected --
 *
 * Nothing in zrand.c calls into Zeitlos, so the host tests can install
 * a source that hands out chosen words and observe exactly which ones
 * get rejected. That is the only way to see the accept region from
 * outside, and the accept region is where the bug lived.
 *
 * Link zrand_sys.c as well and call zg_rng_use_system() to get the
 * real generator. An app that forgets gets a fixed-seed fallback that
 * deals the same first hand every boot -- deliberately obvious in
 * seconds rather than subtly wrong forever.
 */

#include <stdint.h>
#include <stdbool.h>

/* A uniform 32-bit word. Nothing more: the bounding is done here.
 *
 * Bounded sources are what produced two different wrong answers in
 * this tree. The thing worth injecting is entropy, not a second copy
 * of the hard part. */
typedef uint32_t (*zg_rng_fn)(void *ctx);

void zg_rng_set(zg_rng_fn fn, void *ctx);

/* Installs sw/common/zrng.c's generator -- the ChaCha20 stream seeded
 * from rtl/trng.v where the board has one. Declared here, defined in
 * zrand_sys.c, so that this file stays free of Zeitlos headers and the
 * host tests can link it alone. */
void zg_rng_use_system(void);

/* True once zg_rng_set() or zg_rng_use_system() has been called. For
 * an app that wants to say so on screen rather than discover it. */
bool zg_rng_installed(void);

/* Reseeds the built-in fallback. No effect once a source is
 * installed. */
void zg_rng_seed(uint32_t seed);

/* Uniform in [0, n). Returns 0 for n of 0 or 1. Terminates for every
 * n, including every power of two. */
uint32_t zg_rng_below(uint32_t n);

/* Fills `buf` with `len` uniform bytes. */
void zg_rng_bytes(void *buf, uint32_t len);

#endif
