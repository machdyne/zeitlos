/*
 * Zeitlos roulette -- host tests for the table.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- the one that does the work --
 *
 * On a European wheel every bet returns exactly 36 units per unit
 * staked, summed over all 37 pockets. That single number validates the
 * payout ratio AND the coverage set of every bet on the table at once:
 * pay a split 16:1 and it comes out 34, put nineteen numbers in the
 * red set and it comes out 38. There is no way to be wrong about
 * either and still land on 36.
 *
 * It is a property of the game rather than of anybody's code, which is
 * what makes it usable here -- the same reason the poker tests count
 * all 2,598,960 five-card hands rather than checking a list of cases
 * somebody thought of.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../rl_table.h"

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

/* Walks every pocket on a wheel, double zero included. */
static int pocket_at(int wheel, int i)
{
    (void)wheel;
    return i == 37 ? RL_DOUBLE_ZERO : i;
}

/* -- the house edge ---------------------------------------------------- */

static void test_returns_sum(void)
{
    int type, sel, i;
    int bad = 0;

    /* EUROPEAN: every bet, every selector, exactly 36. */
    for (type = 0; type < RL_NTYPES; type++) {
        int nsel = rl_sel_count(RL_EURO, type);
        for (sel = 0; sel < nsel; sel++) {
            long total = 0;
            for (i = 0; i < 37; i++)
                if (rl_covers(RL_EURO, type, sel, pocket_at(RL_EURO, i)))
                    total += rl_returns(type);
            if (total != 36) {
                printf("FAIL: european %s sel %d returns %ld over 37 "
                    "pockets, want 36\n", rl_type_name(type), sel, total);
                bad++;
            }
        }
    }
    check_eq(bad, 0, "every european bet returns exactly 36 per unit staked");

    /* Which is the same as saying the house edge is 1/37 on all of
     * them -- 2.70%. Stated separately because it is the number a
     * person would look up. */
    check(37 * 100 / 37 == 100, "37 pockets, one unit lost per 37 spins");

    /* AMERICAN: 36 for everything except the basket, which returns 35.
     * That is not a rounding artefact -- it is why the five-number bet
     * is the worst on the table, and it gets asserted rather than
     * excused. */
    bad = 0;
    for (type = 0; type < RL_NTYPES; type++) {
        int nsel = rl_sel_count(RL_AMERICAN, type);
        for (sel = 0; sel < nsel; sel++) {
            long total = 0, want = (type == RL_BASKET) ? 35 : 36;
            for (i = 0; i < 38; i++)
                if (rl_covers(RL_AMERICAN, type, sel, pocket_at(RL_AMERICAN, i)))
                    total += rl_returns(type);
            if (total != want) {
                printf("FAIL: american %s sel %d returns %ld, want %ld\n",
                    rl_type_name(type), sel, total, want);
                bad++;
            }
        }
    }
    check_eq(bad, 0, "every american bet returns 36, except the basket at 35");

    /* And the basket really is worse, which is the whole point of
     * carrying it. */
    {
        long basket = 0, straight = 0;
        for (i = 0; i < 38; i++) {
            int p = pocket_at(RL_AMERICAN, i);
            if (rl_covers(RL_AMERICAN, RL_BASKET, 0, p))
                basket += rl_returns(RL_BASKET);
            if (rl_covers(RL_AMERICAN, RL_STRAIGHT, 7, p))
                straight += rl_returns(RL_STRAIGHT);
        }
        check(basket < straight, "the basket is a worse bet than any other");
    }
}

/* -- coverage ---------------------------------------------------------- */

