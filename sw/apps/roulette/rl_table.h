#ifndef RL_TABLE_H
#define RL_TABLE_H

/*
 * Zeitlos roulette -- the wheel, the layout, and what each bet pays.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * No I/O. This file and rl_table.c know nothing about the
 * framebuffer, the keyboard or the bank, which is what lets the whole
 * of it be checked exhaustively on the build machine.
 *
 * -- one wheel, two layouts --
 *
 * European has 37 pockets, 0 to 36. American adds a second zero, "00",
 * and that one extra pocket is the single most important fact about
 * the game: it nearly doubles the house edge, from 2.70% to 5.26%.
 * Both are offered because both exist, and European is the default
 * because it is the better game and somebody who does not know the
 * difference should not be quietly handed the worse one.
 *
 * -- THE INVARIANT THAT CHECKS EVERYTHING --
 *
 * On a European wheel, EVERY bet pays the same in the long run. Stake
 * one unit on any bet, and over all 37 pockets the total returned is
 * exactly 36 units:
 *
 *      straight  1 pocket  x 36  = 36
 *      split     2 pockets x 18  = 36
 *      street    3 pockets x 12  = 36
 *      corner    4 pockets x  9  = 36
 *      six line  6 pockets x  6  = 36
 *      column   12 pockets x  3  = 36
 *      dozen    12 pockets x  3  = 36
 *      red      18 pockets x  2  = 36
 *
 * That single number validates the payout ratio AND the coverage set
 * of every bet on the table at once. Pay a split 16:1 and it comes out
 * 34. Put 19 numbers in the red set and it comes out 38. There is no
 * way to be wrong about a payout or about which pockets a bet covers
 * and still land on 36.
 *
 * tests/table_test.c asserts it for every bet type and every selector,
 * which is the roulette equivalent of counting all 2,598,960 poker
 * hands: a property of the game rather than of anybody's code.
 *
 * The American wheel keeps 36 for every bet EXCEPT the five-number
 * basket (0, 00, 1, 2, 3), which pays 6:1 over 5 pockets and returns
 * 35. That is not a rounding artefact -- it is why the basket is the
 * worst bet on the table, and the test asserts it explicitly rather
 * than excusing it.
 */

#include <stdint.h>
#include <stdbool.h>

#define RL_EURO      0       /* 37 pockets: 0..36        */
#define RL_AMERICAN  1       /* 38 pockets: 0..36 and 00 */

/* "00" is pocket 37. Numbering it after 36 rather than before 0 keeps
 * every ordinary number equal to its own index, so nothing that walks
 * 1..36 has to know the double zero exists. */
#define RL_ZERO         0
#define RL_DOUBLE_ZERO  37
#define RL_MAX_POCKETS  38

int rl_pockets(int wheel);          /* 37 or 38 */

/* -- bets -------------------------------------------------------------
 *
 * `sel` means something different for each type, and what it means is
 * the thing most likely to be got wrong, so it is spelled out here and
 * validated by rl_bet_valid().
 */
#define RL_STRAIGHT  0   /* sel = the number, 0..36 (or 37 for 00)   */
#define RL_SPLIT     1   /* sel = split index, see rl_split_pair()   */
#define RL_STREET    2   /* sel = row, 0..11 -- numbers 3r+1..3r+3   */
#define RL_CORNER    3   /* sel = corner index, see rl_corner_set()  */
#define RL_SIXLINE   4   /* sel = row pair, 0..10                    */
#define RL_COLUMN    5   /* sel = 0..2, numbers n where n%3 == sel+1 */
#define RL_DOZEN     6   /* sel = 0..2, numbers 12*sel+1 .. 12*sel+12 */
#define RL_RED       7   /* sel unused */
#define RL_BLACK     8
#define RL_ODD       9
#define RL_EVEN      10
#define RL_LOW       11  /* 1..18  */
#define RL_HIGH      12  /* 19..36 */
#define RL_BASKET    13  /* American only: 0, 00, 1, 2, 3 at 6:1 */
#define RL_NTYPES    14

/* What one unit staked returns on a win, INCLUDING the stake. A
 * straight-up bet is quoted 35:1 and returns 36.
 *
 * Returning the total rather than the odds is deliberate: "35 to 1"
 * and "35 for 1" are different bets, the difference is exactly the
 * stake, and expressing it once here means nothing downstream has to
 * remember which convention it is holding. */
int rl_returns(int type);

/* How many selectors a type has on this wheel. 1 for the
 * even-money bets; 37 or 38 for straight-up. */
int rl_sel_count(int wheel, int type);

bool rl_bet_valid(int wheel, int type, int sel);

/* True if a bet of this type and selector wins on `pocket`. */
bool rl_covers(int wheel, int type, int sel, int pocket);

/* How many pockets a bet covers. Derived by asking rl_covers() about
 * every pocket, so it cannot disagree with it. */
int rl_coverage(int wheel, int type, int sel);

/* The two numbers of split `sel`, or the four of corner `sel`.
 * Returns the count written, or 0 if `sel` is out of range.
 *
 * Splits and corners are indexed rather than named because the layout
 * is what defines them: a split is any two numbers adjacent on the
 * printed table, which includes the vertical pairs (1-4) and the
 * horizontal ones (1-2) but not 3-4, which merely look adjacent in a
 * list. Getting that set right is most of what the coverage test is
 * for. */
int rl_split_pair(int sel, int *out);
int rl_corner_set(int sel, int *out);

int rl_n_splits(void);
int rl_n_corners(void);

/* Red, for a number 1..36. Zero and double zero are neither. */
bool rl_is_red(int pocket);

/* "17", "0", "00". Writes at most 3 bytes plus the NUL. */
char *rl_pocket_str(int pocket, char *buf);

/* "straight up", "split", "red"... for the bet list and the messages. */
const char *rl_type_name(int type);

/* The quoted odds, e.g. "35:1". */
const char *rl_type_odds(int type);

/* -- a round ----------------------------------------------------------- */

#define RL_MAX_BETS 24

typedef struct {
    uint8_t type;
    uint8_t sel;
    int32_t amount;
} rl_bet_t;

typedef struct {
    int      wheel;
    rl_bet_t bet[RL_MAX_BETS];
    int      nbets;
    int32_t  staked;        /* the sum of every bet on the table */
} rl_round_t;

void rl_round_clear(rl_round_t *r, int wheel);

/* Adds to an existing identical bet rather than making a second
 * entry, so repeatedly clicking one square does not exhaust
 * RL_MAX_BETS. Returns false if the bet is invalid, the amount is not
 * positive, or the table is full. */
bool rl_bet_place(rl_round_t *r, int type, int sel, int32_t amount);

/* Removes up to `amount` from a matching bet, dropping it entirely
 * when nothing is left. Returns what was actually taken off. */
int32_t rl_bet_remove(rl_round_t *r, int type, int sel, int32_t amount);

/* What is currently staked on exactly this bet. */
int32_t rl_bet_on(const rl_round_t *r, int type, int sel);

/* Total returned across every bet if `pocket` comes up, INCLUDING
 * returned stakes. The round's profit is this minus r->staked. */
int32_t rl_round_returns(const rl_round_t *r, int pocket);

#endif
