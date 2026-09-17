/*
 * Zeitlos craps -- host tests for the bets and their edges.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- the one that does the work --
 *
 * The house edge of every bet is a consequence of two dice and the
 * payout table, so this WALKS THE CHAIN and computes it, in exact
 * integer arithmetic, rather than comparing against a constant. A pass
 * line bet works out to 244 wins in 495; a place 6 to an expectation of
 * exactly -1/66.
 *
 * Exact, not approximate. Every one of these is a rational with a small
 * denominator, so there is no reason to accept a tolerance -- and a
 * tolerance is exactly what would let the free odds bet pass while
 * being merely nearly fair.
 *
 * THE ODDS BET IS THE POINT OF CRAPS. It pays true odds, so its
 * expectation is the integer zero: the only bet in a casino with no
 * edge at all. Asserted as a numerator of 0.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../cr_table.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (ok) return;
    failures++;
    printf("FAIL: %s\n", what);
}

/* -- exact rationals -------------------------------------------------------
 *
 * int64 numerator over int64 denominator, reduced after every step. The
 * denominators here never get near overflow -- 495 and 1980 are the
 * biggest that turn up -- but reducing keeps it that way rather than
 * hoping.
 */
typedef struct { int64_t n, d; } frac;

static int64_t gcd64(int64_t a, int64_t b)
{
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int64_t t = a % b; a = b; b = t; }
    return a ? a : 1;
}

static frac mk(int64_t n, int64_t d)
{
    frac f;
    int64_t g;
    if (d < 0) { n = -n; d = -d; }
    g = gcd64(n, d);
    f.n = n / g;
    f.d = d / g;
    return f;
}

static frac add(frac a, frac b) { return mk(a.n * b.d + b.n * a.d, a.d * b.d); }
static frac mul(frac a, frac b) { return mk(a.n * b.n, a.d * b.d); }

static const int ways[13] = { 0,0,1,2,3,4,5,6,5,4,3,2,1 };

static void check_ev(frac ev, int64_t n, int64_t d, const char *what)
{
    frac want = mk(n, d);
    checks++;
    if (ev.n == want.n && ev.d == want.d) return;
    failures++;
    printf("FAIL: %s -- got %lld/%lld, want %lld/%lld\n", what,
        (long long)ev.n, (long long)ev.d,
        (long long)want.n, (long long)want.d);
}

/* -- one-roll bets ---------------------------------------------------------
 *
 * 36 outcomes, resolved immediately. The whole expectation is one sum.
 */
static frac ev_one_roll(int type, int point)
{
    frac ev = mk(0, 1);
    int d1, d2;

    for (d1 = 1; d1 <= 6; d1++)
        for (d2 = 1; d2 <= 6; d2++) {
            int roll = d1 + d2;
            int r = cr_resolve(type, 0, roll, point);
            int num, den;

            cr_payout(type, 0, roll, &num, &den);

            if (r == CR_WIN) ev = add(ev, mul(mk(1, 36), mk(num, den)));
            else if (r == CR_LOSE) ev = add(ev, mk(-1, 36));
        }

    return ev;
}

/* -- bets that resolve over many rolls -------------------------------------
 *
 * A bet that stands is re-rolled, so its expectation is a geometric
 * series. Rather than sum one, condition on the resolving rolls: once
 * a bet can only win or lose, the chance of winning is
 * (ways to win) / (ways to win + ways to lose) -- the standing rolls
 * cancel out entirely.
 *
 * That is why these come out as small exact fractions instead of
 * repeating decimals, and it is why the test does not need a
 * convergence limit or a tolerance.
 */
static frac ev_resolving(int type, int sel, int point)
{
    int64_t w = 0, l = 0, p = 0;
    int d1, d2, num, den;

    for (d1 = 1; d1 <= 6; d1++)
        for (d2 = 1; d2 <= 6; d2++) {
            int r = cr_resolve(type, sel, d1 + d2, point);
            if (r == CR_WIN) w++;
            else if (r == CR_LOSE) l++;
            else if (r == CR_PUSH) p++;
        }

    (void)p;
    if (w + l == 0) return mk(0, 1);

    cr_payout(type, sel, sel, &num, &den);

    return add(mul(mk(w, w + l), mk(num, den)), mk(-l, w + l));
}

/* The pass line, which needs the come-out AND the point phase. */
static frac ev_pass(int type)
{
    frac ev = mk(0, 1);
    int roll;

    for (roll = 2; roll <= 12; roll++) {
        frac pr = mk(ways[roll], 36);
        int r = cr_resolve(type, 0, roll, 0);

        if (r == CR_WIN) ev = add(ev, pr);
        else if (r == CR_LOSE) ev = add(ev, mul(pr, mk(-1, 1)));
        else if (r == CR_PUSH) { /* returned: contributes nothing */ }
        else {
            /* A point is set. From here the bet resolves on that number
             * or a seven, and every other roll cancels. */
            int64_t wp = ways[roll], lp = ways[7];
            frac cond;
            if (type == CR_DONT_PASS) { int64_t t = wp; wp = lp; lp = t; }
            cond = add(mk(wp, wp + lp), mk(-lp, wp + lp));
            ev = add(ev, mul(pr, cond));
        }
    }

    return ev;
}

