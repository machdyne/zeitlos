#ifndef KGRAND_H
#define KGRAND_H

/*
 * kidgames -- the one random number call the whole app makes.
 *
 * The original uses srand(time(NULL)) and rand(). Neither is right
 * here. There is no time() worth the name -- the RTC is optional and
 * may never have been set -- and rand() would be picolibc's, which
 * this app would rather not link at all.
 *
 * Underneath is z_rng_below() (sw/common/zrng.h): a ChaCha20 stream
 * seeded from rtl/trng.v where the board has one, and stirred from
 * whatever entropy exists where it does not. A board with no TRNG
 * still deals a different word each boot rather than the same one
 * forever, which is the failure a kid would notice first.
 *
 * -- WHY THIS IS ITS OWN FILE --
 *
 * So the host tests can replace it.
 *
 * z_rng_* reaches hardware registers that are unmapped on a build
 * machine, and more to the point a test of "does this game pick a word
 * of the right length for the level" is not a test of the generator.
 * tests/kg_rand_host.c supplies a small deterministic sequence
 * instead, so a failing word-list test names the same word every time
 * rather than a different one per run -- which is the difference
 * between a bug report and a rumour.
 *
 * This is the same split sw/apps/poker draws between its engine and
 * its I/O, and for the same reason.
 */

#include "kg.h"

/* A uniform value in [0, n). Returns 0 for n == 0 rather than dividing
 * by it -- callers that computed an empty range have a bug, and
 * crashing a kid's game is a poor way to report it. */
uint32_t kg_rand(uint32_t n);

/* Seed. Called once from main(); a no-op in the host build. */
void kg_rand_init(void);

#endif
