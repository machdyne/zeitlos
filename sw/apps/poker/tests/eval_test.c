/*
 * Zeitlos poker -- host tests for the evaluator and the deck.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Native cc, no RISC-V toolchain, and the SHIPPED sources. There is no
 * second copy of the evaluator anywhere in this tree.
 *
 * -- what these tests actually prove --
 *
 * The centrepiece is an exhaustive walk of all 2,598,960 five-card
 * hands, checked three ways at once:
 *
 *   1. The number of hands in each category must equal its published
 *      combinatorial value -- 40 straight flushes, 624 quads, and so
 *      on. Those are facts about a deck of cards, not about anybody's
 *      implementation, which is what makes them usable in a clean-room
 *      project. They are also extremely unforgiving: an evaluator that
 *      mishandles the wheel misses exactly 4 straight flushes and 1020
 *      straights and gains them back as flushes and high cards, and
 *      four of the nine counts move at once.
 *
 *   2. The number of DISTINCT values in each category must equal its
 *      published value, summing to 7,462. Count (1) says the category
 *      boundaries are right. This says the TIEBREAKS are right: an
 *      evaluator that forgot the kicker on a pair would still classify
 *      all 1,098,240 pairs correctly and would collapse 2,860 distinct
 *      values into 13.
 *
 *   3. Every hand's category must agree with a second classifier,
 *      written in this file, that shares no code and no approach with
 *      the one being tested. pk_eval5() builds a rank histogram;
 *      ref_category() sorts and looks at runs. Two implementations
 *      agreeing on 2.6 million inputs is a much stronger statement
 *      than either one passing a list of cases somebody thought of.
 *
 * That is the same three-way shape sw/apps/chess uses for perft:
 * published counts, plus a second generator written in the test.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "../pk_eval.h"
#include "../pk_deck.h"

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

/* -- a hand from a string, for readable cases ------------------------ */

static void hand(const char *s, uint8_t *out, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        out[i] = pk_card_parse(s + 3 * i);
        if (out[i] == PK_CARD_NONE) {
            printf("FAIL: unparseable card in \"%s\"\n", s);
            exit(1);
        }
    }
}

static uint32_t ev(const char *s)
{
    uint8_t h[5];
    hand(s, h, 5);
    return pk_eval5(h);
}

/* -- the second classifier ------------------------------------------
 *
 * Deliberately not the same algorithm. pk_eval5() counts ranks into a
 * 13-entry histogram and reads multiplicities out of it. This sorts
 * the five ranks and looks at runs of equal neighbours, which is how
 * somebody would do it by hand.
 *
 * Category only. Tiebreaks are covered by the distinct-value counts,
 * which do not need a second implementation to check.
 */
static int ref_category(const uint8_t *h)
{
    int r[5], i, j, t;
    int run[5], nrun = 0;
    bool flush = true, straight;

    for (i = 0; i < 5; i++) r[i] = PK_RANK(h[i]);

    for (i = 1; i < 5; i++)
        if (PK_SUIT(h[i]) != PK_SUIT(h[0])) flush = false;

    /* Insertion sort, ascending. */
    for (i = 1; i < 5; i++) {
        t = r[i];
        for (j = i - 1; j >= 0 && r[j] > t; j--) r[j + 1] = r[j];
        r[j + 1] = t;
    }

    /* Run lengths over the sorted ranks. */
    for (i = 0; i < 5; ) {
        j = i;
        while (j < 5 && r[j] == r[i]) j++;
        run[nrun++] = j - i;
        i = j;
    }

    /* Sort the run lengths descending -- at most four of them. */
    for (i = 1; i < nrun; i++) {
        t = run[i];
        for (j = i - 1; j >= 0 && run[j] < t; j--) run[j + 1] = run[j];
        run[j + 1] = t;
    }

    straight = (nrun == 5) && (r[4] - r[0] == 4);

    /* The wheel, spelled out literally rather than derived. */
    if (nrun == 5 && r[0] == PK_RANK_2 && r[1] == PK_RANK_2 + 1 &&
        r[2] == PK_RANK_2 + 2 && r[3] == PK_RANK_5 && r[4] == PK_RANK_A)
        straight = true;

    if (straight && flush) return PK_STRAIGHT_FLUSH;
    if (run[0] == 4) return PK_QUADS;
    if (run[0] == 3 && run[1] == 2) return PK_FULL_HOUSE;
    if (flush) return PK_FLUSH;
    if (straight) return PK_STRAIGHT;
    if (run[0] == 3) return PK_TRIPS;
    if (run[0] == 2 && run[1] == 2) return PK_TWO_PAIR;
    if (run[0] == 2) return PK_PAIR;
    return PK_HIGH_CARD;
}

