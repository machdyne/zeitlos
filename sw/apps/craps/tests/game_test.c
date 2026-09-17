/*
 * Zeitlos craps -- host tests for the round.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * tests/edge_test.c proves the bets are priced correctly. This proves
 * the TABLE plays them correctly, which is a different question: a
 * perfectly priced pass line bet resolved against the wrong point pays
 * the wrong person.
 *
 * The last test closes the loop -- it plays hundreds of thousands of
 * rolls through the state machine and checks the money comes out where
 * the exact arithmetic said it would.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../cr_game.h"
#include "../../../common/games/zrand.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (ok) return;
    failures++;
    printf("FAIL: %s\n", what);
}

static void check_eq(long got, long want, const char *what)
{
    checks++;
    if (got == want) return;
    failures++;
    printf("FAIL: %s -- got %ld, want %ld\n", what, got, want);
}

static cr_game_t g;

/* A roll by its total, split into a pair that is not a hard way unless
 * asked -- so a test about place bets is not accidentally a test about
 * hardways. */
static int32_t roll(int total)
{
    int d1 = total <= 7 ? 1 : total - 6;
    int d2 = total - d1;
    if (d1 == d2 && d1 < 6) { d1++; d2--; }
    return cr_roll(&g, d1, d2);
}

static void test_come_out(void)
{
    cr_game_init(&g);
    check(cr_place(&g, CR_PASS, 0, 10), "a pass line bet is taken on a come-out");
    check_eq(roll(7), 20, "and a seven pays it even money");
    check_eq(g.point, 0, "leaving the table on a come-out");
    check_eq(g.nbets, 0, "with the bet paid and down");

    cr_game_init(&g);
    cr_place(&g, CR_PASS, 0, 10);
    check_eq(roll(11), 20, "eleven pays it too");

    cr_game_init(&g);
    cr_place(&g, CR_PASS, 0, 10);
    check_eq(roll(2), 0, "craps takes it");
    check_eq(g.nbets, 0, "and it is gone");

    cr_game_init(&g);
    cr_place(&g, CR_PASS, 0, 10);
    roll(4);
    check_eq(g.point, 4, "any other number becomes the point");
    check_eq(g.nbets, 1, "and the bet rides");
    check(!cr_can_place(&g, CR_PASS, 0, 10),
        "no new pass line bet once a point is on");
    check(cr_can_place(&g, CR_COME, 0, 10), "but a come bet is fine");

    check_eq(roll(4), 20, "making the point pays");
    check_eq(g.point, 0, "and returns the table to a come-out");
    check(g.point_made, "which is reported as the point being made");

    cr_game_init(&g);
    cr_place(&g, CR_PASS, 0, 10);
    roll(6);
    check_eq(roll(7), 0, "a seven-out takes it");
    check(g.seven_out, "and says so");
    check_eq(g.point, 0, "and clears the point");
}

static void test_dont_pass(void)
{
    cr_game_init(&g);
    cr_place(&g, CR_DONT_PASS, 0, 10);
    check_eq(roll(3), 20, "don't pass wins on three");

    cr_game_init(&g);
    cr_place(&g, CR_DONT_PASS, 0, 10);
    /* THE BAR. Twelve returns the stake -- and that single exception is
     * the entire house edge on this bet. */
    check_eq(roll(12), 10, "twelve is a push, not a win");
    check_eq(g.nbets, 0, "and the bet comes back");

    cr_game_init(&g);
    cr_place(&g, CR_DONT_PASS, 0, 10);
    roll(5);
    check_eq(roll(7), 20, "once a point is on, a seven wins it");
}

static void test_odds(void)
{
    cr_game_init(&g);
    cr_place(&g, CR_PASS, 0, 10);

    check_eq(cr_odds_room(&g, CR_PASS_ODDS, 4), 0,
        "there is no room for odds without a point");
    check(!cr_can_place(&g, CR_PASS_ODDS, 4, 10), "so they cannot be laid");

    roll(4);
    /* 3-4-5x: three times the flat bet on a four. */
    check_eq(cr_odds_room(&g, CR_PASS_ODDS, 4), 30, "a point of four allows 3x");
    check(!cr_can_place(&g, CR_PASS_ODDS, 4, 31), "and not a chip more");
    check(cr_place(&g, CR_PASS_ODDS, 4, 30), "the full amount is fine");
    check_eq(cr_odds_room(&g, CR_PASS_ODDS, 4), 0, "and then there is no room");

    /* True odds: 2:1 on a four. Flat 10 pays 10, odds 30 pays 60. */
    check_eq(roll(4), 20 + 90, "making the point pays flat and odds");

    /* Odds on the wrong number are not a bet. */
    cr_game_init(&g);
    cr_place(&g, CR_PASS, 0, 10);
    roll(6);
    check(!cr_can_place(&g, CR_PASS_ODDS, 4, 10),
        "odds must be on the point, not on some other number");
    check_eq(cr_odds_room(&g, CR_PASS_ODDS, 6), 50, "a six allows 5x");
    cr_place(&g, CR_PASS_ODDS, 6, 50);
    /* 6:5 on a six: 50 pays 60. */
    check_eq(roll(6), 20 + 110, "and pays six to five");
}

