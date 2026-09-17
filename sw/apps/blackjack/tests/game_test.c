/*
 * Zeitlos blackjack -- host tests for the rules.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Almost nothing here can be checked by playing. A hand valued one too
 * many still plays; a natural paid even money still pays; a split ace
 * that draws again still looks like blackjack. All of them are wrong
 * every time and none announces itself.
 *
 * So: an exhaustive check of the totaller against a second
 * implementation, scripted hands with a stacked shoe asserting exact
 * chip counts, and a fuzz over thousands of rounds asserting the
 * invariants that must hold whatever happens.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../bj_game.h"
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

static uint8_t C(const char *s)
{
    uint8_t c = zcard_parse(s);
    if (c == Z_CARD_NONE) { printf("bad card %s\n", s); exit(1); }
    return c;
}

static void hand(const char *s, uint8_t *out, int n)
{
    int i;
    for (i = 0; i < n; i++) out[i] = C(s + 3 * i);
}

/* -- totalling ----------------------------------------------------------- */

/* A second implementation, written differently: every ace is one, then
 * try adding ten repeatedly and keep the best that fits. bj_total()
 * promotes exactly one ace with a single `if`; this searches. Two
 * approaches agreeing over every hand is a much stronger statement than
 * either passing a list of cases. */
static int ref_total(const uint8_t *cards, int n, bool *soft)
{
    int base = 0, aces = 0, i, best, k;

    for (i = 0; i < n; i++) {
        int r = Z_RANK(cards[i]);
        if (r == Z_RANK_A) { aces++; base += 1; }
        else if (r >= Z_RANK_T) base += 10;
        else base += r + 2;
    }

    best = base;
    if (soft) *soft = false;

    for (k = 1; k <= aces; k++) {
        int t = base + 10 * k;
        if (t <= 21 && t > best) { best = t; if (soft) *soft = true; }
    }

    return best;
}

static void test_totals(void)
{
    uint8_t c[5];
    int a, b, d, e, f, bad = 0;
    long n = 0;

    /* EVERY hand of two, three and four cards, over all 52 ranks and
     * suits, against the reference. */
    for (a = 0; a < Z_NCARDS; a++)
        for (b = 0; b < Z_NCARDS; b++) {
            bool s1 = false, s2 = false;
            c[0] = a; c[1] = b;
            if (bj_total(c, 2, &s1) != ref_total(c, 2, &s2) || s1 != s2) bad++;
            n++;
        }
    check_eq(bad, 0, "every two-card hand totals correctly");

    bad = 0;
    for (a = 0; a < Z_NCARDS; a += 1)
        for (b = 0; b < Z_NCARDS; b += 3)
            for (d = 0; d < Z_NCARDS; d += 5) {
                bool s1 = false, s2 = false;
                c[0] = a; c[1] = b; c[2] = d;
                if (bj_total(c, 3, &s1) != ref_total(c, 3, &s2) || s1 != s2)
                    bad++;
                n++;
            }
    check_eq(bad, 0, "and every three-card hand sampled");

    bad = 0;
    for (a = 0; a < Z_NCARDS; a += 7)
        for (b = 0; b < Z_NCARDS; b += 5)
            for (d = 0; d < Z_NCARDS; d += 3)
                for (e = 0; e < Z_NCARDS; e += 4)
                    for (f = 0; f < Z_NCARDS; f += 11) {
                        c[0] = a; c[1] = b; c[2] = d; c[3] = e; c[4] = f;
                        if (bj_total(c, 5, 0) != ref_total(c, 5, 0)) bad++;
                        n++;
                    }
    check_eq(bad, 0, "and every five-card hand sampled");
    check(n > 100000, "over a lot of hands");

    /* THE ACE RULE, spelled out. A loop that promotes every ace turns
     * these into 22, 33 and 44 -- hands that bust when they should be
     * perfectly good. */
    {
        uint8_t h[4];
        hand("Ah As", h, 2);
        check_eq(bj_total(h, 2, 0), 12, "two aces are twelve, not twenty-two");
        hand("Ah As Ad", h, 3);
        check_eq(bj_total(h, 3, 0), 13, "three aces are thirteen");
        hand("Ah As Ad Ac", h, 4);
        check_eq(bj_total(h, 4, 0), 14, "four aces are fourteen");
    }

    {
        uint8_t h[4];
        bool soft = false;
        hand("Ah 6s", h, 2);
        check_eq(bj_total(h, 2, &soft), 17, "ace-six is seventeen");
        check(soft, "and it is soft");
        hand("Ah 6s Th", h, 3);
        check_eq(bj_total(h, 3, &soft), 17, "with a ten it is still seventeen");
        check(!soft, "but now hard");

        hand("Th 7s", h, 2);
        check_eq(bj_total(h, 2, &soft), 17, "ten-seven is seventeen");
        check(!soft, "and hard");

        hand("Kh Qs 2d", h, 3);
        check_eq(bj_total(h, 3, 0), 22, "a bust total is reported as it is");
    }

    /* Card values: every face is ten, an ace is eleven before the
     * promotion, and the deuce is two -- rank 0 in the encoding. */
    check_eq(bj_card_value(C("2c")), 2, "a deuce is two");
    check_eq(bj_card_value(C("9c")), 9, "a nine is nine");
    check_eq(bj_card_value(C("Tc")), 10, "a ten is ten");
    check_eq(bj_card_value(C("Jc")), 10, "a jack is ten");
    check_eq(bj_card_value(C("Qc")), 10, "a queen is ten");
    check_eq(bj_card_value(C("Kc")), 10, "a king is ten");
    check_eq(bj_card_value(C("Ac")), 11, "an ace is eleven");
}