/* -- 1, 2, 3: the exhaustive walk ------------------------------------ */

static const long want_count[PK_NCATEGORIES] = {
    1302540,  /* high card  */
    1098240,  /* pair       */
     123552,  /* two pair   */
      54912,  /* trips      */
      10200,  /* straight   */
       5108,  /* flush      */
       3744,  /* full house */
        624,  /* quads      */
         40   /* str flush  */
};

static const long want_distinct[PK_NCATEGORIES] = {
    1277, 2860, 858, 858, 10, 1277, 156, 156, 10
};

static const char *const catname[PK_NCATEGORIES] = {
    "high card", "pair", "two pair", "trips", "straight",
    "flush", "full house", "quads", "straight flush"
};

static void test_exhaustive(void)
{
    long count[PK_NCATEGORIES];
    long distinct[PK_NCATEGORIES];
    unsigned char *seen;
    uint8_t h[5];
    long total = 0, disagree = 0, sum_count = 0, sum_distinct = 0;
    int a, b, c, d, e, i;

    for (i = 0; i < PK_NCATEGORIES; i++) count[i] = distinct[i] = 0;

    /* One bit per possible encoded value. The encoding is 24 bits, so
     * this is 2MB -- irrelevant on a build machine, and it makes the
     * distinct count a single pass rather than a sort of 2.6 million
     * integers. */
    seen = calloc(1u << 24, 1);
    if (!seen) { printf("FAIL: out of memory\n"); exit(1); }

    for (a = 0; a < PK_NCARDS; a++)
    for (b = a + 1; b < PK_NCARDS; b++)
    for (c = b + 1; c < PK_NCARDS; c++)
    for (d = c + 1; d < PK_NCARDS; d++)
    for (e = d + 1; e < PK_NCARDS; e++) {

        uint32_t v;
        int cat;

        h[0] = a; h[1] = b; h[2] = c; h[3] = d; h[4] = e;

        v = pk_eval5(h);
        cat = PK_CATEGORY(v);

        if (cat < 0 || cat >= PK_NCATEGORIES) {
            printf("FAIL: category %d out of range\n", cat);
            exit(1);
        }

        if (cat != ref_category(h)) disagree++;

        count[cat]++;
        total++;

        if (!seen[v]) { seen[v] = 1; distinct[cat]++; }
    }

    free(seen);

    check_eq(total, 2598960, "every five-card hand visited");
    check_eq(disagree, 0, "second classifier agrees on every hand");

    for (i = 0; i < PK_NCATEGORIES; i++) {
        char msg[96];

        snprintf(msg, sizeof msg, "%s: hand count", catname[i]);
        check_eq(count[i], want_count[i], msg);
        sum_count += count[i];

        snprintf(msg, sizeof msg, "%s: distinct values", catname[i]);
        check_eq(distinct[i], want_distinct[i], msg);
        sum_distinct += distinct[i];
    }

    check_eq(sum_count, 2598960, "category counts sum to C(52,5)");
    check_eq(sum_distinct, 7462, "distinct values sum to 7462");
}

/* -- ordering -------------------------------------------------------- */