static void test_come(void)
{
    cr_game_init(&g);
    cr_place(&g, CR_PASS, 0, 10);
    roll(8);
    check_eq(g.point, 8, "the point is eight");

    check(cr_place(&g, CR_COME, 0, 10), "a come bet goes up with no number");
    roll(5);
    check_eq(cr_bet_on(&g, CR_COME, 5), 10, "and takes the next roll's number");
    check_eq(cr_bet_on(&g, CR_COME, 0), 0, "leaving nothing in the come box");
    check_eq(g.point, 8, "without disturbing the table's point");

    check_eq(cr_odds_room(&g, CR_COME_ODDS, 5), 40,
        "its own odds may go behind it");
    cr_place(&g, CR_COME_ODDS, 5, 40);

    /* 3:2 on a five: 40 pays 60. Plus the flat 10 paying 10. */
    check_eq(roll(5), 20 + 100, "and the number pays both");
    check_eq(g.point, 8, "with the table's point still eight");

    /* A come bet on its first roll wins on a seven -- which is the same
     * roll that would take the pass line. Both happen. */
    cr_game_init(&g);
    cr_place(&g, CR_PASS, 0, 10);
    roll(9);
    cr_place(&g, CR_COME, 0, 10);
    check_eq(roll(7), 20, "a seven pays a fresh come bet and takes the line");
    check_eq(g.point, 0, "and the round is over");
}

static void test_place_and_hard(void)
{
    cr_game_init(&g);
    cr_place(&g, CR_PASS, 0, 10);
    roll(10);

    check(cr_place(&g, CR_PLACE, 6, 12), "a place bet goes up");
    /* 7:6 on a six: 12 pays 14. PAID AND LEFT UP, so only the winnings
     * come back -- the stake is still at risk. */
    check_eq(roll(6), 14, "and pays without coming down");
    check_eq(cr_bet_on(&g, CR_PLACE, 6), 12, "the bet is still working");
    check_eq(roll(6), 14, "and pays again");

    check_eq(roll(7), 0, "a seven takes it");
    check_eq(cr_bet_on(&g, CR_PLACE, 6), 0, "and it is gone");

    /* Hardways need the dice. */
    cr_game_init(&g);
    cr_place(&g, CR_PASS, 0, 10);
    roll(5);
    cr_place(&g, CR_HARD, 6, 10);
    cr_place(&g, CR_PLACE, 6, 12);

    /* A six the easy way: place six wins, hard six loses, from ONE
     * roll. This is the case that needs the pair and not the total. */
    {
        int32_t back = cr_roll(&g, 4, 2);
        check_eq(back, 14, "4-2 pays place six");
        check_eq(cr_bet_on(&g, CR_HARD, 6), 0, "and takes hard six");
    }

    cr_place(&g, CR_HARD, 6, 10);
    {
        /* And the hard way pays 9:1 and stays up. */
        int32_t back = cr_roll(&g, 3, 3);
        check_eq(back, 14 + 90, "3-3 pays both");
        check_eq(cr_bet_on(&g, CR_HARD, 6), 10, "with the hardway still up");
    }
}

static void test_take_down(void)
{
    cr_game_init(&g);
    cr_place(&g, CR_PASS, 0, 10);
    roll(4);

    /* THE LINE BETS ARE CONTRACT. A pass line bet has already had its
     * come-out, where most of its winning chances are, so pulling it
     * would be taking the good half and leaving the bad. */
    check(!cr_can_take_down(CR_PASS), "a pass line bet cannot be pulled");
    check_eq(cr_take_down(&g, CR_PASS, 0), 0, "and asking does nothing");
    check_eq(cr_bet_on(&g, CR_PASS, 0), 10, "it is still there");

    cr_place(&g, CR_PLACE, 8, 12);
    check(cr_can_take_down(CR_PLACE), "a place bet may be pulled");
    check_eq(cr_take_down(&g, CR_PLACE, 8), 12, "and comes back in full");

    cr_game_init(&g);
    cr_place(&g, CR_DONT_PASS, 0, 10);
    roll(4);
    check(cr_can_take_down(CR_DONT_PASS),
        "a don't pass may be pulled -- it is the favourite now");
}