static void test_line(void)
{
    frac ev = ev_pass(CR_PASS);

    /* 244 wins in 495, which is the number every craps book quotes --
     * and here it falls out of the dice and the rules rather than
     * being written down. */
    check_ev(ev, -7, 495, "the pass line has an expectation of -7/495");
    /* Stated the other way too, because 244 in 495 is the form people
     * recognise. An even-money bet with expectation e wins a fraction
     * (1 + e) / 2 of the time. */
    {
        frac w = mul(add(mk(1, 1), ev), mk(1, 2));
        check(w.n == 244 && w.d == 495,
            "which is 244 wins in 495");
    }

    ev = ev_pass(CR_DONT_PASS);
    check_ev(ev, -3, 220, "don't pass is -3/220, slightly better");
    {
        /* Cross-multiplied, because comparing two fractions by eye is
         * how the first version of this line came to be nonsense.
         * a/b > c/d, with b and d positive, is a*d > c*b. */
        frac pass = ev_pass(CR_PASS);
        check(ev.n * pass.d > pass.n * ev.d,
            "and is the better of the two bets");
    }
    {
        /* The bar on twelve IS the edge. Remove it and the bet wins. */
        check(cr_resolve(CR_DONT_PASS, 0, 12, 0) == CR_PUSH,
            "twelve is barred on the come-out");
        check(cr_resolve(CR_DONT_PASS, 0, 2, 0) == CR_WIN, "but two wins");
        check(cr_resolve(CR_DONT_PASS, 0, 3, 0) == CR_WIN, "and three");
    }
}

static void test_odds_are_free(void)
{
    static const int pts[6] = { 4, 5, 6, 8, 9, 10 };
    int i, bad = 0;

    /* THE HEADLINE. True odds means the expectation is the integer
     * zero -- not small, not within a tolerance. A tolerance here would
     * pass a bet that was merely nearly fair, which is the one thing
     * this bet must not be. */
    for (i = 0; i < 6; i++) {
        frac ev = ev_resolving(CR_PASS_ODDS, pts[i], pts[i]);
        if (ev.n != 0) {
            printf("      odds on %d: %lld/%lld\n", pts[i],
                (long long)ev.n, (long long)ev.d);
            bad++;
        }
    }
    check(bad == 0, "the odds behind the pass line are exactly fair");

    bad = 0;
    for (i = 0; i < 6; i++) {
        frac ev = ev_resolving(CR_DONT_ODDS, pts[i], pts[i]);
        if (ev.n != 0) bad++;
    }
    check(bad == 0, "and so is laying them");

    /* The 3-4-5x cap exists so the win is always six times the flat
     * bet, whichever point is on. */
    bad = 0;
    for (i = 0; i < 6; i++) {
        int32_t flat = 10;
        int32_t odds = cr_max_odds(pts[i], flat);
        int num, den;
        cr_payout(CR_PASS_ODDS, pts[i], pts[i], &num, &den);
        if (odds * num / den != flat * 6) bad++;
    }
    check(bad == 0, "max odds always win six times the flat bet");
    check(cr_max_odds(0, 10) == 0, "and there are no odds without a point");
}

static void test_place_and_props(void)
{
    check_ev(ev_resolving(CR_PLACE, 6, 6), -1, 66, "place 6 is -1/66");
    check_ev(ev_resolving(CR_PLACE, 8, 8), -1, 66, "place 8 the same");
    check_ev(ev_resolving(CR_PLACE, 5, 5), -1, 25, "place 5 is -1/25");
    check_ev(ev_resolving(CR_PLACE, 9, 9), -1, 25, "place 9 the same");
    check_ev(ev_resolving(CR_PLACE, 4, 4), -1, 15, "place 4 is -1/15");
    check_ev(ev_resolving(CR_PLACE, 10, 10), -1, 15, "place 10 the same");

    /* The place bets are the odds bets with the fair price shaved. That
     * is visible in the payouts and it is the whole difference between
     * a 1.5% bet and a 0% one. */
    {
        int pn, pd, on, od;
        cr_payout(CR_PLACE, 6, 6, &pn, &pd);
        cr_payout(CR_PASS_ODDS, 6, 6, &on, &od);
        check(pn * od < on * pd, "place 6 pays less than fair odds on 6");
    }

    check_ev(ev_one_roll(CR_FIELD, 0), -1, 36, "the field is -1/36");
    check_ev(ev_one_roll(CR_ANY7, 0), -1, 6, "any seven is -1/6, the worst");
    check_ev(ev_one_roll(CR_ANY_CRAPS, 0), -1, 9, "any craps is -1/9");
    check_ev(ev_one_roll(CR_ELEVEN, 0), -1, 9, "eleven the same");
    check_ev(ev_one_roll(CR_TWO, 0), -5, 36, "two is -5/36");
    check_ev(ev_one_roll(CR_TWELVE, 0), -5, 36, "twelve the same");

    /* The field would be a losing bet without the double and triple
     * ends -- sixteen of thirty-six ways win, which is under half. */
    {
        int d1, d2, w = 0;
        for (d1 = 1; d1 <= 6; d1++)
            for (d2 = 1; d2 <= 6; d2++)
                if (cr_resolve(CR_FIELD, 0, d1 + d2, 0) == CR_WIN) w++;
        check(w == 16, "the field wins sixteen ways in thirty-six");
    }
}