static void test_ordering(void)
{
    /* A ladder, worst to best. Each must beat everything below it, so
     * this is C(n,2) comparisons rather than n - 1: an evaluator can
     * get every adjacent pair right and still be non-transitive. */
    static const char *const ladder[] = {
        "7h 5d 4c 3s 2h",   /* the worst possible hand          */
        "Ah Kd Qc Js 9h",   /* ace high, one short of a straight */
        "2h 2d 4c 5s 7h",   /* pair of deuces                    */
        "2h 2d 4c 5s 8h",   /* same pair, better kicker          */
        "Ah Ad 4c 5s 7h",   /* pair of aces                      */
        "2h 2d 3c 3s 7h",   /* two pair                          */
        "Ah Ad Kc Ks 7h",   /* aces up                           */
        "2h 2d 2c 5s 7h",   /* trips                             */
        "Ah 2d 3c 4s 5h",   /* the wheel                         */
        "2h 3d 4c 5s 6h",   /* six-high straight                 */
        "Ah Kd Qc Js Th",   /* broadway                          */
        "2h 4h 6h 8h Th",   /* flush                             */
        "Ah Kh Qh Jh 9h",   /* ace-high flush                    */
        "2h 2d 2c 3s 3h",   /* full house                        */
        "Ah Ad Ac 2s 2h",   /* aces full                         */
        "2h 2d 2c 2s 7h",   /* quads                             */
        "Ah Ad Ac As 7h",   /* quad aces                         */
        "Ah 2h 3h 4h 5h",   /* steel wheel                       */
        "2h 3h 4h 5h 6h",   /* six-high straight flush           */
        "Ah Kh Qh Jh Th"    /* royal                             */
    };
    int n = (int)(sizeof ladder / sizeof ladder[0]);
    int i, j, bad = 0;

    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (!(ev(ladder[i]) < ev(ladder[j]))) {
                printf("FAIL: \"%s\" should rank below \"%s\"\n",
                    ladder[i], ladder[j]);
                bad++;
            }

    check_eq(bad, 0, "the ranking ladder is a strict total order");

    /* The wheel is the LOWEST straight, and specifically ranks below
     * a six-high straight rather than above broadway. An ace-high
     * reading of it is the single most likely evaluator bug and would
     * pass every category count on its own. */
    check(ev("Ah 2d 3c 4s 5h") < ev("2h 3d 4c 5s 6h"),
        "the wheel is the lowest straight");
    check(ev("Ah 2h 3h 4h 5h") < ev("2h 3h 4h 5h 6h"),
        "the steel wheel is the lowest straight flush");
    check(PK_CATEGORY(ev("Ah 2d 3c 4s 5h")) == PK_STRAIGHT,
        "the wheel is a straight at all");

    /* And there is no straight that turns the corner. */
    check(PK_CATEGORY(ev("Qh Kd Ac 2s 3h")) == PK_HIGH_CARD,
        "Q-K-A-2-3 is not a straight");

    /* Suits never break a tie. The same hand in two suits must be
     * exactly equal, or every split pot in the game is wrong. */
    check(ev("Ah Kh Qh Jh 9h") == ev("As Ks Qs Js 9s"),
        "suits do not break ties");
    check(ev("2h 2d 7c 5s 9h") == ev("2s 2c 7h 5d 9s"),
        "suits do not break ties for a pair");
}

/* -- best of seven --------------------------------------------------- */

