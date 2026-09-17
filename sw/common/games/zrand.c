/*
 * Zeitlos -- an unbiased bounded draw, for games.
 * See zrand.h for why this exists alongside z_rng_below().
 */

#include "zrand.h"
#include "../zrng.h"    /* for z_rng_reject_count() only -- see below */

static zg_rng_fn rng_fn = 0;
static void     *rng_ctx = 0;

/* The fallback generator's state. Any nonzero seed works; zero is a
 * fixed point, so zg_rng_seed() refuses it. */
static uint32_t state = 0x9e3779b9u;

void zg_rng_set(zg_rng_fn fn, void *ctx)
{
    rng_fn = fn;
    rng_ctx = ctx;
}

bool zg_rng_installed(void)
{
    return rng_fn != 0;
}

void zg_rng_seed(uint32_t seed)
{
    state = seed ? seed : 0x9e3779b9u;
}

static uint32_t builtin(void)
{
    uint32_t x;

    /* xorshift32 with a multiply-xor finaliser, NOT bare xorshift32.
     *
     * Bare xorshift32's low bits are weak -- they are a short linear
     * recurrence of their own -- and almost everything here reaches for
     * a small bound, which reads exactly those bits. A chi-square over
     * 370,000 draws does not notice; what noticed was sw/apps/craps
     * running a hundred thousand rounds and getting a house edge of
     * 0.35%, 1.06% or 0.55% from IDENTICAL code, decided only by how
     * many draws had been taken beforehand.
     *
     * The finaliser is splitmix32's: two multiply-and-xor-shift rounds,
     * which scatter the high bits down into the low ones. It costs two
     * multiplies on a path that is already doing a modulo.
     *
     * This is the FALLBACK, so on hardware it is not what runs --
     * zg_rng_use_system() installs a ChaCha20 stream, which has no such
     * weakness. It is what every host test runs on, which is where a
     * long simulation lives. */
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;

    x = state;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;

    return x;
}

static uint32_t raw(void)
{
    if (rng_fn) return rng_fn(rng_ctx);
    return builtin();
}

uint32_t zg_rng_below(uint32_t n)
{
    uint32_t reject, bound, v;

    if (n <= 1) return 0;

    /* How many of the 2^32 possible words have to be thrown away for
     * the rest to divide evenly by n.
     *
     * SHARED WITH sw/common/zrng.c rather than reimplemented. Two
     * copies of this arithmetic is exactly what produced two different
     * bugs -- z_rng_below() hung on every power of two, and an earlier
     * version of this function rejected slightly the wrong interval
     * and left a bias of two parts in four billion. One inline, one
     * set of tests.
     *
     * z_rng_reject_count() is a pure inline in a header. Including
     * zrng.h costs this file nothing at link time and keeps it
     * host-testable, which is the property the whole injection design
     * exists to preserve. */
    reject = z_rng_reject_count(n);
    /* NOTHING TO REJECT: n divides 2^32, every word is usable, and the
     * modulo is already unbiased. Handled separately because the
     * accept bound would be 2^32, which does not fit in the type it
     * has to live in -- it becomes 0 and the loop below never ends. */
    if (reject == 0) return raw() % n;

    bound = (uint32_t)(0u - reject);        /* 2^32 - reject */

    do {
        v = raw();
    } while (v >= bound);

    return v % n;
}

void zg_rng_bytes(void *buf, uint32_t len)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t w = 0;
    uint32_t have = 0;

    while (len--) {
        if (have == 0) { w = raw(); have = 4; }
        *p++ = (uint8_t)(w & 0xffu);
        w >>= 8;
        have--;
    }
}
