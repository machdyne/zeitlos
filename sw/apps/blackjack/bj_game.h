#ifndef BJ_GAME_H
#define BJ_GAME_H

/*
 * Zeitlos blackjack -- the rules.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * No I/O. This file and bj_game.c know nothing about the framebuffer,
 * the keyboard or the bank, which is what lets all of it be driven from
 * tests/game_test.c with a stacked shoe.
 *
 * Cards, the shoe and the shuffle come from sw/common/games -- promoted
 * out of sw/apps/poker when this app became their second caller. See
 * docs/casino_bank.md.
 *
 * -- an ace is eleven at most once --
 *
 * A hand's total counts every ace as one, then promotes ONE of them to
 * eleven if that still fits under 21. Two aces are twelve, not
 * twenty-two; three are thirteen. Writing the promotion as a loop over
 * every ace is the classic way to get A-A-A wrong, and it is wrong
 * quietly -- the hand is still playable, just valued a few too many.
 *
 * A hand is SOFT when that promotion happened, which is the only thing
 * that distinguishes soft 17 from hard 17 and therefore the only thing
 * standing between the dealer rules and nonsense.
 *
 * -- twenty-one is not always blackjack --
 *
 * A natural is exactly two cards. Twenty-one made from three, or from a
 * split, pays even money and loses the tie to a dealer's natural. Two
 * split aces drawing a ten each are two twenty-ones and neither is a
 * blackjack -- which is worth 50% of the bet, so it is not a rounding
 * detail.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/games/zcard.h"
#include "../../common/games/zdeck.h"

/* A hand cannot exceed this: twelve cards is four aces, four twos and
 * four threes, which totals 21 and cannot be extended. */
#define BJ_MAX_CARDS  12
#define BJ_MAX_HANDS  4       /* the original plus three splits */

/* -- rules ------------------------------------------------------------
 *
 * Every one of these changes the house edge, and the defaults are the
 * player-friendly side of each: six decks, dealer stands on all
 * seventeens, blackjack pays 3:2, doubling after a split allowed.
 *
 * 6:5 blackjack is the one that matters most and is offered because it
 * is what most modern tables actually pay. It raises the house edge
 * from about 0.4% to about 1.8% -- more than every other rule on this
 * list combined -- and a person should be able to see that happen.
 */
typedef struct {
    int  ndecks;
    bool dealer_hits_soft17;
    bool double_after_split;
    bool resplit_aces;
    bool surrender;           /* late surrender */
    int  bj_pay_num;          /* 3 and 2 for 3:2; 6 and 5 for 6:5 */
    int  bj_pay_den;
    int  max_hands;           /* splits allowed, including the original */
    int  penetration;         /* reshuffle once this % of the shoe is gone */
} bj_rules_t;

void bj_rules_default(bj_rules_t *r);

/* -- hand values ------------------------------------------------------- */

/* The value of a single card: two through nine, ten for any face, and
 * ELEVEN for an ace. The promotion back down to one is bj_total()'s
 * business, not this function's. */
int bj_card_value(uint8_t card);

/* The best total not exceeding 21, or the bust total if there is none.
 * `soft`, if non-NULL, says whether an ace is counting eleven. */
int bj_total(const uint8_t *cards, int n, bool *soft);

/* Exactly two cards totalling 21. */
bool bj_is_natural(const uint8_t *cards, int n);

/* Whether the dealer must draw again, by the configured rule. */
bool bj_dealer_draws(const bj_rules_t *r, const uint8_t *cards, int n);

/* -- a hand in play ----------------------------------------------------- */

typedef struct {
    uint8_t card[BJ_MAX_CARDS];
    int     n;
    int32_t bet;
    bool    doubled;
    bool    surrendered;
    bool    from_split;
    bool    split_ace;      /* split aces get one card and no more */
    bool    done;
    int32_t won;            /* returned at settlement, stake included */
} bj_hand_t;

/* -- a round ------------------------------------------------------------ */

#define BJ_PHASE_BETTING    0
#define BJ_PHASE_INSURANCE  1   /* dealer shows an ace */
#define BJ_PHASE_PLAYER     2
#define BJ_PHASE_DEALER     3
#define BJ_PHASE_DONE       4

#define BJ_HIT        0
#define BJ_STAND      1
#define BJ_DOUBLE     2
#define BJ_SPLIT      3
#define BJ_SURRENDER  4

typedef struct {

    bj_rules_t rules;
    zdeck_t    shoe;

    bj_hand_t  hand[BJ_MAX_HANDS];
    int        nhands;
    int        active;        /* which hand is being played */

    uint8_t    dealer[BJ_MAX_CARDS];
    int        ndealer;

    int32_t    insurance;     /* side bet, 0 if none */
    bool       insurance_offered;

    int        phase;

    int32_t    staked;        /* everything committed this round */
    int32_t    returned;      /* everything paid back at settlement */

    bool       needs_shuffle;

} bj_game_t;

void bj_game_init(bj_game_t *g, const bj_rules_t *r);

/* Deals a round for `bet`. Returns false if the bet is not positive or
 * the shoe cannot supply four cards. */
bool bj_round_begin(bj_game_t *g, int32_t bet);

/* Same, without shuffling -- for tests that have stacked the shoe. */
bool bj_round_begin_stacked(bj_game_t *g, int32_t bet);

/* What the active hand may do right now. */
typedef struct {
    bool can_hit, can_stand, can_double, can_split, can_surrender;
} bj_options_t;

void bj_options(const bj_game_t *g, bj_options_t *o);

/* Takes an action for the active hand and advances as far as it can --
 * to the next hand, to the dealer, or to settlement. Returns false and
 * changes NOTHING if the action is illegal. */
bool bj_act(bj_game_t *g, int action);

/* Takes or declines insurance. `amount` of 0 declines. */
bool bj_insure(bj_game_t *g, int32_t amount);

/* The dealer's up-card, or Z_CARD_NONE before the deal. */
uint8_t bj_dealer_up(const bj_game_t *g);

/* True once the hole card is public: at a showdown, or when the dealer
 * has a natural. */
bool bj_hole_shown(const bj_game_t *g);

const char *bj_action_name(int action);

#endif
