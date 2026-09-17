/*
 * Zeitlos poker -- host tests for the opponents.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- what can and cannot be asserted about a poker AI --
 *
 * "Plays well" is not a testable proposition and nothing here pretends
 * otherwise. Four things ARE testable, and between them they cover the
 * failures that actually happen:
 *
 *   1. The EQUITY ESTIMATOR against hands whose answers are known
 *      independently. Aces against kings is 82% and has been for as
 *      long as anybody has been counting; a board that is already
 *      decided is 0% or 100% with no sampling error at all. An
 *      estimator that is wrong here makes every decision above it
 *      wrong in a way no amount of watching it play would reveal.
 *
 *   2. LEGALITY, exhaustively. An opponent that returns an illegal
 *      action does not misplay a hand, it wedges the app -- pk_act()
 *      refuses and the game stops with nobody to act. This is the
 *      failure that matters most and it is the easiest to check.
 *
 *   3. The LADDER being a ladder. A level 7 opponent must actually
 *      beat a level 1 opponent over enough hands for the variance to
 *      settle. Otherwise the difficulty setting is decoration.
 *
 *   4. ABORTING cleanly, since the real app interrupts a search every
 *      time the window manager wants a redraw.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../pk_ai.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (ok) return;
    failures++;
    printf("FAIL: %s\n", what);
}

static void check_range(int32_t got, int32_t lo, int32_t hi, const char *what)
{
    checks++;
    if (got >= lo && got <= hi) return;
    failures++;
    printf("FAIL: %s -- got %d, want %d..%d\n", what, (int)got, (int)lo, (int)hi);
}

static uint8_t C(const char *s)
{
    uint8_t c = zcard_parse(s);
    if (c == Z_CARD_NONE) { printf("bad card \"%s\"\n", s); exit(1); }
    return c;
}

static void cards(const char *const *names, int n, uint8_t *out)
{
    int i;
    for (i = 0; i < n; i++) out[i] = C(names[i]);
}

/* -- the equity estimator -------------------------------------------- */

static void test_equity_known(void)
{
    uint8_t hero[2], opp[2], board[5];
    static const char *const aa[] = { "Ah", "Ad" };
    static const char *const kk[] = { "Kh", "Kd" };
    static const char *const seven_two[] = { "7h", "2d" };
    int32_t eq;

    zg_rng_seed(4242);

    /* The headline number. A pair of aces against a pair of kings,
     * all five board cards to come, is a shade over 82%. It is not a
     * figure anybody has to take on trust -- it is a property of the
     * deck, countable by anyone with the patience, and it is the
     * single best check that the sampler, the runout and the
     * comparison all agree.
     *
     * The tolerance is the sampling error at 40,000 rollouts, which
     * is under half a point, widened to two points so this never
     * fails on an unlucky seed. */
    cards(aa, 2, hero);
    cards(kk, 2, opp);
    eq = pk_ai_equity_vs(hero, 2, opp, 2, board, 0, 5, 25000);
    check_range(eq, 802, 848, "aces against kings is about 82%");

    /* And the other way round, which must be the complement. If these
     * two do not add to 1000 the tie handling is wrong. */
    {
        int32_t back = pk_ai_equity_vs(opp, 2, hero, 2, board, 0, 5, 25000);
        check_range(eq + back, 985, 1015,
            "the two sides of a matchup add up to one");
    }

    /* The worst starting hand in hold'em against the best. */
    cards(seven_two, 2, hero);
    eq = pk_ai_equity_vs(hero, 2, opp, 2, board, 0, 5, 20000);
    check_range(eq, 80, 160, "seven-deuce against kings is a big underdog");

    /* A coin flip is a coin flip: a pair against two overcards runs
     * close to even. */
    {
        static const char *const nn[] = { "9h", "9d" };
        static const char *const aq[] = { "Ac", "Qs" };
        cards(nn, 2, hero);
        cards(aq, 2, opp);
        eq = pk_ai_equity_vs(hero, 2, opp, 2, board, 0, 5, 20000);
        check_range(eq, 500, 580, "nines against ace-queen is near even");
    }
}