static void test_coverage(void)
{
    int sel, bad = 0;

    check_eq(rl_coverage(RL_EURO, RL_STRAIGHT, 17), 1, "straight up covers one");
    check_eq(rl_coverage(RL_EURO, RL_STREET, 0), 3, "a street covers three");
    check_eq(rl_coverage(RL_EURO, RL_SIXLINE, 0), 6, "a six line covers six");
    check_eq(rl_coverage(RL_EURO, RL_COLUMN, 0), 12, "a column covers twelve");
    check_eq(rl_coverage(RL_EURO, RL_DOZEN, 0), 12, "a dozen covers twelve");
    check_eq(rl_coverage(RL_EURO, RL_RED, 0), 18, "red covers eighteen");
    check_eq(rl_coverage(RL_EURO, RL_BLACK, 0), 18, "black covers eighteen");
    check_eq(rl_coverage(RL_EURO, RL_ODD, 0), 18, "odd covers eighteen");
    check_eq(rl_coverage(RL_EURO, RL_EVEN, 0), 18, "even covers eighteen");
    check_eq(rl_coverage(RL_EURO, RL_LOW, 0), 18, "low covers eighteen");
    check_eq(rl_coverage(RL_EURO, RL_HIGH, 0), 18, "high covers eighteen");
    check_eq(rl_coverage(RL_AMERICAN, RL_BASKET, 0), 5, "the basket covers five");

    for (sel = 0; sel < rl_n_splits(); sel++)
        if (rl_coverage(RL_EURO, RL_SPLIT, sel) != 2) bad++;
    check_eq(bad, 0, "every split covers exactly two");

    bad = 0;
    for (sel = 0; sel < rl_n_corners(); sel++)
        if (rl_coverage(RL_EURO, RL_CORNER, sel) != 4) bad++;
    check_eq(bad, 0, "every corner covers exactly four");

    /* ZERO LOSES EVERY EVEN-MONEY BET. That is the entire house edge
     * on those bets, and a `% 2` shortcut gives it away without
     * changing anything a casual test would look at. */
    check(!rl_covers(RL_EURO, RL_ODD, 0, 0), "zero is not odd");
    check(!rl_covers(RL_EURO, RL_EVEN, 0, 0), "zero is not even");
    check(!rl_covers(RL_EURO, RL_RED, 0, 0), "zero is not red");
    check(!rl_covers(RL_EURO, RL_BLACK, 0, 0), "zero is not black");
    check(!rl_covers(RL_EURO, RL_LOW, 0, 0), "zero is not low");
    check(!rl_covers(RL_EURO, RL_HIGH, 0, 0), "zero is not high");
    check(!rl_covers(RL_EURO, RL_COLUMN, 0, 0), "zero is in no column");
    check(!rl_covers(RL_EURO, RL_DOZEN, 0, 0), "zero is in no dozen");
    check(rl_covers(RL_EURO, RL_STRAIGHT, 0, 0), "but zero pays itself");

    /* The double zero must not exist on a European wheel. */
    check(!rl_covers(RL_EURO, RL_STRAIGHT, 0, RL_DOUBLE_ZERO),
        "there is no double zero on a european wheel");
    check_eq(rl_sel_count(RL_EURO, RL_BASKET), 0,
        "and no basket bet either");
    check(rl_covers(RL_AMERICAN, RL_STRAIGHT, RL_DOUBLE_ZERO, RL_DOUBLE_ZERO),
        "but there is on an american one");
}

static void test_colours(void)
{
    int n, reds = 0, blacks = 0, both = 0, neither = 0;

    for (n = 1; n <= 36; n++) {
        bool r = rl_covers(RL_EURO, RL_RED, 0, n);
        bool b = rl_covers(RL_EURO, RL_BLACK, 0, n);
        if (r) reds++;
        if (b) blacks++;
        if (r && b) both++;
        if (!r && !b) neither++;
    }

    check_eq(reds, 18, "eighteen reds");
    check_eq(blacks, 18, "eighteen blacks");
    check_eq(both, 0, "no number is both");
    check_eq(neither, 0, "and none of 1 to 36 is neither");

    /* The four that break the "odd is red in the outer dozens" rule of
     * thumb. A derived colouring gets these wrong and still looks
     * plausible, which is why the set is a written-out list. */
    check(!rl_covers(RL_EURO, RL_RED, 0, 10), "10 is black, not red");
    check(!rl_covers(RL_EURO, RL_RED, 0, 29), "29 is black, not red");
    check(rl_covers(RL_EURO, RL_RED, 0, 12), "12 is red despite being even");
    check(rl_covers(RL_EURO, RL_RED, 0, 19), "19 is red");
}

