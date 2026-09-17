#ifndef SL_REELS_H
#define SL_REELS_H

/*
 * Zeitlos slots -- the reels and what they pay.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * No I/O. This file and sl_reels.c know nothing about the framebuffer,
 * which is what lets every outcome the machine can produce be
 * enumerated on the build machine -- all 32,768 of them.
 *
 * -- the house edge is a number you can count, not estimate --
 *
 * Three reels of 32 stops is 32^3 = 32,768 positions, and five paylines
 * over each of those is 163,840 line-bets. That is small enough to walk
 * exhaustively, so the return to player is not a simulation result or a
 * designer's intention: it is an exact integer.
 *
 * Staking one coin on every line of every possible position costs
 * 163,840 and returns exactly 155,060 -- an RTP of 94.641% and a house
 * edge of 5.359%, which is where a real machine sits.
 *
 * tests/reel_test.c asserts that integer. Any change to a reel strip or
 * a payout moves it, which is the point: the edge cannot drift without
 * a test failing, and nobody has to notice by playing.
 *
 * -- 32 stops, which is not an accident --
 *
 * An electromechanical machine had 22 physical stops. A virtual-reel
 * machine maps a larger table onto them, and 32 is the usual size.
 *
 * It is also exactly the bound that used to hang this tree:
 * z_rng_below(32) never returned, because the accept bound 2^32 - (2^32
 * % 32) is 2^32 and truncates to zero. sw/apps/poker found it on a card
 * shuffle. A slot machine asks for it three times a spin.
 */

#include <stdint.h>
#include <stdbool.h>

#define SL_BLANK   0
#define SL_CHERRY  1
#define SL_BAR     2
#define SL_BAR2    3      /* BARBAR */
#define SL_BAR3    4      /* 3BAR   */
#define SL_BELL    5
#define SL_SEVEN   6
#define SL_NSYMS   7

#define SL_STOPS   32     /* virtual stops per reel */
#define SL_REELS   3
#define SL_ROWS    3      /* symbols visible per reel */
#define SL_LINES   5      /* three rows and two diagonals */

/* The strips. Written out rather than generated: the distribution IS
 * the machine's design, and the edge falls out of it. */
extern const uint8_t sl_strip[SL_REELS][SL_STOPS];

/* The three symbols showing on `reel` when it stops at `stop`, top to
 * bottom. `stop` is taken modulo SL_STOPS, so a caller need not. */
void sl_window(int reel, int stop, uint8_t *out);

/* The symbols on payline `line`, given where the three reels stopped. */
void sl_line(const int *stops, int line, uint8_t *out);

/* What one coin on a line pays for those three symbols, stake NOT
 * included -- a losing line pays 0 and the coin is gone.
 *
 * CHERRIES PAY FROM THE LEFT, which is the classic rule and not a
 * detail: a cherry anywhere on the line was the first thing tried here
 * and it put the return to player at 133%. Left-to-right means one
 * cherry counts only on reel 1, two only on reels 1 and 2. */
int sl_pay(int a, int b, int c);

/* Everything the given stops pay, for `per_line` coins on each of the
 * first `lines` paylines. If `line_pays` is non-NULL it receives the
 * per-line amounts, which is what the display highlights. */
int32_t sl_evaluate(const int *stops, int32_t per_line, int lines,
    int32_t *line_pays);

/* Which rows of which reels payline `line` passes through -- for
 * drawing it. `rows[r]` is the row index on reel r. */
void sl_line_rows(int line, int *rows);

const char *sl_sym_name(int sym);

/* The paytable, for putting on screen. Returns the payout for three of
 * `sym` on a line, or 0 if three of it pays nothing. */
int sl_pay_three(int sym);

#endif
