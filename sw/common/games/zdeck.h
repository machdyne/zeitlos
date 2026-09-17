#ifndef ZDECK_H
#define ZDECK_H

/*
 * Zeitlos -- a deck, or a shoe of several.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Promoted from sw/apps/poker, where it was a single 52-card deck,
 * when blackjack arrived and wanted six of them in a shoe. That is the
 * point at which the shape of the shared thing is known rather than
 * guessed -- see docs/casino_bank.md on why nothing moves here before
 * there is a second caller.
 *
 * -- the generator is not in here --
 *
 * The version in poker carried its own injectable RNG and its own
 * rejection-sampling arithmetic, which is how the tree ended up with
 * two implementations of the same bound, both wrong in different ways.
 * This one calls zg_rng_below() (zrand.h), which is the only place that
 * arithmetic now lives.
 *
 * -- why the shuffle still matters --
 *
 * zg_rng_below() rejection-samples because `% n` is biased toward low
 * values. In a shuffle that bias is a deck that deals low cards to
 * early positions slightly too often, forever, in a way nobody would
 * catch by playing.
 *
 * And the Fisher-Yates runs DOWNWARD, swapping each card with one at or
 * below it. The upward variant that swaps with a card at or above it
 * looks equivalent and is not: it produces n^n equally likely outcomes
 * over n! permutations, which do not divide, so some orderings come up
 * more often than others.
 */

#include <stdint.h>
#include <stdbool.h>
#include "zcard.h"

/* Eight decks is the largest shoe anybody deals from. A zdeck_t is a
 * little over 400 bytes, which is fine as a global and fine on an
 * app's stack -- but it is worth knowing before putting one in a
 * recursive function. */
#define Z_DECK_MAX_DECKS 8
#define Z_DECK_MAX       (Z_DECK_MAX_DECKS * Z_NCARDS)

typedef struct {
    uint8_t card[Z_DECK_MAX];
    int     n;      /* cards in the shoe, dealt and undealt */
    int     pos;    /* index of the next card off the top */
    int     ndecks;
} zdeck_t;

/* `ndecks` packs, in a fixed order. Not shuffled -- call zdeck_shuffle().
 * Separate so a test can stack an unshuffled shoe and know exactly what
 * it is holding. */
void zdeck_init(zdeck_t *d, int ndecks);

/* One deck with `n` named cards missing, undealt and in fixed order.
 *
 * What a Monte Carlo rollout needs: the cards a player can see are the
 * ones it must not deal itself, and building the deck without them is
 * both faster and harder to get wrong than dealing and rejecting.
 */
void zdeck_init_excluding(zdeck_t *d, const uint8_t *seen, int n);

/* Fisher-Yates over the undealt portion. Safe mid-shoe, which is what
 * a blackjack reshuffle at the cut card does. */
void zdeck_shuffle(zdeck_t *d);

/* The next card, or Z_CARD_NONE when the shoe is empty.
 *
 * Exhaustion is a real case, not a defensive check: eight players in
 * seven-card stud need 56 cards from one deck, and the rules say the
 * last card is dealt face up as a community card. A caller can only do
 * that if this reports the condition instead of wrapping. */
uint8_t zdeck_deal(zdeck_t *d);

int zdeck_remaining(const zdeck_t *d);

/* How many cards have come off since the shoe was built. For a cut
 * card: reshuffle once penetration reaches some fraction. */
int zdeck_dealt(const zdeck_t *d);

/* Forces the next `n` cards off the top to be exactly `cards`, by
 * swapping them up from wherever they sit.
 *
 * For tests, and for nothing else -- there is no debug command that
 * reaches this, because a stacked deck reachable from the command line
 * is a cheat that will eventually be found.
 *
 * A MULTI-DECK SHOE MAY LEGITIMATELY REPEAT A CARD, so a repeated entry
 * is accepted as long as the shoe still holds enough of them. The
 * single-deck version this came from rejected any repeat, which would
 * have made half the interesting blackjack cases unstackable.
 *
 * Returns false, having changed nothing, if the list cannot be
 * satisfied. Validated in full before anything moves, so a bad list
 * leaves the shoe exactly as it was -- a half-rearranged shoe would
 * produce a test failure a long way from its cause. */
bool zdeck_stack(zdeck_t *d, const uint8_t *cards, int n);

#endif