static void test_limits(void)
{
    int i;

    cr_game_init(&g);
    cr_place(&g, CR_PASS, 0, 10);
    roll(6);

    /* Clicking one spot repeatedly stacks rather than filling the
     * table. */
    for (i = 0; i < 20; i++) cr_place(&g, CR_PLACE, 8, 6);
    check_eq(cr_bet_on(&g, CR_PLACE, 8), 120, "identical bets stack");
    check(g.nbets < CR_MAX_BETS, "without exhausting the table");

    /* And filling it really does stop. */
    cr_game_init(&g);
    cr_place(&g, CR_PASS, 0, 10);
    roll(6);
    for (i = 0; i < CR_MAX_BETS + 8; i++) {
        static const int t[6] = { CR_FIELD, CR_ANY7, CR_ANY_CRAPS,
            CR_ELEVEN, CR_TWO, CR_TWELVE };
        cr_place(&g, t[i % 6], 0, 1);
        cr_place(&g, CR_PLACE, 4 + (i % 6 == 3 ? 1 : i % 6), 1);
        cr_place(&g, CR_HARD, 4 + 2 * (i % 4), 1);
    }
    check(g.nbets <= CR_MAX_BETS, "the table cannot be overfilled");

    check(!cr_place(&g, CR_PLACE, 7, 10), "seven cannot be placed");
    check(!cr_place(&g, CR_PASS, 0, 0), "nor can nothing be bet");
    check(!cr_place(&g, CR_PASS, 0, -5), "nor a negative amount");
}

/* zg_rng_below(6), not a hand-rolled modulo.
 *
 * The first version here was a bare xorshift32 with `% 6`, and it made
 * this test report a house edge of 0.35%, 1.06% or 0.55% from identical
 * code -- decided only by how many draws had been taken beforehand.
 * xorshift32's low bits are weak and a small modulo reads exactly
 * those. See sw/common/games/zrand.c, whose fallback generator grew a
 * finaliser because of this.
 */
static int die(void)
{
    return (int)zg_rng_below(6) + 1;
}

/* -- the loop closer --------------------------------------------------------
 *
 * tests/edge_test.c derives the pass line's expectation from the dice
 * and the payout table: -7/495, or 1.414%. This plays it through the
 * STATE MACHINE and checks the money comes out in the same place.
 *
 * -- and why it is a PAIRED comparison --
 *
 * A hundred thousand rounds of pass-and-full-odds has a standard error
 * of about 0.3% on the realised edge, because the odds bets swing hard.
 * A tight band round the true 0.374% would fail at random, and a band
 * wide enough not to would catch nothing.
 *
 * So the same dice are played TWICE -- once flat, once with full odds
 * behind. Placing odds consumes no randomness, and odds resolve on the
 * same roll the flat bet does, so both runs see an identical sequence
 * and the flat bet's profit and loss is identical between them. What
 * differs is only the odds' own P&L, whose mean is exactly zero.
 *
 * That makes the interesting claim -- that free odds dilute the edge
 * without the house taking any more -- checkable rather than folklore.
 */
static void play(long rounds, bool with_odds, int64_t *staked, int64_t *back)
{
    long n;

    *staked = 0;
    *back = 0;

    for (n = 0; n < rounds; n++) {
        int guard = 0;

        cr_game_init(&g);
        cr_place(&g, CR_PASS, 0, 100);
        *staked += 100;

        while (g.nbets > 0 && guard++ < 400) {
            if (with_odds && g.point) {
                int32_t r = cr_odds_room(&g, CR_PASS_ODDS, g.point);
                if (r > 0) {
                    cr_place(&g, CR_PASS_ODDS, g.point, r);
                    *staked += r;
                }
            }
            *back += cr_roll(&g, die(), die());
        }
    }
}

static void test_realised_edge(void)
{
    int64_t fs, fb, os, ob;
    const long N = 120000;

    zg_rng_seed(20260916);
    play(N, false, &fs, &fb);

    zg_rng_seed(20260916);
    play(N, true, &os, &ob);

    /* The flat bet alone, against the 1.414% the exact arithmetic
     * derives. Wide, because even flat-only has real variance at this
     * many rounds -- but a machine that resolves against the wrong
     * point, or pays a place bet on a come-out, misses by far more. */
    {
        long e = (long)(((fs - fb) * 1000) / fs);
        check(e >= 8 && e <= 21,
            "the pass line realises about a 1.4% edge through the table");
        if (e < 8 || e > 21)
            printf("      realised %ld.%ld%%\n", e / 10, e % 10);
    }

    /* THE SAME DICE, so the flat bet's loss is identical and the whole
     * difference is the odds. */
    {
        int64_t flat_loss = fs - fb;
        int64_t total_loss = os - ob;
        int64_t odds_staked = os - fs;
        int64_t odds_pl = total_loss - flat_loss;

        check(odds_staked > N * 200,
            "full odds put several times the flat bet at risk");

        /* Fair means the odds break even in the long run. Checked as a
         * fraction of the odds action rather than as an absolute, and
         * generously -- this is the one number here that is genuinely
         * statistical. */
        {
            int64_t mag = odds_pl < 0 ? -odds_pl : odds_pl;
            check(mag * 100 < odds_staked,
                "and lose nothing to the house -- they are a fair bet");
        }

        /* Which means the edge, as a fraction of everything risked,
         * falls. That is the one genuinely good piece of advice in a
         * casino and it is demonstrable here rather than folklore. */
        check(total_loss * fs < flat_loss * os,
            "so backing the line with odds dilutes the edge");
    }
}

int main(void)
{
    printf("craps: round tests\n");

    test_come_out();
    test_dont_pass();
    test_odds();
    test_come();
    test_place_and_hard();
    test_take_down();
    test_limits();
    test_realised_edge();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
