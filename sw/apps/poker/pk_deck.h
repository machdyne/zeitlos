#ifndef PK_DECK_H
#define PK_DECK_H

/*
 * Zeitlos poker -- the deck, the shuffle, and where randomness comes
 * from.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- the generator is injected, and that is what makes the game
 *    testable --
 *
 * Nothing in this file calls into Zeitlos. The app installs a wrapper
 * around z_rng_below() (sw/common/zrng.h) at startup; the host tests
 * install a counter-based generator and get the same deal twice.
 *
 * Without that, a test of the betting engine could only assert things
 * that are true of every hand, which rules out almost everything
 * worth asserting -- a side pot is only checkable against an exact
 * board and exact holdings.
 *
 * A DEFAULT is built in (xorshift32, fixed seed) so that a forgotten
 * pk_rng_set() produces a playable game rather than a null call
 * through a function pointer. It is deliberately a poor default: it
 * deals the same first hand every boot, which is obvious in seconds
 * and much better than a subtle bias nobody notices.
 *
 * -- why z_rng_below() and not z_rng_u32() % 52 --
 *
 * sw/common/zrng.h's own header makes the point: the modulo is biased
 * toward low values, and z_rng_below() rejection-samples instead. In a
 * shuffle that bias is not academic. It is a deck that deals low cards
 * to early positions slightly too often, forever, in a way that nobody
 * would ever catch by playing and that would quietly make the AI's
 * equity estimates wrong.
 *
 * The same header also says which question to ask about quality:
 * shuffling wants unpredictable-to-a-person and must NOT gate on
 * z_rng_secure(), which is for keys. A board with no TRNG still deals
 * a perfectly good game of poker.
 */

#include <stdint.h>
#include <stdbool.h>
#include "pk_cards.h"

/* A uniform 32-bit word. Nothing more.
 *
 * -- why the injected source is RAW and not bounded --
 *
 * It used to be `uint32_t (*)(uint32_t n, void *ctx)`, returning a
 * value already reduced into [0, n). That put the rejection-sampling
 * arithmetic in every implementation instead of in one place, and the
 * two implementations that existed were BOTH wrong, differently:
 *
 *   - sw/common/zrng.c's z_rng_below() computes its accept bound as
 *     2^32 - (2^32 % n), which for any power-of-two n is 2^32 and
 *     truncates to 0 in a uint32_t. `while (v >= 0)` on an unsigned
 *     never ends. A 52-card Fisher-Yates walks n from 52 down to 2, so
 *     it hits 32, 16, 8, 4 and 2 -- the FIRST shuffle hung the app
 *     before it had drawn anything, which looked like a blank window
 *     and a window manager timing out on a redraw acknowledgement.
 *
 *   - the version in this file rejected slightly the wrong interval,
 *     leaving an accept region that was not a multiple of n. A bias of
 *     two parts in four billion: harmless, invisible, and wrong.
 *
 * So the bound now lives in pk_rng_below() alone, and the thing being
 * injected is a source of entropy rather than a second copy of the
 * hard part. tests/eval_test.c pins the rejection boundary exactly.
 */
typedef uint32_t (*pk_rng_fn)(void *ctx);

void pk_rng_set(pk_rng_fn fn, void *ctx);

/* Reseeds the BUILT-IN generator. No effect once pk_rng_set() has
 * installed something else, which is the common case on hardware. */
void pk_rng_seed(uint32_t seed);

uint32_t pk_rng_below(uint32_t n);

typedef struct {
    uint8_t card[PK_NCARDS];
    int     n;      /* cards in the deck, dealt and undealt */
    int     pos;    /* index of the next card to come off the top */
} pk_deck_t;

/* A full 52-card deck in a fixed order. Not shuffled -- call
 * pk_deck_shuffle(). Separate so that a test can stack an unshuffled
 * deck and know exactly what it is holding. */
void pk_deck_init(pk_deck_t *d);

/* A deck with `n` named cards missing, undealt and in fixed order.
 *
 * What a Monte Carlo rollout needs: the cards the AI can see are the
 * ones it must not deal itself, and building the deck without them is
 * both faster and harder to get wrong than dealing and rejecting.
 */
void pk_deck_init_excluding(pk_deck_t *d, const uint8_t *seen, int n);

/* Fisher-Yates over the undealt portion. Safe to call mid-deck,
 * though nothing does. */
void pk_deck_shuffle(pk_deck_t *d);

/* The next card, or PK_CARD_NONE when the deck is exhausted.
 *
 * Exhaustion is a real case, not a defensive check: eight players in
 * seven-card stud need 56 cards plus burns. pk_game.c handles it the
 * way the rules do, by dealing a single community card, and it can
 * only do that if this reports the condition instead of wrapping. */
uint8_t pk_deck_deal(pk_deck_t *d);

int pk_deck_remaining(const pk_deck_t *d);

/* Forces the next `n` cards off the top to be exactly `cards`, in
 * order, by swapping them up from wherever they currently sit.
 *
 * For tests, and for nothing else -- there is no debug command that
 * reaches this, because a stacked deck that can be reached from the
 * command line is a cheat that will eventually be found. Returns
 * false, having changed nothing, if any named card has already been
 * dealt or appears twice.
 */
bool pk_deck_stack(pk_deck_t *d, const uint8_t *cards, int n);

#endif