static void test_naturals(void)
{
    uint8_t h[3];

    hand("Ah Ks", h, 2);
    check(bj_is_natural(h, 2), "ace-king is a natural");
    hand("Th As", h, 2);
    check(bj_is_natural(h, 2), "and so is ten-ace");

    /* TWENTY-ONE IS NOT ALWAYS BLACKJACK. A three-card 21 pays even
     * money and loses the tie to a dealer's natural -- worth 50% of the
     * bet, so it is not a rounding detail. */
    hand("7h 7s 7d", h, 3);
    check_eq(bj_total(h, 3, 0), 21, "three sevens are twenty-one");
    check(!bj_is_natural(h, 3), "but not a natural");
}

/* -- the dealer ----------------------------------------------------------- */

static void test_dealer_rule(void)
{
    bj_rules_t r;
    uint8_t h[3];

    bj_rules_default(&r);
    r.dealer_hits_soft17 = false;

    hand("Th 6s", h, 2);
    check(bj_dealer_draws(&r, h, 2), "the dealer draws to sixteen");
    hand("Th 7s", h, 2);
    check(!bj_dealer_draws(&r, h, 2), "and stands on hard seventeen");

    /* SOFT SEVENTEEN is the whole reason bj_total() reports softness.
     * The two rules differ by about 0.2% of house edge and are
     * indistinguishable without it. */
    hand("Ah 6s", h, 2);
    check(!bj_dealer_draws(&r, h, 2), "stands on soft seventeen by default");
    r.dealer_hits_soft17 = true;
    check(bj_dealer_draws(&r, h, 2), "and hits it when the rule says so");

    hand("Ah 7s", h, 2);
    check(!bj_dealer_draws(&r, h, 2), "but never soft eighteen");
}

/* -- scripted rounds ------------------------------------------------------ */

static bj_game_t g;

/* Stacks the shoe so the deal comes out exactly as named. The order is
 * player, dealer, player, dealer -- reproduced here rather than
 * exposed, so if the dealing order ever changes these break loudly
 * instead of quietly following it. */