static void test_best(void)
{
    uint8_t seven[7], five[5], best[5];
    int iter, bad_val = 0, bad_cards = 0;
    int a, b, c, d, e, i;

    /* Five cards in, and pk_eval_best() must agree with pk_eval5()
     * exactly. The one-subset case is easy to break when the loop is
     * written for seven. */
    for (iter = 0; iter < 20000; iter++) {
        pk_deck_t dk;
        pk_deck_init(&dk);
        pk_deck_shuffle(&dk);
        for (i = 0; i < 5; i++) five[i] = pk_deck_deal(&dk);
        if (pk_eval_best(five, 5, best) != pk_eval5(five)) bad_val++;
    }
    check_eq(bad_val, 0, "best-of-five equals eval5");

    /* Seven cards, against a brute force written here. pk_eval_best()
     * enumerates with an index-set walker; this nests five loops. */
    bad_val = 0;
    for (iter = 0; iter < 200000; iter++) {
        pk_deck_t dk;
        uint32_t got, want = PK_EVAL_NONE;

        pk_deck_init(&dk);
        pk_deck_shuffle(&dk);
        for (i = 0; i < 7; i++) seven[i] = pk_deck_deal(&dk);

        for (a = 0; a < 7; a++)
        for (b = a + 1; b < 7; b++)
        for (c = b + 1; c < 7; c++)
        for (d = c + 1; d < 7; d++)
        for (e = d + 1; e < 7; e++) {
            uint32_t v;
            five[0] = seven[a]; five[1] = seven[b]; five[2] = seven[c];
            five[3] = seven[d]; five[4] = seven[e];
            v = pk_eval5(five);
            if (v > want) want = v;
        }

        got = pk_eval_best(seven, 7, best);
        if (got != want) bad_val++;

        /* The five cards reported must be a subset of the seven and
         * must actually evaluate to the winning value. A best-of-N
         * that returns the right number and the wrong cards draws the
         * wrong highlight at showdown, which no value check catches. */
        if (pk_eval5(best) != got) bad_cards++;
        else {
            for (i = 0; i < 5; i++) {
                int j, found = 0;
                for (j = 0; j < 7; j++) if (seven[j] == best[i]) found = 1;
                if (!found) { bad_cards++; break; }
            }
        }
    }

    check_eq(bad_val, 0, "best-of-seven matches brute force");
    check_eq(bad_cards, 0, "best-of-seven reports a valid winning five");

    /* Six cards, the seven-stud case where somebody is all in early. */
    check(pk_eval_best(seven, 6, best) != PK_EVAL_NONE,
        "six cards evaluate");

    check_eq(pk_eval_best(seven, 4, best), PK_EVAL_NONE,
        "four cards refuse rather than read past the end");
    check_eq(pk_eval_best(seven, 8, best), PK_EVAL_NONE,
        "eight cards refuse");
}

/* -- the Omaha constraint -------------------------------------------- */

static void test_constrained(void)
{
    uint8_t hole[4], board[5], best[5];
    uint32_t free_val, omaha_val;

    /* Four hearts on board and one in hand. Hold'em says flush;
     * Omaha says it is not, because exactly two hole cards must play
     * and the second one is a club. This is THE Omaha mistake and it
     * is invisible unless the constraint is enforced. */
    hand("Ah Kc Qc Jc", hole, 4);
    hand("2h 5h 9h Th 3s", board, 5);

    free_val = pk_eval_best((uint8_t[]){ hole[0], hole[1], board[0],
        board[1], board[2], board[3], board[4] }, 7, best);
    omaha_val = pk_eval_constrained(hole, 4, board, 5, 2, best);

    check(PK_CATEGORY(free_val) == PK_FLUSH,
        "unconstrained, one hearts hole card makes the flush");
    check(PK_CATEGORY(omaha_val) != PK_FLUSH,
        "constrained to two hole cards, it does not");

    /* Two hearts in hand and three on board is a real Omaha flush. */
    hand("Ah Kh Qc Jc", hole, 4);
    check(PK_CATEGORY(pk_eval_constrained(hole, 4, board, 5, 2, best))
        == PK_FLUSH, "two hearts in hand does make the flush");

    /* use_hole == 0 is the board playing by itself, which is a legal
     * hold'em outcome even though no variant here asks for it. */
    check(pk_eval_constrained(hole, 4, board, 5, 0, best) ==
        pk_eval5(board), "zero hole cards is the board alone");

    check_eq(pk_eval_constrained(hole, 1, board, 5, 2, best),
        PK_EVAL_NONE, "not enough hole cards refuses");
    check_eq(pk_eval_constrained(hole, 4, board, 2, 2, best),
        PK_EVAL_NONE, "not enough board refuses");
}

/* -- naming ---------------------------------------------------------- */

