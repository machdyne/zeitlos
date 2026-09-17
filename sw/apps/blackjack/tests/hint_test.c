/*
 * Zeitlos blackjack -- host tests for basic strategy.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Basic strategy is a published fact about the game, not an opinion, so
 * it is testable the same way the poker hand counts and roulette's
 * 36-unit return are: assert the cells everybody knows, plus the shape
 * properties a mistyped table cannot satisfy.
 *
 * The strongest of those is the last one: OVER THOUSANDS OF RANDOM
 * HANDS, THE HINT IS ALWAYS AN ACTION THE ENGINE ACCEPTS. A hint the
 * player cannot follow is worse than no hint -- it teaches a rule and
 * then refuses it.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../bj_hint.h"
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

static bj_game_t g;

static uint8_t C(const char *s) { return zcard_parse(s); }

/* Sets up a hand and a dealer up-card directly, without dealing -- the
 * point is the advice, not the shoe. */
static int advise(const char *a, const char *b, const char *up)
{
    bj_rules_t r;
    bj_rules_default(&r);
    bj_game_init(&g, &r);

    g.nhands = 1;
    g.active = 0;
    g.phase = BJ_PHASE_PLAYER;
    g.hand[0].n = 2;
    g.hand[0].bet = 10;
    g.hand[0].card[0] = C(a);
    g.hand[0].card[1] = C(b);
    g.ndealer = 2;
    g.dealer[0] = C(up);
    g.dealer[1] = C("2c");

    return bj_hint_ideal(&g);
}

static void test_famous_cells(void)
{
    /* ALWAYS SPLIT ACES AND EIGHTS. The eights cell is the one a
     * totals-first lookup gets wrong: a pair of eights is sixteen, and
     * the hard chart says stand or surrender there. */
    check_eq(advise("8h", "8s", "Td"), BJ_HINT_SPLIT,
        "eights are split against a ten");
    check_eq(advise("8h", "8s", "Ad"), BJ_HINT_SPLIT,
        "and against an ace");
    check_eq(advise("Ah", "As", "Ad"), BJ_HINT_SPLIT, "aces are always split");

    /* NEVER SPLIT TENS OR FIVES. Twenty is already a winning hand, and
     * a pair of fives is a ten, which wants doubling. */
    check_eq(advise("Th", "Ks", "6d"), BJ_HINT_STAND,
        "tens are never split, even against a six");
    check_eq(advise("5h", "5s", "6d"), BJ_HINT_DOUBLE,
        "fives are doubled, not split");

    /* The hard totals everybody knows. */
    check_eq(advise("Th", "7s", "Ad"), BJ_HINT_STAND, "hard 17 always stands");
    check_eq(advise("9h", "2s", "5d"), BJ_HINT_DOUBLE, "eleven doubles");
    check_eq(advise("9h", "2s", "Ad"), BJ_HINT_HIT, "but hits against an ace");
    check_eq(advise("Th", "2s", "2d"), BJ_HINT_HIT, "twelve hits against a two");
    check_eq(advise("Th", "2s", "4d"), BJ_HINT_STAND, "and stands against a four");
    check_eq(advise("Th", "6s", "7d"), BJ_HINT_HIT, "sixteen hits against a seven");
    check_eq(advise("Th", "6s", "6d"), BJ_HINT_STAND, "and stands against a six");

    /* SOFT EIGHTEEN, the hand that needs two kinds of double. */
    check_eq(advise("Ah", "7s", "2d"), BJ_HINT_STAND, "soft 18 stands on a two");
    check_eq(advise("Ah", "7s", "5d"), BJ_HINT_DOUBLE, "doubles on a five");
    check_eq(advise("Ah", "7s", "9d"), BJ_HINT_HIT, "and hits against a nine");
    check_eq(advise("Ah", "8s", "6d"), BJ_HINT_STAND, "soft 19 always stands");

    /* Surrender, the rule most tables omit and most players forget. */
    check_eq(advise("Th", "6s", "Td"), BJ_HINT_SURRENDER,
        "sixteen surrenders against a ten");
    check_eq(advise("Th", "5s", "Td"), BJ_HINT_SURRENDER,
        "and so does fifteen");
    check_eq(advise("Th", "5s", "9d"), BJ_HINT_HIT,
        "but not against a nine");

    /* Nines: split against most, but stand on seven, ten and ace --
     * the one pair row that is not monotonic, and therefore the one a
     * "split small cards" rule of thumb gets wrong. */
    check_eq(advise("9h", "9s", "7d"), BJ_HINT_STAND, "nines stand on a seven");
    check_eq(advise("9h", "9s", "8d"), BJ_HINT_SPLIT, "split on an eight");
    check_eq(advise("9h", "9s", "Td"), BJ_HINT_STAND, "and stand on a ten");
}