static void deal(const char *p1, const char *d1, const char *p2,
    const char *d2, const char *rest, int nrest)
{
    uint8_t order[16];
    int n = 0, i;

    order[n++] = C(p1);
    order[n++] = C(d1);
    order[n++] = C(p2);
    order[n++] = C(d2);
    for (i = 0; i < nrest; i++) order[n++] = C(rest + 3 * i);

    zdeck_init(&g.shoe, g.rules.ndecks);
    if (!zdeck_stack(&g.shoe, order, n)) {
        printf("FAIL: could not stack the shoe\n");
        exit(1);
    }
}

static void fresh(void)
{
    bj_rules_t r;
    bj_rules_default(&r);
    bj_game_init(&g, &r);
}

static void test_natural_pays(void)
{
    fresh();
    deal("Ah", "9s", "Kd", "7c", "", 0);
    check(bj_round_begin_stacked(&g, 10), "a round deals");
    check_eq(g.phase, BJ_PHASE_DONE, "a natural against no ace settles at once");
    check_eq(g.returned, 25, "and pays three to two, stake included");
    check_eq(g.returned - g.staked, 15, "for a profit of fifteen on ten");

    /* 6:5, the rule that matters most. Same hand, nearly half the
     * money. */
    {
        bj_rules_t r;
        bj_rules_default(&r);
        r.bj_pay_num = 6;
        r.bj_pay_den = 5;
        bj_game_init(&g, &r);
        deal("Ah", "9s", "Kd", "7c", "", 0);
        bj_round_begin_stacked(&g, 10);
        check_eq(g.returned, 22, "six to five pays twelve on ten");
    }

    /* Both naturals is a push. */
    fresh();
    deal("Ah", "As", "Kd", "Qc", "", 0);
    bj_round_begin_stacked(&g, 10);
    bj_insure(&g, 0);
    check_eq(g.phase, BJ_PHASE_DONE, "two naturals settle at once");
    check_eq(g.returned, 10, "and push");

    /* Dealer natural alone takes the bet. */
    fresh();
    deal("Th", "As", "9d", "Qc", "", 0);
    bj_round_begin_stacked(&g, 10);
    bj_insure(&g, 0);
    check_eq(g.returned, 0, "a dealer natural takes the bet");
}

static void test_insurance(void)
{
    fresh();
    deal("Th", "As", "9d", "Qc", "", 0);
    bj_round_begin_stacked(&g, 10);
    check_eq(g.phase, BJ_PHASE_INSURANCE, "an ace up offers insurance");

    check(!bj_insure(&g, 6), "insurance is capped at half the bet");
    check(bj_insure(&g, 5), "and five on ten is fine");

    /* 2:1 on the side bet exactly covers the main bet lost. */
    check_eq(g.returned, 15, "insurance pays two to one");
    check_eq(g.staked, 15, "against a stake of fifteen");
    check_eq(g.returned - g.staked, 0, "so the round is a wash");

    /* And when the dealer has no natural, the insurance is simply
     * lost. */
    fresh();
    deal("Th", "As", "9d", "7c", "", 0);
    bj_round_begin_stacked(&g, 10);
    bj_insure(&g, 5);
    check_eq(g.phase, BJ_PHASE_PLAYER, "no natural, so play continues");
    check_eq(g.insurance, 5, "with the side bet standing");
    bj_act(&g, BJ_STAND);
    check_eq(g.returned, 20, "nineteen beats seventeen -- but only the hand");
    check_eq(g.staked, 15, "and the insurance is gone");

    fresh();
    deal("Th", "9s", "9d", "7c", "", 0);
    bj_round_begin_stacked(&g, 10);
    check_eq(g.phase, BJ_PHASE_PLAYER, "no ace up, no insurance offered");
    check(!bj_insure(&g, 5), "and it cannot be taken");
}