static void test_hardways(void)
{
    int d1, d2, w, l, s;
    static const int hw[4] = { 4, 6, 8, 10 };
    int i, bad = 0;

    for (i = 0; i < 4; i++) {
        frac ev;
        int num, den;
        w = l = s = 0;
        for (d1 = 1; d1 <= 6; d1++)
            for (d2 = 1; d2 <= 6; d2++) {
                int r = cr_hard_resolve(hw[i], d1, d2, 0);
                if (r == CR_WIN) w++;
                else if (r == CR_LOSE) l++;
                else s++;
            }
        cr_payout(CR_HARD, hw[i], hw[i], &num, &den);
        ev = add(mul(mk(w, w + l), mk(num, den)), mk(-l, w + l));

        if (hw[i] == 4 || hw[i] == 10) {
            if (!(ev.n == -1 && ev.d == 9)) bad++;
        } else {
            if (!(ev.n == -1 && ev.d == 11)) bad++;
        }
    }
    check(bad == 0, "hard 4 and 10 are -1/9, hard 6 and 8 are -1/11");

    /* THE ONE BET THAT NEEDS THE DICE. A six made of 4-2 loses hard six
     * and wins place six, from the same roll. */
    check(cr_hard_resolve(6, 3, 3, 4) == CR_WIN, "3-3 wins hard six");
    check(cr_hard_resolve(6, 4, 2, 4) == CR_LOSE, "4-2 loses it");
    check(cr_resolve(CR_PLACE, 6, 6, 4) == CR_WIN,
        "while place six wins on either");
    check(cr_hard_resolve(6, 3, 4, 4) == CR_LOSE, "and a seven kills it");
    check(cr_hard_resolve(6, 5, 5, 4) == CR_STAND, "a ten leaves it alone");
}

static void test_come(void)
{
    /* A come bet on its first roll is a pass line bet, whatever the
     * table's point is. That equivalence is the whole idea, and it is
     * checkable directly. */
    int roll, point, bad = 0;

    for (point = 4; point <= 10; point++) {
        if (point == 7) continue;
        for (roll = 2; roll <= 12; roll++) {
            int c = cr_resolve(CR_COME, 0, roll, point);
            int p = cr_resolve(CR_PASS, 0, roll, 0);
            if (p == CR_STAND) p = CR_MOVE;   /* the pass line sets a point */
            if (c != p) bad++;
        }
    }
    check(bad == 0, "a come bet's first roll is a pass line bet");

    /* And once it has a number it behaves like a place bet that pays
     * even money. */
    check(cr_resolve(CR_COME, 8, 8, 5) == CR_WIN, "a come 8 wins on eight");
    check(cr_resolve(CR_COME, 8, 7, 5) == CR_LOSE, "and loses on seven");
    check(cr_resolve(CR_COME, 8, 5, 5) == CR_STAND,
        "and ignores the table's point");
}

static void test_validity(void)
{
    check(!cr_bet_valid(CR_PLACE, 7), "seven cannot be placed");
    check(!cr_bet_valid(CR_PLACE, 2), "nor two");
    check(cr_bet_valid(CR_PLACE, 6), "but six can");
    check(!cr_bet_valid(CR_HARD, 5), "an odd number has no hard way");
    check(cr_bet_valid(CR_HARD, 8), "an even one does");
    check(!cr_bet_valid(CR_PASS_ODDS, 0),
        "there are no odds without a point");
    check(!cr_bet_valid(CR_NTYPES, 0), "and no bet of an unknown type");

    /* Place bets are off on the come-out, which is the convention and
     * protects the player from the roll where sevens are likeliest. */
    check(cr_resolve(CR_PLACE, 6, 7, 0) == CR_STAND,
        "a place bet is off on the come-out");
    check(cr_resolve(CR_PLACE, 6, 7, 5) == CR_LOSE, "and on once a point is");

    {
        int t, bad = 0;
        for (t = 0; t < CR_NTYPES; t++)
            if (!cr_type_name(t) || !cr_type_name(t)[0]) bad++;
        check(bad == 0, "every bet type has a name");
    }
}

int main(void)
{
    printf("craps: bet and edge tests\n");

    test_validity();
    test_line();
    test_odds_are_free();
    test_place_and_props();
    test_hardways();
    test_come();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