static void test_shape(void)
{
    int up, bad = 0;
    static const char *const ups[10] = {
        "2c", "3c", "4c", "5c", "6c", "7c", "8c", "9c", "Tc", "Ac" };

    /* EVERY CELL RETURNS SOMETHING. A mistyped table leaves a hole,
     * and a hole here is a hand with no advice at all. */
    for (up = 0; up < 10; up++) {
        int a, b;
        for (a = 0; a < Z_NCARDS; a += 3)
            for (b = 0; b < Z_NCARDS; b += 5) {
                int h;
                bj_rules_t r;
                bj_rules_default(&r);
                bj_game_init(&g, &r);
                g.nhands = 1; g.active = 0; g.phase = BJ_PHASE_PLAYER;
                g.hand[0].n = 2; g.hand[0].bet = 10;
                g.hand[0].card[0] = (uint8_t)a;
                g.hand[0].card[1] = (uint8_t)b;
                g.ndealer = 2;
                g.dealer[0] = C(ups[up]);
                g.dealer[1] = C("2c");
                h = bj_hint_ideal(&g);
                if (h < 0 || h > BJ_SURRENDER) bad++;
            }
    }
    check_eq(bad, 0, "every two-card hand against every up-card has advice");

    /* Nothing to advise when it is not the player's turn. */
    g.phase = BJ_PHASE_DONE;
    check_eq(bj_hint(&g), -1, "a finished round advises nothing");
    g.phase = BJ_PHASE_PLAYER;
    g.hand[0].done = true;
    check_eq(bj_hint(&g), -1, "and neither does a finished hand");
    g.hand[0].done = false;

    check(bj_hint_name(BJ_HINT_SPLIT)[0] != '\0', "every hint has a name");
    check(bj_hint_name(-1)[0] == '\0', "and nothing has none");

    /* The table's assumptions are declared rather than assumed. */
    {
        bj_rules_t r;
        bj_rules_default(&r);
        check(bj_hint_exact(&r), "the defaults are the chart's own rules");
        r.dealer_hits_soft17 = true;
        check(!bj_hint_exact(&r), "H17 is flagged as a different game");
        bj_rules_default(&r);
        r.ndecks = 1;
        check(!bj_hint_exact(&r), "and so is single deck");
    }
}

static uint32_t st = 7;

static uint32_t frand(void *ctx)
{
    (void)ctx;
    st ^= st << 13; st ^= st >> 17; st ^= st << 5;
    return st;
}

static void test_always_legal(void)
{
    long round;
    int illegal = 0, none = 0, followed = 0;

    zg_rng_set(frand, NULL);

    /* THE ONE THAT MATTERS. Play thousands of rounds following the hint
     * every time. Every hint must be an action the engine accepts --
     * a hint the player cannot follow teaches a rule and then refuses
     * it. */
    for (round = 0; round < 4000; round++) {
        bj_rules_t r;
        int guard = 0;

        bj_rules_default(&r);
        /* Including rule sets the chart was not built for, because the
         * reduction has to stay legal even where the advice is
         * approximate. */
        r.dealer_hits_soft17 = (round & 1) != 0;
        r.double_after_split = (round & 2) != 0;
        r.surrender = (round & 4) != 0;
        r.ndecks = 1 + (int)(round % 8);

        bj_game_init(&g, &r);
        if (!bj_round_begin(&g, 10)) continue;

        while (g.phase != BJ_PHASE_DONE && guard++ < 60) {
            int h;

            if (g.phase == BJ_PHASE_INSURANCE) {
                /* Basic strategy never takes insurance. It is a bet on
                 * the hole card at 2:1 when the true odds are worse,
                 * and it is the one decision at the table that no hand
                 * ever justifies. */
                bj_insure(&g, 0);
                continue;
            }

            h = bj_hint(&g);
            if (h < 0) { none++; break; }

            if (!bj_act(&g, h)) { illegal++; break; }
            followed++;
        }

        if (g.phase != BJ_PHASE_DONE && guard < 60) none++;
    }

    check_eq(illegal, 0, "every hint is an action the engine accepts");
    check_eq(none, 0, "and there is always advice while a hand is live");
    check(followed > 4000, "over a lot of decisions");
    printf("      %d decisions followed across 4,000 rounds\n", followed);

    zg_rng_set(NULL, NULL);
}

int main(void)
{
    printf("blackjack: strategy tests\n");

    test_famous_cells();
    test_shape();
    test_always_legal();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