static void test_split(void)
{
    bj_options_t o;

    /* Splitting eights against a dealer's ten -- the most famous
     * decision in the game. */
    fresh();
    deal("8h", "Ts", "8d", "7c", "3h 2s 5d", 3);
    bj_round_begin_stacked(&g, 10);
    bj_options(&g, &o);
    check(o.can_split, "a pair can be split");
    check(bj_act(&g, BJ_SPLIT), "and splitting works");
    check_eq(g.nhands, 2, "making two hands");
    check_eq(g.staked, 20, "for twice the stake");
    check_eq(g.hand[0].n, 2, "the first hand draws its second card");
    check_eq(g.hand[1].n, 1, "and the second waits its turn");

    /* Tens of different ranks are a pair by VALUE. */
    fresh();
    deal("Kh", "7s", "Qd", "9c", "", 0);
    bj_round_begin_stacked(&g, 10);
    bj_options(&g, &o);
    check(o.can_split, "a king and a queen are a pair by value");

    fresh();
    deal("Kh", "7s", "9d", "9c", "", 0);
    bj_round_begin_stacked(&g, 10);
    bj_options(&g, &o);
    check(!o.can_split, "a king and a nine are not");

    /* SPLIT ACES TAKE ONE CARD EACH, and a resulting 21 is not a
     * natural. Both halves matter: the first is worth about a fifth of
     * a percent of edge, the second is worth 50% of the bet. */
    fresh();
    deal("Ah", "7s", "Ad", "9c", "Kh Qs", 2);
    bj_round_begin_stacked(&g, 10);
    bj_insure(&g, 0);
    check(bj_act(&g, BJ_SPLIT), "aces split");
    check_eq(g.phase, BJ_PHASE_DONE, "and both hands finish at once");
    check_eq(g.hand[0].n, 2, "each having drawn exactly one card");
    check_eq(g.hand[1].n, 2, "both of them");
    check_eq(bj_total(g.hand[0].card, 2, 0), 21, "for twenty-one");
    /* 21 vs the dealer's 16-then-draw. Paid EVEN money, not 3:2 --
     * which is the whole point. */
    check(g.hand[0].won == 20 || g.hand[0].won == 10,
        "paid as a plain twenty-one, not as a natural");
    check(g.hand[0].won != 25, "certainly not three to two");
}

static void test_double_and_surrender(void)
{
    bj_options_t o;

    fresh();
    deal("6h", "9s", "5d", "7c", "Th", 1);
    bj_round_begin_stacked(&g, 10);
    bj_options(&g, &o);
    check(o.can_double, "eleven can be doubled");
    check(bj_act(&g, BJ_DOUBLE), "and doubling works");
    check_eq(g.staked, 20, "doubling the stake");
    check_eq(g.hand[0].n, 3, "for exactly one more card");
    check_eq(g.phase, BJ_PHASE_DONE, "and ends the hand");
    check_eq(g.returned, 40, "twenty-one beats sixteen-plus for forty");

    /* Doubling is a two-card decision only. */
    fresh();
    deal("6h", "9s", "5d", "7c", "2h 3s", 2);
    bj_round_begin_stacked(&g, 10);
    bj_act(&g, BJ_HIT);
    bj_options(&g, &o);
    check(!o.can_double, "but not after a hit");

    /* Late surrender returns half, rounded the player's way. */
    fresh();
    deal("Th", "Ts", "6d", "7c", "", 0);
    bj_round_begin_stacked(&g, 10);
    bj_options(&g, &o);
    check(o.can_surrender, "sixteen against ten may surrender");
    check(bj_act(&g, BJ_SURRENDER), "and does");
    check_eq(g.returned, 5, "for half the bet back");

    fresh();
    deal("Th", "Ts", "5d", "7c", "", 0);
    bj_round_begin_stacked(&g, 11);
    bj_act(&g, BJ_SURRENDER);
    check_eq(g.returned, 6, "an odd bet rounds in the player's favour");
}

static void test_dealer_play(void)
{
    /* The dealer does not draw when every hand has busted: there is
     * nothing to beat, and drawing would change the shoe for the next
     * round on the strength of a decision nobody made. */
    fresh();
    deal("Th", "6s", "6d", "5c", "Qh 9s 8d", 3);
    bj_round_begin_stacked(&g, 10);
    bj_act(&g, BJ_HIT);
    check(bj_total(g.hand[0].card, g.hand[0].n, 0) > 21, "the player busts");
    check_eq(g.ndealer, 2, "and the dealer does not draw");
    check_eq(g.returned, 0, "the bet is lost");

    /* A dealer bust pays every standing hand. */
    fresh();
    deal("Th", "6s", "9d", "Kc", "Qh", 1);
    bj_round_begin_stacked(&g, 10);
    bj_act(&g, BJ_STAND);
    check(bj_total(g.dealer, g.ndealer, 0) > 21, "the dealer busts");
    check_eq(g.returned, 20, "and pays the standing hand");
}