static void test_names(void)
{
    struct { const char *h; const char *want; } cases[] = {
        { "Ah Kh Qh Jh Th", "royal flush"             },
        { "9h 8h 7h 6h 5h", "nine-high straight flush" },
        { "Ah 2h 3h 4h 5h", "five-high straight flush" },
        { "Ah Ad Ac As 7h", "four aces"               },
        { "Ah Ad Ac Ks Kh", "aces full of kings"      },
        { "Ah Kh Qh Jh 9h", "ace-high flush"          },
        { "Ah 2d 3c 4s 5h", "five-high straight"      },
        { "7h 7d 7c 5s 2h", "three sevens"            },
        { "Ah Ad Kc Ks 7h", "aces and kings"          },
        { "2h 2d 7c 5s 9h", "pair of deuces"          },
        { "Ah Kd Qc Js 9h", "ace high"                }
    };
    int i, n = (int)(sizeof cases / sizeof cases[0]);
    char buf[40];

    for (i = 0; i < n; i++) {
        pk_eval_name(ev(cases[i].h), buf, sizeof buf);
        if (strcmp(buf, cases[i].want) != 0) {
            printf("FAIL: \"%s\" named \"%s\", want \"%s\"\n",
                cases[i].h, buf, cases[i].want);
            failures++;
        }
        checks++;
    }

    /* Truncation must still terminate. The message line is short and
     * "aces full of kings" is not the longest thing this can say. */
    pk_eval_name(ev("Ah Ad Ac Ks Kh"), buf, 6);
    check(strlen(buf) == 5, "a short buffer truncates and terminates");

    pk_eval_name(PK_EVAL_NONE, buf, sizeof buf);
    check(strcmp(buf, "no hand") == 0, "PK_EVAL_NONE has a name");
}

/* -- cards ----------------------------------------------------------- */

static void test_cards(void)
{
    int i, bad = 0;
    char buf[4];

    /* Every card survives a round trip through its own name. */
    for (i = 0; i < PK_NCARDS; i++) {
        pk_card_str((uint8_t)i, buf);
        if (pk_card_parse(buf) != (uint8_t)i) bad++;
    }
    check_eq(bad, 0, "every card round-trips through its name");

    check_eq(pk_card_parse("AH"), pk_card_parse("ah"),
        "card parsing is case-insensitive");
    check_eq(pk_card_parse("Xh"), PK_CARD_NONE, "a bad rank refuses");
    check_eq(pk_card_parse("Ax"), PK_CARD_NONE, "a bad suit refuses");
    check_eq(pk_card_parse("A"), PK_CARD_NONE, "a short string refuses");

    pk_card_str(PK_CARD_NONE, buf);
    check(strcmp(buf, "--") == 0, "PK_CARD_NONE has a name");

    /* The encoding claim from pk_cards.h: card order is rank order. */
    check(PK_CARD(PK_RANK_A, PK_CLUBS) > PK_CARD(PK_RANK_K, PK_SPADES),
        "a higher rank is a higher card byte regardless of suit");
}

/* -- the deck -------------------------------------------------------- */

static uint32_t seq_rng(void *ctx)
{
    /* Not random at all -- it walks a counter. Installed only to prove
     * that the injection point works and that the shuffle consumes the
     * generator rather than reaching around it. */
    uint32_t *p = (uint32_t *)ctx;
    return (*p)++;
}

/* -- the rejection boundary -------------------------------------------
 *
 * A source that hands out whatever the test tells it to, and counts
 * how many words each call to pk_rng_below() consumed. That count is
 * the whole point: it is the only way to observe the accept region
 * from outside, and the accept region is where both of the bugs this
 * replaced lived.
 */
static uint32_t feed_vals[8];
static int feed_n, feed_i, feed_used;

static uint32_t feed_rng(void *ctx)
{
    (void)ctx;
    feed_used++;

    /* A BROKEN ACCEPT BOUND DOES NOT RETURN, IT SPINS. So the source
     * itself has to stop it, or this test hangs instead of failing --
     * which is worse than useless: a hung test looks like a slow
     * machine and gets killed, while a failing one names the problem.
     * Verified by reinstating the old bound and watching this fire. */
    if (feed_used > 1000) {
        printf("FAIL: pk_rng_below() did not terminate -- %d draws "
            "and still rejecting\n", feed_used);
        exit(1);
    }

    if (feed_i >= feed_n) return 0;
    return feed_vals[feed_i++];
}

static void feed(uint32_t a, uint32_t b)
{
    feed_vals[0] = a;
    feed_vals[1] = b;
    feed_n = 2;
    feed_i = 0;
    feed_used = 0;
}

