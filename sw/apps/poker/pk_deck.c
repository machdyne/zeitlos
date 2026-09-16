/*
 * Zeitlos poker -- the deck.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See pk_deck.h for why the generator is injected and why the shuffle
 * does not use a modulo.
 */

#include "pk_deck.h"

static pk_rng_fn pk_rng = 0;
static void     *pk_rng_ctx = 0;

/* xorshift32. Any nonzero seed works; zero is a fixed point and would
 * produce a deck that never moves, so pk_rng_seed() refuses it. */
static uint32_t pk_state = 0x9e3779b9u;

void pk_rng_set(pk_rng_fn fn, void *ctx)
{
    pk_rng = fn;
    pk_rng_ctx = ctx;
}

void pk_rng_seed(uint32_t seed)
{
    pk_state = seed ? seed : 0x9e3779b9u;
}

static uint32_t pk_builtin_u32(void)
{
    uint32_t x = pk_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    pk_state = x;
    return x;
}

static uint32_t pk_raw(void)
{
    if (pk_rng) return pk_rng(pk_rng_ctx);
    return pk_builtin_u32();
}

uint32_t pk_rng_below(uint32_t n)
{
    uint32_t reject, bound, v;

    if (n <= 1) return 0;

    /* Rejection sampling, for the reason pk_deck.h gives: `% n` alone
     * is biased toward low values, and in a shuffle that bias is
     * permanent and invisible.
     *
     * `reject` is how many of the 2^32 possible words have to be
     * thrown away for the rest to divide evenly by n, i.e. 2^32 mod n.
     * It is computed in 32 bits as ((2^32 - 1) mod n + 1) mod n, which
     * avoids a 64-bit modulo -- that would be a __umoddi3 call on
     * rv32i, on a path the rollouts hit a few hundred thousand times a
     * hand.
     *
     * WHEN n DIVIDES 2^32 THERE IS NOTHING TO REJECT, and that case
     * has to be handled separately rather than falling out of the
     * arithmetic: the accept bound would be 2^32, which is not
     * representable, and writing it in a uint32_t gives 0 -- a loop
     * that never ends. That is exactly the bug in the shared helper
     * this replaced, and a deck shuffle hits it five times. */
    reject = ((0xffffffffu % n) + 1u) % n;
    if (reject == 0) return pk_raw() % n;

    bound = (uint32_t)(0u - reject);      /* 2^32 - reject */

    do {
        v = pk_raw();
    } while (v >= bound);

    return v % n;
}

void pk_deck_init(pk_deck_t *d)
{
    int i;

    for (i = 0; i < PK_NCARDS; i++) d->card[i] = (uint8_t)i;
    d->n = PK_NCARDS;
    d->pos = 0;
}

void pk_deck_init_excluding(pk_deck_t *d, const uint8_t *seen, int n)
{
    bool gone[PK_NCARDS];
    int i;

    for (i = 0; i < PK_NCARDS; i++) gone[i] = false;

    for (i = 0; i < n; i++) {
        uint8_t c = seen[i];
        /* PK_CARD_NONE is expected here, not exceptional: an
         * opponent's hole cards are passed in as unknown when the AI
         * is estimating equity, and a folded seat's cards are never
         * dealt at all. */
        if (c < PK_NCARDS) gone[c] = true;
    }

    d->n = 0;
    d->pos = 0;

    for (i = 0; i < PK_NCARDS; i++)
        if (!gone[i]) d->card[d->n++] = (uint8_t)i;
}

void pk_deck_shuffle(pk_deck_t *d)
{
    int i;

    /* Downward from the top, swapping each card with one at or below
     * it. The upward variant that swaps with a card at or ABOVE it
     * looks equivalent and is not -- it produces n^n equally likely
     * outcomes over n! permutations, which do not divide, so some
     * orderings come up more often than others. */
    for (i = d->n - 1; i > d->pos; i--) {
        int j = d->pos + (int)pk_rng_below((uint32_t)(i - d->pos + 1));
        uint8_t t = d->card[i];
        d->card[i] = d->card[j];
        d->card[j] = t;
    }
}

uint8_t pk_deck_deal(pk_deck_t *d)
{
    if (d->pos >= d->n) return PK_CARD_NONE;
    return d->card[d->pos++];
}

int pk_deck_remaining(const pk_deck_t *d)
{
    return d->n - d->pos;
}

bool pk_deck_stack(pk_deck_t *d, const uint8_t *cards, int n)
{
    int k, i;

    if (n < 0 || d->pos + n > d->n) return false;

    /* Validated in full before anything moves, so a bad list leaves
     * the deck exactly as it was rather than half rearranged. A test
     * that stacked a duplicate and got a partially shuffled deck back
     * would produce a failure a long way from its cause. */
    for (k = 0; k < n; k++) {
        bool found = false;

        if (cards[k] >= PK_NCARDS) return false;

        for (i = 0; i < k; i++)
            if (cards[i] == cards[k]) return false;

        for (i = d->pos; i < d->n; i++)
            if (d->card[i] == cards[k]) { found = true; break; }

        if (!found) return false;
    }

    for (k = 0; k < n; k++) {
        int dst = d->pos + k;

        for (i = dst; i < d->n; i++) {
            if (d->card[i] != cards[k]) continue;
            d->card[i] = d->card[dst];
            d->card[dst] = cards[k];
            break;
        }
    }

    return true;
}