/* -- fuzz ------------------------------------------------------------------ */

static uint32_t st = 99;

static uint32_t frand(void *ctx)
{
    (void)ctx;
    st ^= st << 13; st ^= st >> 17; st ^= st << 5;
    return st;
}

static void test_fuzz(void)
{
    bj_rules_t r;
    long round;
    int bad_phase = 0, bad_total = 0, bad_cards = 0, overpaid = 0;
    int stuck = 0;

    zg_rng_set(frand, NULL);

    for (round = 0; round < 6000; round++) {
        int guard = 0;
        int i;

        bj_rules_default(&r);
        r.dealer_hits_soft17 = (round & 1) != 0;
        r.double_after_split = (round & 2) != 0;
        r.resplit_aces = (round & 4) != 0;
        r.surrender = (round & 8) != 0;
        if (round & 16) { r.bj_pay_num = 6; r.bj_pay_den = 5; }
        r.ndecks = 1 + (int)(round % 6);

        bj_game_init(&g, &r);
        if (!bj_round_begin(&g, 10)) { stuck++; continue; }

        while (g.phase != BJ_PHASE_DONE && guard++ < 60) {
            bj_options_t o;
            int pick;

            if (g.phase == BJ_PHASE_INSURANCE) {
                bj_insure(&g, (int32_t)(st % 6));
                continue;
            }

            bj_options(&g, &o);
            pick = (int)(st % 5);
            st ^= st << 7;

            if (pick == BJ_SPLIT && o.can_split) bj_act(&g, BJ_SPLIT);
            else if (pick == BJ_DOUBLE && o.can_double) bj_act(&g, BJ_DOUBLE);
            else if (pick == BJ_SURRENDER && o.can_surrender)
                bj_act(&g, BJ_SURRENDER);
            else if (pick == BJ_HIT && o.can_hit) bj_act(&g, BJ_HIT);
            else if (o.can_stand) bj_act(&g, BJ_STAND);
            else { stuck++; break; }
        }

        if (g.phase != BJ_PHASE_DONE) { bad_phase++; continue; }

        /* No hand may hold more cards than it can, nor exceed the
         * total a hand can actually reach. */
        for (i = 0; i < g.nhands; i++) {
            if (g.hand[i].n > BJ_MAX_CARDS) bad_cards++;
            if (g.hand[i].n < 1) bad_cards++;
            if (bj_total(g.hand[i].card, g.hand[i].n, 0) > 30) bad_total++;
        }
        if (g.ndealer > BJ_MAX_CARDS) bad_cards++;

        /* THE PAYOUT CEILING. The most a round can return is every
         * hand winning at three to two plus insurance at two to one.
         * Anything above that is money appearing from nowhere, which
         * is the failure mode a bank would hide until somebody looked
         * at the balance. */
        if (g.returned > g.staked * 3) overpaid++;
        if (g.returned < 0) overpaid++;
    }

    check_eq(bad_phase, 0, "every round reaches a settlement");
    check_eq(stuck, 0, "and none wedges with no legal action");
    check_eq(bad_cards, 0, "no hand overruns its card array");
    check_eq(bad_total, 0, "and no total is impossible");
    check_eq(overpaid, 0, "no round returns more than it could");

    zg_rng_set(NULL, NULL);
}

int main(void)
{
    printf("blackjack: rules tests\n");

    test_totals();
    test_naturals();
    test_dealer_rule();
    test_natural_pays();
    test_insurance();
    test_split();
    test_double_and_surrender();
    test_dealer_play();
    test_fuzz();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
