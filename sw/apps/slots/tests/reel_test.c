/*
 * Zeitlos slots -- host tests for the reels and the paytable.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- the one that does the work --
 *
 * Three reels of 32 stops is 32,768 positions, and five paylines over
 * each is 163,840 line-bets. That is small enough to walk exhaustively,
 * so the return to player is not a simulation result or an intention --
 * it is an exact integer, and this file asserts it.
 *
 * Staking one coin on every line of every possible position costs
 * 163,840 and must return exactly 155,060: an RTP of 94.641% and a
 * house edge of 5.359%.
 *
 * Any change to a strip or a payout moves that number. That is the
 * point. A slot machine's edge is the whole of its design, and it is
 * the one property that cannot be checked by playing -- at this edge it
 * would take tens of thousands of spins to distinguish 94% from 88%,
 * and a player who noticed would be the one person who had lost enough
 * to care.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../sl_reels.h"

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

/* -- the edge ----------------------------------------------------------- */

#define ALL_POSITIONS (SL_STOPS * SL_STOPS * SL_STOPS)
#define ALL_LINEBETS  (ALL_POSITIONS * SL_LINES)

static void test_return(void)
{
    long total = 0, hits = 0;
    int a, b, c;

    for (a = 0; a < SL_STOPS; a++)
        for (b = 0; b < SL_STOPS; b++)
            for (c = 0; c < SL_STOPS; c++) {
                int stops[SL_REELS];
                int32_t lp[SL_LINES];
                int i;

                stops[0] = a; stops[1] = b; stops[2] = c;
                total += sl_evaluate(stops, 1, SL_LINES, lp);
                for (i = 0; i < SL_LINES; i++) if (lp[i]) hits++;
            }

    check_eq(total, 155060,
        "the machine returns exactly 155,060 on 163,840 coins");
    check_eq(ALL_LINEBETS, 163840, "which is every line-bet there is");

    /* Stated as the edge too, because that is the number a person would
     * look up, and because a wrong total could still be a plausible
     * one. */
    {
        long edge_x1000 = (ALL_LINEBETS - total) * 1000 / ALL_LINEBETS;
        check_eq(edge_x1000, 53, "a house edge of 5.3%");
    }

    /* Roughly one line-bet in five pays something. A machine that pays
     * far less often than that feels broken however good its RTP, and
     * one that pays far more is giving the money back in dribbles. */
    check(hits * 100 / ALL_LINEBETS >= 15, "at least 15% of lines pay");
    check(hits * 100 / ALL_LINEBETS <= 25, "and at most 25%");

    /* Scaling. Doubling the coins doubles the return exactly -- no
     * rounding anywhere, which is what lets the edge above be an
     * integer at any stake. */
    {
        long twice = 0;
        for (a = 0; a < SL_STOPS; a += 3)
            for (b = 0; b < SL_STOPS; b += 5)
                for (c = 0; c < SL_STOPS; c += 7) {
                    int stops[3];
                    stops[0] = a; stops[1] = b; stops[2] = c;
                    twice += sl_evaluate(stops, 2, SL_LINES, 0) -
                        2 * sl_evaluate(stops, 1, SL_LINES, 0);
                }
        check_eq(twice, 0, "two coins a line pays exactly twice one coin");
    }
}

/* -- the jackpot --------------------------------------------------------- */

static void test_jackpot(void)
{
    long sevens = 0;
    int a, b, c, i;

    for (a = 0; a < SL_STOPS; a++)
        for (b = 0; b < SL_STOPS; b++)
            for (c = 0; c < SL_STOPS; c++) {
                int stops[3];
                stops[0] = a; stops[1] = b; stops[2] = c;
                for (i = 0; i < SL_LINES; i++) {
                    uint8_t s[SL_REELS];
                    sl_line(stops, i, s);
                    if (s[0] == SL_SEVEN && s[1] == SL_SEVEN &&
                        s[2] == SL_SEVEN) sevens++;
                }
            }

    check_eq(sevens, 40, "three sevens land on 40 of the 163,840 line-bets");
    check_eq(ALL_LINEBETS / sevens, 4096, "which is once in 4,096");

    /* The jackpot must be most of the return, or it is not a jackpot. */
    check(sevens * 800 * 100 / 155060 >= 15,
        "and is at least 15% of everything the machine gives back");
}