static void test_equity_decided(void)
{
    uint8_t hero[2], opp[2], board[5];
    static const char *const b[] = { "Kh", "Qh", "Jh", "2c", "3d" };
    static const char *const nuts[] = { "Ah", "Th" };
    static const char *const junk[] = { "7s", "4c" };
    int32_t eq;

    cards(b, 5, board);
    cards(nuts, 2, hero);
    cards(junk, 2, opp);

    /* Nothing left to come, so there is nothing to sample and the
     * answer must be exact. A sampler that returns 998 here has an
     * off-by-one somewhere in the runout. */
    eq = pk_ai_equity_vs(hero, 2, opp, 2, board, 5, 5, 200);
    check(eq == 1000, "a made royal flush on a finished board is certain");

    eq = pk_ai_equity_vs(opp, 2, hero, 2, board, 5, 5, 200);
    check(eq == 0, "and the other side is hopeless");

    /* An exact tie must be exactly half, not nearly half. */
    {
        static const char *const h1[] = { "2c", "3d" };
        static const char *const h2[] = { "2s", "3h" };
        static const char *const bb[] = { "Ac", "Kd", "Qs", "Jh", "Th" };
        cards(h1, 2, hero);
        cards(h2, 2, opp);
        cards(bb, 5, board);
        eq = pk_ai_equity_vs(hero, 2, opp, 2, board, 5, 5, 200);
        check(eq == 500, "a dead tie is exactly half");
    }
}

static void test_equity_monotone(void)
{
    uint8_t board[5];
    uint8_t a[2], b[2], c[2];
    static const char *const s1[] = { "Ah", "Ad" };
    static const char *const s2[] = { "Ah", "Kh" };
    static const char *const s3[] = { "6h", "2d" };
    static const char *const opp[] = { "9c", "9s" };
    uint8_t o[2];
    int32_t e1, e2, e3;

    zg_rng_seed(99);

    cards(s1, 2, a); cards(s2, 2, b); cards(s3, 2, c); cards(opp, 2, o);

    e1 = pk_ai_equity_vs(a, 2, o, 2, board, 0, 5, 12000);
    e2 = pk_ai_equity_vs(b, 2, o, 2, board, 0, 5, 12000);
    e3 = pk_ai_equity_vs(c, 2, o, 2, board, 0, 5, 12000);

    /* Against a pair of nines: aces are a big favourite, suited
     * ace-king is a slight underdog, six-deuce is a large one. The
     * gaps are tens of points, not fractions of one.
     *
     * An earlier version of this used queens instead of ace-king,
     * which was a bad test rather than a failing one: queens against
     * nines and aces against nines are BOTH about 80.5%, so it was
     * asking a sampler to resolve a half-point difference and it
     * failed on noise exactly as it should have. */
    check(e1 > e2 + 200, "aces beat ace-king against the same hand");
    check(e2 > e3 + 80, "ace-king beats six-deuce against the same hand");
}

static void test_equity_in_game(void)
{
    pk_game_t g;
    pk_ai_t ai;
    int32_t eq;

    pk_ai_init(&ai, 6);
    zg_rng_seed(7);

    /* More opponents is less equity for the same hand, which is the
     * relationship every decision above this depends on. */
    {
        int32_t heads_up, six_way;
        int i;

        pk_game_init(&g, &pk_variant_holdem, 2, 1000, 5, 10);
        pk_hand_begin(&g);
        for (i = 0; i < 2; i++) { g.seat[0].hole[0] = C("Ah"); g.seat[0].hole[1] = C("Ad"); }
        heads_up = pk_ai_equity_game(&ai, &g, 0, 3000);

        pk_game_init(&g, &pk_variant_holdem, 6, 1000, 5, 10);
        pk_hand_begin(&g);
        g.seat[0].hole[0] = C("Ah"); g.seat[0].hole[1] = C("Ad");
        six_way = pk_ai_equity_game(&ai, &g, 0, 3000);

        check(heads_up > six_way,
            "aces are worth less against five opponents than against one");
        check_range(heads_up, 800, 880, "heads-up aces are about 85%");
        /* Aces against FIVE random hands is about 49%, not the ~35%
         * this test originally asserted. The estimator was right and
         * the expectation was wrong: each additional opponent costs
         * less than the one before, because they are increasingly
         * likely to beat each other rather than the aces. */
        check_range(six_way, 440, 550, "six-handed aces are about 49%");
    }

    /* In stud the opponents' exposed cards are real information. A
     * seat showing three kings should terrify the estimator; the same
     * seat showing three rags should not. */
    {
        int32_t vs_scary, vs_rags;

        pk_game_init(&g, &pk_variant_stud7, 2, 1000, 5, 10);
        pk_hand_begin(&g);

        g.seat[0].nhole = 4;
        g.seat[0].hole[0] = C("9c"); g.seat[0].up[0] = false;
        g.seat[0].hole[1] = C("9d"); g.seat[0].up[1] = false;
        g.seat[0].hole[2] = C("4h"); g.seat[0].up[2] = true;
        g.seat[0].hole[3] = C("7s"); g.seat[0].up[3] = true;

        g.seat[1].nhole = 4;
        g.seat[1].hole[0] = C("2c"); g.seat[1].up[0] = false;
        g.seat[1].hole[1] = C("3d"); g.seat[1].up[1] = false;
        g.seat[1].hole[2] = C("Kh"); g.seat[1].up[2] = true;
        g.seat[1].hole[3] = C("Ks"); g.seat[1].up[3] = true;
        vs_scary = pk_ai_equity_game(&ai, &g, 0, 6000);

        g.seat[1].hole[2] = C("8h"); g.seat[1].hole[3] = C("Ts");
        vs_rags = pk_ai_equity_game(&ai, &g, 0, 6000);

        /* A MARGIN, not a strict inequality.
         *
         * The first version of this asserted only vs_rags > vs_scary,
         * and an estimator mutated to ignore exposed cards entirely
         * PASSED it -- because that mutation makes the two numbers the
         * same, and `>` on two equal-but-noisy estimates is a coin
         * flip rather than a test. The real gap is about 320 points
         * and repeatable to within five, so requiring 200 turns a
         * 50/50 into a certainty.
         *
         * Any comparison between two measured quantities needs a
         * margin wider than the noise and narrower than the effect.
         * Without one it passes half the time on a broken build. */
        check(vs_rags > vs_scary + 200,
            "exposed kings are read as a threat and rags are not");
        check_range(vs_scary, 230, 350, "exposed kings are bad news");
        check_range(vs_rags, 530, 670, "exposed rags are not");
    }

    eq = pk_ai_equity_game(&ai, &g, 0, 100);
    check(eq >= 0 && eq <= 1000, "equity is always a probability");
}

