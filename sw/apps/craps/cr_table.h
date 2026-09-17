#ifndef CR_TABLE_H
#define CR_TABLE_H

/*
 * Zeitlos craps -- the bets, and what each one does on a given roll.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * No I/O, no state beyond what is passed in. This file answers one
 * question -- given a bet, a roll and whether a point is on, does the
 * bet win, lose, push or stand? -- and the payout for a win.
 *
 * -- the edge is derived, not declared --
 *
 * The house edge of every bet here is a consequence of two dice and the
 * payout table, so tests/edge_test.c WALKS THE CHAIN and computes it in
 * exact integer arithmetic rather than comparing against a constant
 * somebody typed. A pass line bet works out to 244 wins in 495; a place
 * 6 to an expectation of exactly -1/66.
 *
 * That is the same arrangement as roulette's 36-units-returned and
 * slots' 155,060: a fact about the game rather than about the code, so
 * changing a payout moves a number a test is watching.
 *
 * -- and one bet has an edge of exactly zero --
 *
 * The odds behind the pass line pay TRUE ODDS. Not approximately, not
 * nearly: 2:1 on a four against a probability of exactly 1/3, so the
 * expectation is the integer 0. It is the only bet in a casino with no
 * edge at all, which is why it exists and why the house caps it at a
 * multiple of the flat bet.
 *
 * tests/edge_test.c asserts that zero as a rational with numerator 0 --
 * not "within a tolerance", which would pass on a bet that was merely
 * nearly fair.
 */

#include <stdint.h>
#include <stdbool.h>

/* The flat bets on the line, and the odds behind them. */
#define CR_PASS         0
#define CR_DONT_PASS    1
#define CR_PASS_ODDS    2
#define CR_DONT_ODDS    3

/* The same pair again, established after the point rather than on the
 * come-out. `sel` carries the come point once one is set. */
#define CR_COME         4
#define CR_DONT_COME    5
#define CR_COME_ODDS    6
#define CR_DONT_COME_ODDS 7

/* sel is the number: 4, 5, 6, 8, 9 or 10. */
#define CR_PLACE        8
#define CR_HARD         9      /* sel is 4, 6, 8 or 10 */

/* One-roll bets. */
#define CR_FIELD        10
#define CR_ANY7         11
#define CR_ANY_CRAPS    12
#define CR_ELEVEN       13
#define CR_TWO          14
#define CR_TWELVE       15

#define CR_NTYPES       16

/* What a bet does on a roll. */
#define CR_STAND   0    /* still there, undecided */
#define CR_WIN     1
#define CR_LOSE    2
#define CR_PUSH    3    /* returned -- the don't pass bar */
#define CR_MOVE    4    /* a come bet taking a point; sel becomes the roll */

/* Given `roll` (2..12) with the game's point at `point` (0 on the
 * come-out), what happens to this bet?
 *
 * `sel` is the bet's own number where it has one -- a place number, a
 * hardway, or the point a come bet has moved to (0 if it has not).
 */
int cr_resolve(int type, int sel, int roll, int point);

/* The hardways need the DICE, not the total: a six made of 4-2 loses
 * hard six and wins place six, from the same roll. It is the only bet
 * on the table that cares which pair came up. */
int cr_hard_resolve(int sel, int d1, int d2, int point);

/* What a winning bet returns per unit staked, as a ratio. Stake NOT
 * included: a winning pass line bet returns 1:1, which is the stake
 * back plus one more.
 *
 * `roll` matters only for the field, where 2 and 12 pay more than the
 * rest -- which is the whole reason the field is not a 50/50 bet
 * somebody has misread. */
void cr_payout(int type, int sel, int roll, int *num, int *den);

/* True if a bet of this type and selector can exist. */
bool cr_bet_valid(int type, int sel);

/* The maximum odds the house allows behind a flat bet of `flat` on
 * `point`. Capped because the bet is fair -- the house makes nothing on
 * it and carries the variance, so it only offers it in proportion to a
 * bet it does make money on. */
int32_t cr_max_odds(int point, int32_t flat);

const char *cr_type_name(int type);

/* Is `roll` a hard way -- both dice the same? Needs the dice, not the
 * total, which is the one place this game cares which pair came up. */
bool cr_is_hard(int d1, int d2);

#endif