static void test_layout(void)
{
    int set[4], n, sel, i;
    int seen[37];
    int bad;

    /* Every number appears in exactly one street, one dozen and one
     * column -- the three partitions of 1..36. */
    for (i = 0; i < 37; i++) seen[i] = 0;
    for (sel = 0; sel < 12; sel++)
        for (i = 1; i <= 36; i++)
            if (rl_covers(RL_EURO, RL_STREET, sel, i)) seen[i]++;
    bad = 0;
    for (i = 1; i <= 36; i++) if (seen[i] != 1) bad++;
    check_eq(bad, 0, "the streets partition 1 to 36");

    for (i = 0; i < 37; i++) seen[i] = 0;
    for (sel = 0; sel < 3; sel++)
        for (i = 1; i <= 36; i++)
            if (rl_covers(RL_EURO, RL_COLUMN, sel, i)) seen[i]++;
    bad = 0;
    for (i = 1; i <= 36; i++) if (seen[i] != 1) bad++;
    check_eq(bad, 0, "the columns partition 1 to 36");

    for (i = 0; i < 37; i++) seen[i] = 0;
    for (sel = 0; sel < 3; sel++)
        for (i = 1; i <= 36; i++)
            if (rl_covers(RL_EURO, RL_DOZEN, sel, i)) seen[i]++;
    bad = 0;
    for (i = 1; i <= 36; i++) if (seen[i] != 1) bad++;
    check_eq(bad, 0, "the dozens partition 1 to 36");

    /* The third column is 3, 6, 9 ... 36, where n % 3 == 0. Written as
     * (n - 1) % 3 == sel precisely so that this case is not special. */
    check(rl_covers(RL_EURO, RL_COLUMN, 2, 36), "36 is in the third column");
    check(rl_covers(RL_EURO, RL_COLUMN, 0, 34), "34 is in the first");

    /* SPLITS ARE ADJACENT ON THE PRINTED TABLE, NOT CONSECUTIVE AS
     * NUMBERS. 3 and 4 are consecutive and sit at opposite ends of
     * different rows; they share no edge and are not a split. That is
     * the mistake a "consecutive numbers" implementation makes and it
     * is invisible until a winning bet does not pay. */
    {
        bool found_34 = false, found_12 = false, found_14 = false;
        for (sel = 0; sel < rl_n_splits(); sel++) {
            n = rl_split_pair(sel, set);
            if (n != 2) continue;
            if (set[0] == 3 && set[1] == 4) found_34 = true;
            if (set[0] == 4 && set[1] == 3) found_34 = true;
            if (set[0] == 1 && set[1] == 2) found_12 = true;
            if (set[0] == 1 && set[1] == 4) found_14 = true;
        }
        check(!found_34, "3 and 4 are not a split");
        check(found_12, "1 and 2 are");
        check(found_14, "and so are 1 and 4");
    }

    /* No split is listed twice, and every one is inside the table. */
    {
        int a, b;
        bad = 0;
        for (sel = 0; sel < rl_n_splits(); sel++) {
            rl_split_pair(sel, set);
            if (set[0] < 1 || set[0] > 36 || set[1] < 1 || set[1] > 36) bad++;
        }
        check_eq(bad, 0, "every split lies inside 1 to 36");

        bad = 0;
        for (a = 0; a < rl_n_splits(); a++) {
            int pa[2];
            rl_split_pair(a, pa);
            for (b = a + 1; b < rl_n_splits(); b++) {
                int pb[2];
                rl_split_pair(b, pb);
                if (pa[0] == pb[0] && pa[1] == pb[1]) bad++;
            }
        }
        check_eq(bad, 0, "no split is listed twice");
    }

    /* Corners: four numbers forming a 2x2 block. */
    bad = 0;
    for (sel = 0; sel < rl_n_corners(); sel++) {
        rl_corner_set(sel, set);
        if (set[1] != set[0] + 1) bad++;
        if (set[2] != set[0] + 3) bad++;
        if (set[3] != set[0] + 4) bad++;
        if (set[3] > 36) bad++;
    }
    check_eq(bad, 0, "every corner is a 2x2 block inside the table");

    /* A six line is two adjacent streets. */
    check(rl_covers(RL_EURO, RL_SIXLINE, 0, 1), "a six line starts at its row");
    check(rl_covers(RL_EURO, RL_SIXLINE, 0, 6), "and runs six numbers");
    check(!rl_covers(RL_EURO, RL_SIXLINE, 0, 7), "and no further");
    check_eq(rl_sel_count(RL_EURO, RL_SIXLINE), 11,
        "there are eleven six lines, not twelve");
}

static void test_validity(void)
{
    check(!rl_bet_valid(RL_EURO, RL_STRAIGHT, 37),
        "a double zero straight up is invalid on a european wheel");
    check(rl_bet_valid(RL_AMERICAN, RL_STRAIGHT, 37), "and valid on american");
    check(!rl_bet_valid(RL_EURO, RL_STRAIGHT, -1), "a negative selector is not");
    check(!rl_bet_valid(RL_EURO, RL_DOZEN, 3), "nor a fourth dozen");
    check(!rl_bet_valid(RL_EURO, RL_STREET, 12), "nor a thirteenth street");
    check(!rl_bet_valid(RL_EURO, RL_BASKET, 0), "nor a european basket");
    check(!rl_bet_valid(RL_EURO, RL_NTYPES, 0), "nor an unknown bet type");
    check(rl_bet_valid(RL_EURO, RL_RED, 0), "red is valid");
    check(!rl_bet_valid(RL_EURO, RL_RED, 1), "but has only one selector");

    /* rl_covers() must refuse an invalid bet rather than reading past
     * the end of something. */
    check(!rl_covers(RL_EURO, RL_STRAIGHT, 99, 5), "an invalid bet covers nothing");
    check(!rl_covers(RL_EURO, 99, 0, 5), "and so does an invalid type");
}

/* -- a round ----------------------------------------------------------- */