/* -- legality -------------------------------------------------------- */

static void play_out(const pk_variant_t *v, int nseats, int limit,
    int level, int hands, int *illegal, int *stuck)
{
    pk_game_t g;
    pk_ai_t ai[PK_MAX_SEATS];
    int h, i;

    pk_ai_init(&ai[0], level);
    for (i = 0; i < nseats; i++) pk_ai_init(&ai[i], level);

    pk_game_init(&g, v, nseats, 1000, 5, 10);
    pk_game_set_limit(&g, limit);

    for (h = 0; h < hands; h++) {
        int guard = 0;

        if (pk_live_count(&g) < 2) {
            pk_game_init(&g, v, nseats, 1000, 5, 10);
            pk_game_set_limit(&g, limit);
        }

        pk_hand_begin(&g);

        while (g.phase != PK_PHASE_COMPLETE && guard++ < 400) {
            int action;
            int32_t to;
            int who = g.actor;

            if (g.phase == PK_PHASE_DRAW) {
                uint8_t idx[2] = { 0, 1 };
                pk_draw(&g, idx, (int)zg_rng_below(3));
                continue;
            }

            pk_ai_decide(&ai[who], &g, who, &action, &to);

            if (!pk_legal(&g, who, action, to)) { (*illegal)++; break; }

            for (i = 0; i < nseats; i++) pk_ai_observe(&ai[i], &g, who, action);

            if (!pk_act(&g, action, to)) { (*illegal)++; break; }
        }

        if (g.phase != PK_PHASE_COMPLETE) { (*stuck)++; break; }
    }
}

static void test_legality(void)
{
    struct { const pk_variant_t *v; int seats; int limit; const char *label; }
    cases[] = {
        { &pk_variant_holdem, 6, PK_LIMIT_NONE,  "holdem no-limit" },
        { &pk_variant_holdem, 2, PK_LIMIT_NONE,  "holdem heads-up" },
        { &pk_variant_holdem, 4, PK_LIMIT_FIXED, "holdem fixed" },
        { &pk_variant_holdem, 3, PK_LIMIT_POT,   "holdem pot-limit" },
        { &pk_variant_draw5,  5, PK_LIMIT_FIXED, "five-card draw" },
        { &pk_variant_stud5,  5, PK_LIMIT_FIXED, "five-card stud" },
        { &pk_variant_stud7,  7, PK_LIMIT_FIXED, "seven-card stud" }
    };
    int n = (int)(sizeof cases / sizeof cases[0]);
    int i, level;
    int illegal = 0, stuck = 0;

    zg_rng_seed(31337);

    /* Every level against every variant and every betting structure.
     *
     * The cheap levels play most of the hands, and that is a
     * deliberate trade rather than a compromise: WHICH ACTIONS ARE
     * LEGAL does not depend on how many rollouts went into choosing
     * one. A level 8 opponent runs fifty times the samples of a level
     * 1 opponent and reaches the same pk_options() gate. So the
     * expensive levels only need enough hands to cover every variant
     * once, while the cheap ones supply the volume.
     *
     * This test was 42 seconds before that reasoning was applied to
     * it, which is long enough that it would have started being
     * skipped. */
    for (i = 0; i < n; i++)
        for (level = PK_MIN_LEVEL; level <= PK_MAX_LEVEL; level++)
            play_out(cases[i].v, cases[i].seats, cases[i].limit, level,
                level <= 4 ? 20 : 3, &illegal, &stuck);

    check(illegal == 0, "no level ever chooses an illegal action");
    check(stuck == 0, "no level ever wedges a hand");
    if (illegal) printf("      %d illegal actions\n", illegal);
}

