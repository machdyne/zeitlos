/*
 * Zeitlos -- wires zrand.c to the real generator.
 *
 * Its own file so that zrand.c stays free of Zeitlos headers and the
 * host tests can link it alone. An app links both and calls
 * zg_rng_use_system() once at startup.
 */

#include "zrand.h"
#include "../zrng.h"

/*
 * z_rng_u32(), NOT z_rng_below().
 *
 * zrand.h explains why at length; the short version is that
 * z_rng_below() never returns for a power-of-two bound. The bounding
 * belongs in zg_rng_below(), which is one place and is tested.
 *
 * NOT gated on z_rng_secure(). zrng.h states the rule plainly:
 * shuffling and games want unpredictable-to-a-person and should just
 * call z_rng_bytes(); keys and nonces check z_rng_secure() and refuse.
 * A board with no TRNG still deals a perfectly good game. An app that
 * wants to SAY which it got can ask z_rng_secure() itself and put it
 * on screen -- which is the useful thing to do with that bit, rather
 * than refusing to run.
 */
static uint32_t sys_source(void *ctx)
{
    (void)ctx;
    return z_rng_u32();
}

void zg_rng_use_system(void)
{
    zg_rng_set(sys_source, 0);
}