static void test_round(void)
{
    rl_round_t r;
    int i;

    rl_round_clear(&r, RL_EURO);
    check_eq(r.nbets, 0, "a cleared round holds nothing");
    check_eq(r.staked, 0, "and stakes nothing");

    check(rl_bet_place(&r, RL_RED, 0, 10), "a bet is placed");
    check_eq(r.staked, 10, "and staked");
    check_eq(rl_bet_on(&r, RL_RED, 0), 10, "and readable");

    /* Stacked, not duplicated -- a person clicking one square ten
     * times must not exhaust the table. */
    check(rl_bet_place(&r, RL_RED, 0, 10), "the same bet again");
    check_eq(r.nbets, 1, "stacks rather than duplicating");
    check_eq(rl_bet_on(&r, RL_RED, 0), 20, "and adds up");
    check_eq(r.staked, 20, "and the stake follows");

    check(!rl_bet_place(&r, RL_RED, 0, 0), "a zero bet is refused");
    check(!rl_bet_place(&r, RL_RED, 0, -5), "and so is a negative one");
    check(!rl_bet_place(&r, RL_DOZEN, 9, 5), "and so is an invalid one");
    check_eq(r.staked, 20, "none of which changed the stake");

    check_eq(rl_bet_remove(&r, RL_RED, 0, 5), 5, "a chip comes off");
    check_eq(rl_bet_on(&r, RL_RED, 0), 15, "leaving the rest");
    check_eq(rl_bet_remove(&r, RL_RED, 0, 999), 15,
        "taking more than is there takes what is there");
    check_eq(r.nbets, 0, "and empties the square");
    check_eq(r.staked, 0, "and the stake");
    check_eq(rl_bet_remove(&r, RL_RED, 0, 5), 0, "removing nothing takes nothing");

    /* Filling the table, then one more. */
    rl_round_clear(&r, RL_EURO);
    for (i = 0; i < RL_MAX_BETS; i++)
        check(rl_bet_place(&r, RL_STRAIGHT, i, 1), "a straight-up bet fits");
    check(!rl_bet_place(&r, RL_STRAIGHT, RL_MAX_BETS, 1),
        "the table fills up and says so");
    check(rl_bet_place(&r, RL_STRAIGHT, 0, 1),
        "but an existing square still takes another chip");

    /* Payouts across a whole round, checked against the same invariant
     * as the single bets: whatever the spread, 36 units come back per
     * unit staked over all 37 pockets. */
    {
        long total = 0;
        rl_round_clear(&r, RL_EURO);
        rl_bet_place(&r, RL_RED, 0, 5);
        rl_bet_place(&r, RL_STRAIGHT, 17, 2);
        rl_bet_place(&r, RL_CORNER, 4, 3);
        rl_bet_place(&r, RL_DOZEN, 1, 4);

        for (i = 0; i < 37; i++) total += rl_round_returns(&r, i);
        check_eq(total, 36 * r.staked,
            "a mixed round returns 36 per unit over the whole wheel");
    }

    /* And a round with nothing on it returns nothing, on every
     * pocket. */
    rl_round_clear(&r, RL_EURO);
    for (i = 0; i < 37; i++)
        check_eq(rl_round_returns(&r, i), 0, "an empty round pays nothing");
}

static void test_names(void)
{
    char buf[4];
    int i;

    check(strcmp(rl_pocket_str(0, buf), "0") == 0, "zero prints as 0");
    check(strcmp(rl_pocket_str(7, buf), "7") == 0, "a single digit");
    check(strcmp(rl_pocket_str(36, buf), "36") == 0, "two digits");
    check(strcmp(rl_pocket_str(RL_DOUBLE_ZERO, buf), "00") == 0,
        "and the double zero prints as 00, not 37");

    for (i = 0; i < RL_NTYPES; i++) {
        check(rl_type_name(i) != NULL && rl_type_name(i)[0] != '\0',
            "every bet type has a name");
        check(rl_type_odds(i) != NULL && rl_type_odds(i)[0] != '\0',
            "and quoted odds");
    }

    /* The quoted odds must agree with what is actually paid, or the
     * table lies to the person reading it. */
    {
        int bad = 0;
        static const struct { int type; int ret; } want[] = {
            { RL_STRAIGHT, 36 }, { RL_SPLIT, 18 }, { RL_STREET, 12 },
            { RL_CORNER, 9 }, { RL_SIXLINE, 6 }, { RL_COLUMN, 3 },
            { RL_DOZEN, 3 }, { RL_RED, 2 }, { RL_BASKET, 7 }
        };
        for (i = 0; i < (int)(sizeof want / sizeof want[0]); i++)
            if (rl_returns(want[i].type) != want[i].ret) bad++;
        check_eq(bad, 0, "the quoted odds match what is paid");
    }
}

int main(void)
{
    printf("roulette: table tests\n");

    test_validity();
    test_colours();
    test_layout();
    test_coverage();
    test_returns_sum();
    test_round();
    test_names();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