/* -- the ladder ------------------------------------------------------ */

static int32_t duel(int level_a, int level_b, int hands, uint32_t seed)
{
    pk_game_t g;
    pk_ai_t ai[2];
    int32_t net = 0;
    int h;

    zg_rng_seed(seed);
    pk_ai_init(&ai[0], level_a);
    pk_ai_init(&ai[1], level_b);

    for (h = 0; h < hands; h++) {
        int guard = 0;

        /* Equal stacks every hand, so the result is the sum of the
         * hands rather than a tournament in which one bad hand early
         * ends the measurement. */
        pk_game_init(&g, &pk_variant_holdem, 2, 1000, 5, 10);

        /* Alternate who has the button, so position cannot account
         * for the difference. */
        if (h & 1) g.button = 0; else g.button = 1;

        pk_hand_begin(&g);

        while (g.phase != PK_PHASE_COMPLETE && guard++ < 300) {
            int action, i;
            int32_t to;
            int who = g.actor;

            pk_ai_decide(&ai[who], &g, who, &action, &to);
            for (i = 0; i < 2; i++) pk_ai_observe(&ai[i], &g, who, action);
            if (!pk_act(&g, action, to)) break;
        }

        net += g.seat[0].stack - 1000;
    }

    return net;
}

static void test_ladder(void)
{
    int32_t strong_first, strong_second;

    /* A level 6 opponent against a level 1 opponent. If the ladder
     * means anything the strong one wins, and it must win from either
     * seat -- a result that only holds in one seat is measuring
     * position or blind order, not skill.
     *
     * 200 hands each way. Heads-up no-limit is high variance, so the
     * assertion is only that the sign is right, not that the margin
     * is any particular size. */
    strong_first = duel(6, 1, 200, 0xC0FFEE);
    strong_second = -duel(1, 6, 200, 0xBEEF);

    check(strong_first > 0, "level 6 beats level 1 from seat 0");
    check(strong_second > 0, "level 6 beats level 1 from seat 1");

    printf("      level 6 vs level 1: %+d and %+d chips over 200 hands\n",
        (int)strong_first, (int)strong_second);
}

/* -- aborting -------------------------------------------------------- */

static int poll_calls = 0;

static bool poll_stop(void *ctx)
{
    (void)ctx;
    poll_calls++;
    return false;      /* abort immediately */
}

static bool poll_go(void *ctx)
{
    (void)ctx;
    poll_calls++;
    return true;
}

static void test_abort(void)
{
    pk_game_t g;
    pk_ai_t ai;
    int action;
    int32_t to;

    pk_ai_init(&ai, 8);
    pk_game_init(&g, &pk_variant_holdem, 3, 1000, 5, 10);
    pk_hand_begin(&g);

    poll_calls = 0;
    pk_ai_set_poll(poll_go, NULL);
    (void)pk_ai_equity_game(&ai, &g, g.actor, 3000);
    check(poll_calls > 0, "the poll callback is actually called");
    check(!ai.aborted_last, "a poll that says carry on is not an abort");
    check(ai.rollouts_last == 3000, "and every rollout is taken");

    poll_calls = 0;
    pk_ai_set_poll(poll_stop, NULL);
    (void)pk_ai_equity_game(&ai, &g, g.actor, 30000);
    check(ai.aborted_last, "a poll that says stop aborts the estimate");
    check(ai.rollouts_last < 30000, "and stops early");
    check(ai.rollouts_last > 0, "but keeps the samples it took");

    /* The important half: an aborted estimate must still produce a
     * legal action rather than a failure the caller has to handle. */
    pk_ai_decide(&ai, &g, g.actor, &action, &to);
    check(pk_legal(&g, g.actor, action, to),
        "an aborted decision is still a legal one");

    pk_ai_set_poll(NULL, NULL);
}