/* -- the paytable -------------------------------------------------------- */

static void test_pay(void)
{
    check_eq(sl_pay(SL_SEVEN, SL_SEVEN, SL_SEVEN), 800, "three sevens");
    check_eq(sl_pay(SL_BELL, SL_BELL, SL_BELL), 150, "three bells");
    check_eq(sl_pay(SL_BAR, SL_BAR, SL_BAR), 20, "three single bars");
    check_eq(sl_pay(SL_BLANK, SL_BLANK, SL_BLANK), 0, "three blanks pay nothing");

    /* Mixed bars, in every order. */
    check_eq(sl_pay(SL_BAR, SL_BAR2, SL_BAR3), 2, "any three bars pay 2");
    check_eq(sl_pay(SL_BAR3, SL_BAR, SL_BAR2), 2, "in any order");
    check_eq(sl_pay(SL_BAR, SL_BAR, SL_BAR2), 2, "two of a kind and one other");
    check(sl_pay(SL_BAR, SL_BAR, SL_BAR) > sl_pay(SL_BAR, SL_BAR, SL_BAR2),
        "three matching bars beat three mixed ones");

    /* CHERRIES PAY FROM THE LEFT. Paying them anywhere put the return
     * at 133% -- they are the most common paying symbol, so position
     * matters more for them than for anything else on the reel. */
    check_eq(sl_pay(SL_CHERRY, SL_BLANK, SL_BLANK), 1, "one cherry, on reel 1");
    check_eq(sl_pay(SL_CHERRY, SL_CHERRY, SL_BLANK), 5, "two, on reels 1 and 2");
    check_eq(sl_pay(SL_CHERRY, SL_CHERRY, SL_CHERRY), 100, "and three");
    check_eq(sl_pay(SL_BLANK, SL_CHERRY, SL_CHERRY), 0,
        "but cherries on reels 2 and 3 pay nothing");
    check_eq(sl_pay(SL_BLANK, SL_BLANK, SL_CHERRY), 0, "nor one on reel 3");

    /* Every symbol that pays three-of-a-kind has a name and a price,
     * and the blank has neither. */
    {
        int s, bad = 0;
        for (s = 0; s < SL_NSYMS; s++) {
            const char *n = sl_sym_name(s);
            if (!n || !n[0]) bad++;
            if (s != SL_BLANK && sl_pay_three(s) <= 0) bad++;
        }
        check_eq(bad, 0, "every symbol has a name, and every paying one a price");
        check_eq(sl_pay_three(SL_BLANK), 0, "the blank pays nothing");
    }

    /* Rarer beats commoner, all the way down. A paytable that is not
     * monotonic in rarity is a paytable somebody edited without
     * recounting. */
    check(sl_pay_three(SL_SEVEN) > sl_pay_three(SL_BELL), "sevens beat bells");
    check(sl_pay_three(SL_BELL) >= sl_pay_three(SL_BAR3), "bells beat 3BAR");
    check(sl_pay_three(SL_BAR3) > sl_pay_three(SL_BAR2), "3BAR beats BARBAR");
    check(sl_pay_three(SL_BAR2) > sl_pay_three(SL_BAR), "BARBAR beats BAR");
}

/* -- the strips and the window -------------------------------------------- */