static void test_rng_bounds(void)
{
    uint32_t n;
    int worst = 0;

    /* TERMINATION, for every bound a deck can ask for and then some.
     *
     * pk_rng_below() used to delegate to sw/common/zrng.c's
     * z_rng_below(), whose accept bound is 2^32 - (2^32 % n). For a
     * power-of-two n that is 2^32, which truncates to 0 in a uint32_t,
     * and `while (v >= 0)` on an unsigned never ends. A 52-card
     * Fisher-Yates walks n from 52 down to 2, so it hits 32, 16, 8, 4
     * and 2 -- every shuffle, guaranteed, on the first deal.
     *
     * Counting the words consumed is what makes this a test rather
     * than a hang: a broken bound spins forever, so the assertion has
     * to be on the COST, with a source that cannot run out. */
    pk_rng_set(feed_rng, NULL);

    for (n = 1; n <= 300; n++) {
        uint32_t v;
        feed_vals[0] = 0xffffffffu;
        feed_vals[1] = 0xfffffffeu;
        feed_n = 2;
        feed_i = 0;
        feed_used = 0;
        v = pk_rng_below(n);
        if (feed_used > worst) worst = feed_used;
        if (v >= n && n > 0) {
            printf("FAIL: pk_rng_below(%u) returned %u\n", n, v);
            failures++;
        }
        checks++;
    }

    check(worst <= 8, "no bound needs more than a handful of draws");

    /* Powers of two specifically, which is where it hung. */
    for (n = 2; n <= (1u << 20); n <<= 1) {
        feed_vals[0] = 0xffffffffu;
        feed_n = 1;
        feed_i = 0;
        feed_used = 0;
        (void)pk_rng_below(n);
        check_eq(feed_used, 1,
            "a power-of-two bound rejects nothing and draws once");
    }

    /* THE BOUNDARY, exactly.
     *
     * 2^32 mod 3 is 1, so precisely one word -- 0xffffffff -- must be
     * thrown away, and the next one used instead. One more or one
     * fewer and the accept region stops being a multiple of n, which
     * is the definition of the bias this is here to prevent. */
    feed(0xffffffffu, 0u);
    check_eq(pk_rng_below(3), 0, "the top word is rejected for n = 3");
    check_eq(feed_used, 2, "and exactly one redraw was needed");

    feed(0xfffffffeu, 0u);
    check_eq(pk_rng_below(3), 0xfffffffeu % 3,
        "the one below it is accepted");
    check_eq(feed_used, 1, "with no redraw");

    /* n = 4 divides 2^32, so even the very top word is accepted. */
    feed(0xffffffffu, 0u);
    check_eq(pk_rng_below(4), 3, "nothing is rejected for n = 4");
    check_eq(feed_used, 1, "and one draw suffices");

    pk_rng_set(NULL, NULL);
}