/* -- servicing the message queue --------------------------------------
 *
 * THE DESKTOP-FREEZE REGRESSION TEST.
 *
 * wm blocks in wait_for_redraw_done() after it repairs a region, and
 * its chrome pass has already cleared the window's content by then. An
 * opponent that thinks for a second without pumping therefore freezes
 * every other window on screen AND leaves this one blank until it
 * finally answers.
 *
 * The first version of pk_ai_equity_game() polled inside two nested
 * guards, `(r & 63) == 0` around `(r & 255) == 0`, which at the
 * default level's 450 rollouts fired exactly ONCE per decision and at
 * levels 1 to 3 never fired at all. Six-handed, five opponents act
 * before the person does, so the app serviced about five messages in
 * the several seconds after startup. It shipped with a permanently
 * blank window and wm timing out on every redraw.
 *
 * Nothing else here could have caught it. Every other test in this
 * file installs no poll callback at all, or installs one and only asks
 * whether it was called -- never how often. So this asserts a RATE.
 */
static int rate_polls;

static bool poll_counting(void *ctx)
{
    (void)ctx;
    rate_polls++;
    return true;
}

static void test_poll_rate(void)
{
    int level;

    pk_ai_set_poll(poll_counting, NULL);

    for (level = PK_MIN_LEVEL; level <= PK_MAX_LEVEL; level++) {
        pk_game_t g;
        pk_ai_t a;
        long rollouts = 0;
        int decisions = 0, i;

        zg_rng_seed(1234 + level);
        pk_ai_init(&a, level);
        pk_game_init(&g, &pk_variant_holdem, 6, 1000, 5, 10);
        pk_hand_begin(&g);

        rate_polls = 0;

        for (i = 0; i < 12 && g.phase == PK_PHASE_BETTING; i++) {
            int action;
            int32_t to;
            pk_ai_decide(&a, &g, g.actor, &action, &to);
            rollouts += a.rollouts_last;
            decisions++;
            if (!pk_act(&g, action, to)) break;
        }

        /* Every decision must service the queue at least once, INCLUDING
         * the ones the weak levels answer without sampling at all. */
        check(rate_polls >= decisions,
            "every decision services the message queue at least once");

        /* And often enough during the sampling itself. A rollout at a
         * six-handed table is well under a millisecond, so one poll per
         * thirty-two of them is comfortably inside any redraw timeout. */
        /* Guarded, because zero polls IS the failure being tested for
         * and dividing by it turns a clear assertion failure into a
         * crash with no message. The first version of this test did
         * exactly that when run against the bug it was written to
         * catch, which is a poor way to find out. */
        if (rollouts > 0 && rate_polls > 0)
            check(rollouts / rate_polls <= 32,
                "and often enough while it is sampling");

        if (rate_polls < decisions || rate_polls == 0 ||
            (rollouts > 0 && rollouts / rate_polls > 32))
            printf("      level %d: %d polls, %ld rollouts, %d decisions\n",
                level, rate_polls, rollouts, decisions);
    }

    pk_ai_set_poll(NULL, NULL);
}

/* -- levels ---------------------------------------------------------- */

static void test_levels(void)
{
    pk_ai_t ai;
    int i;

    for (i = PK_MIN_LEVEL; i <= PK_MAX_LEVEL; i++) {
        pk_ai_init(&ai, i);
        check(ai.level == i, "a level is stored as given");
        check(pk_level_name(i) != NULL && pk_level_name(i)[0] != '\0',
            "every level has a name");
    }

    pk_ai_init(&ai, 0);
    check(ai.level == PK_MIN_LEVEL, "level 0 clamps up");
    pk_ai_init(&ai, 99);
    check(ai.level == PK_MAX_LEVEL, "level 99 clamps down");

    /* The read must survive a long session without wrapping into
     * nonsense. */
    {
        pk_game_t g;
        long n;
        pk_game_init(&g, &pk_variant_holdem, 2, 1000, 5, 10);
        pk_ai_init(&ai, 8);
        for (n = 0; n < 200000; n++) pk_ai_observe(&ai, &g, 1, PK_FOLD);
        check(ai.read[1].folds * 2 >= ai.read[1].actions,
            "a saturating read keeps its ratio");
        check(ai.read[1].actions > 0, "and does not wrap to zero");
    }
}

int main(void)
{
    printf("poker: opponent tests\n");

    test_levels();
    test_equity_decided();
    test_equity_known();
    test_equity_monotone();
    test_equity_in_game();
    test_abort();
    test_poll_rate();
    test_legality();
    test_ladder();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