static void test_strips(void)
{
    int r, i, bad = 0;
    int count[SL_REELS][SL_NSYMS];

    for (r = 0; r < SL_REELS; r++) {
        for (i = 0; i < SL_NSYMS; i++) count[r][i] = 0;
        for (i = 0; i < SL_STOPS; i++) {
            uint8_t s = sl_strip[r][i];
            if (s >= SL_NSYMS) { bad++; continue; }
            count[r][s]++;
        }
    }
    check_eq(bad, 0, "every stop holds a real symbol");

    /* Each reel carries every paying symbol -- a reel missing one makes
     * that symbol's three-of-a-kind unreachable, and the exhaustive
     * total would still look plausible. */
    bad = 0;
    for (r = 0; r < SL_REELS; r++)
        for (i = 1; i < SL_NSYMS; i++)
            if (count[r][i] == 0) bad++;
    check_eq(bad, 0, "every reel carries every paying symbol");

    /* Cherries thin out left to right, which is what makes paying them
     * from the left worth anything. */
    check(count[0][SL_CHERRY] > count[2][SL_CHERRY],
        "reel 1 has more cherries than reel 3");

    /* The window wraps. Stop 31 shows stops 31, 0, 1. */
    {
        uint8_t w[SL_ROWS];
        sl_window(0, SL_STOPS - 1, w);
        check_eq(w[0], sl_strip[0][SL_STOPS - 1], "the window starts at its stop");
        check_eq(w[1], sl_strip[0][0], "and wraps round the strip");
        check_eq(w[2], sl_strip[0][1], "twice over");

        sl_window(0, SL_STOPS + 3, w);
        check_eq(w[0], sl_strip[0][3], "an out-of-range stop is taken modulo");
        sl_window(0, -1, w);
        check_eq(w[0], sl_strip[0][SL_STOPS - 1], "including a negative one");
    }

    /* Every payline touches every reel exactly once. */
    bad = 0;
    for (i = 0; i < SL_LINES; i++) {
        int rows[SL_REELS], k;
        sl_line_rows(i, rows);
        for (k = 0; k < SL_REELS; k++)
            if (rows[k] < 0 || rows[k] >= SL_ROWS) bad++;
    }
    check_eq(bad, 0, "every payline stays inside the window");

    /* And no two paylines are the same line. */
    bad = 0;
    for (i = 0; i < SL_LINES; i++) {
        int j;
        int ri[SL_REELS];
        sl_line_rows(i, ri);
        for (j = i + 1; j < SL_LINES; j++) {
            int rj[SL_REELS], k, same = 1;
            sl_line_rows(j, rj);
            for (k = 0; k < SL_REELS; k++) if (ri[k] != rj[k]) same = 0;
            if (same) bad++;
        }
    }
    check_eq(bad, 0, "no two paylines are the same line");
}

static void test_lines_played(void)
{
    int stops[SL_REELS];
    int32_t lp[SL_LINES];
    int a, b, c, bad = 0;

    /* Paying for fewer lines can only ever return less. Somebody who
     * plays one line must not be able to win on line four. */
    for (a = 0; a < SL_STOPS; a += 3)
        for (b = 0; b < SL_STOPS; b += 5)
            for (c = 0; c < SL_STOPS; c += 7) {
                int n;
                int32_t prev = -1;
                stops[0] = a; stops[1] = b; stops[2] = c;
                for (n = 0; n <= SL_LINES; n++) {
                    int32_t t = sl_evaluate(stops, 1, n, lp);
                    if (t < prev) bad++;
                    prev = t;
                    {
                        int i;
                        for (i = n; i < SL_LINES; i++)
                            if (lp[i] != 0) bad++;
                    }
                }
            }
    check_eq(bad, 0, "unpaid lines never pay, and more lines never pay less");

    check_eq(sl_evaluate(stops, 1, 0, 0), 0, "playing no lines wins nothing");
    check_eq(sl_evaluate(stops, 0, SL_LINES, 0), 0, "nor does a zero stake");
}

int main(void)
{
    printf("slots: reel tests\n");

    test_pay();
    test_strips();
    test_lines_played();
    test_jackpot();
    test_return();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