static void test_deck(void)
{
    pk_deck_t d, e;
    int seen[PK_NCARDS];
    int i, bad;
    uint32_t ctr;
    uint8_t want[3];

    pk_deck_init(&d);
    check_eq(pk_deck_remaining(&d), 52, "a fresh deck holds 52");

    /* A shuffle is a permutation: every card exactly once, still. */
    pk_deck_shuffle(&d);
    for (i = 0; i < PK_NCARDS; i++) seen[i] = 0;
    for (i = 0; i < PK_NCARDS; i++) {
        uint8_t c = pk_deck_deal(&d);
        check(c < PK_NCARDS, "a dealt card is a real card");
        seen[c]++;
    }
    bad = 0;
    for (i = 0; i < PK_NCARDS; i++) if (seen[i] != 1) bad++;
    check_eq(bad, 0, "a shuffle is a permutation");

    check_eq(pk_deck_deal(&d), PK_CARD_NONE,
        "an exhausted deck reports it rather than wrapping");
    check_eq(pk_deck_remaining(&d), 0, "an exhausted deck has none left");

    /* Determinism, which is what every later betting test depends on. */
    ctr = 0;
    pk_rng_set(seq_rng, &ctr);
    pk_deck_init(&d);
    pk_deck_shuffle(&d);
    ctr = 0;
    pk_deck_init(&e);
    pk_deck_shuffle(&e);
    check(memcmp(d.card, e.card, PK_NCARDS) == 0,
        "the same generator state deals the same deck");
    pk_rng_set(NULL, NULL);

    /* Stacking. */
    pk_deck_init(&d);
    pk_deck_shuffle(&d);
    hand("Ah Kd 2c", want, 3);
    check(pk_deck_stack(&d, want, 3), "stacking three cards succeeds");
    check_eq(pk_deck_deal(&d), want[0], "the first stacked card comes off");
    check_eq(pk_deck_deal(&d), want[1], "the second does too");
    check_eq(pk_deck_deal(&d), want[2], "and the third");

    /* Still a full deck underneath. */
    check_eq(pk_deck_remaining(&d), 49, "stacking does not add cards");
    for (i = 0; i < PK_NCARDS; i++) seen[i] = 0;
    for (i = 0; i < 3; i++) seen[want[i]]++;
    while (pk_deck_remaining(&d)) seen[pk_deck_deal(&d)]++;
    bad = 0;
    for (i = 0; i < PK_NCARDS; i++) if (seen[i] != 1) bad++;
    check_eq(bad, 0, "stacking preserves the permutation");

    /* A stacked card that has already been dealt must refuse, and must
     * refuse without disturbing the deck. */
    pk_deck_init(&d);
    (void)pk_deck_deal(&d);
    want[0] = 0;
    check(!pk_deck_stack(&d, want, 1),
        "stacking an already-dealt card refuses");

    hand("Ah Ah", want, 2);
    pk_deck_init(&d);
    check(!pk_deck_stack(&d, want, 2), "stacking a duplicate refuses");
    check_eq(pk_deck_remaining(&d), 52, "a refused stack changes nothing");

    /* Exclusion, which is what the rollouts will use. */
    hand("Ah Kd 2c", want, 3);
    pk_deck_init_excluding(&d, want, 3);
    check_eq(pk_deck_remaining(&d), 49, "excluding three leaves 49");
    bad = 0;
    while (pk_deck_remaining(&d)) {
        uint8_t c = pk_deck_deal(&d);
        for (i = 0; i < 3; i++) if (c == want[i]) bad++;
    }
    check_eq(bad, 0, "an excluded card is never dealt");

    /* PK_CARD_NONE in the exclusion list is expected, not an error:
     * an opponent's unknown hole cards arrive that way. */
    want[0] = PK_CARD_NONE;
    pk_deck_init_excluding(&d, want, 3);
    check_eq(pk_deck_remaining(&d), 50,
        "PK_CARD_NONE excludes nothing and is not an error");
}

/* -- shuffle uniformity ---------------------------------------------- */

static void test_uniformity(void)
{
    /* Not a proof, and not trying to be. A chi-square over where one
     * card lands catches the two mistakes that actually happen: an
     * off-by-one in the Fisher-Yates bound, and a `% n` bias. Both
     * skew this badly enough to be obvious; neither is visible by
     * playing.
     *
     * 52 bins, 520,000 trials, so 10,000 expected per bin. The
     * threshold is loose on purpose -- this must not fail once a
     * month on a correct shuffle. */
    long bin[PK_NCARDS];
    const long trials = 520000;
    double expect = (double)trials / PK_NCARDS;
    double chi2 = 0.0;
    long t;
    int i;

    for (i = 0; i < PK_NCARDS; i++) bin[i] = 0;

    pk_rng_seed(12345);

    for (t = 0; t < trials; t++) {
        pk_deck_t d;
        pk_deck_init(&d);
        pk_deck_shuffle(&d);
        for (i = 0; i < PK_NCARDS; i++)
            if (d.card[i] == PK_CARD(PK_RANK_A, PK_SPADES)) { bin[i]++; break; }
    }

    for (i = 0; i < PK_NCARDS; i++) {
        double diff = (double)bin[i] - expect;
        chi2 += diff * diff / expect;
    }

    /* 51 degrees of freedom. The 0.999 critical value is about 86;
     * 120 leaves a wide margin over it. A biased shuffle scores in
     * the thousands. */
    check(chi2 < 120.0, "the ace of spades lands uniformly");
    if (chi2 >= 120.0) printf("      chi2 = %.1f over 51 df\n", chi2);
}

int main(void)
{
    printf("poker: evaluator and deck tests\n");

    test_cards();
    test_rng_bounds();
    test_deck();
    test_uniformity();
    test_ordering();
    test_names();
    test_constrained();
    test_best();
    printf("  (exhaustive walk of all 2,598,960 five-card hands)\n");
    test_exhaustive();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
