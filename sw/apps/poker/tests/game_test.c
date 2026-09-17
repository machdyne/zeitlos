/*
 * Zeitlos poker -- host tests for the betting engine.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- what is hard about testing a betting engine --
 *
 * Almost nothing here can be checked by playing. A side pot built one
 * chip wrong still pays somebody, an incomplete all-in that wrongly
 * reopens the betting just looks like an aggressive hand, and a stud
 * act order that is off by one seat is invisible unless you are
 * tracking it. All of them are wrong every time and none of them
 * announces itself.
 *
 * So the tests come in two kinds.
 *
 * SCRIPTED hands, with a stacked deck, asserting exact chip counts at
 * the end. These are the ones that pin down the rules. Each is a
 * situation somebody would otherwise have to contrive on real hardware
 * and then eyeball.
 *
 * FUZZ, thousands of hands per variant with a legal action chosen at
 * random at every decision, asserting the invariants that must hold
 * whatever happens: chips are conserved, no stack goes negative, the
 * engine always reaches a settled state, the pots sum to the pot, and
 * no illegal action is ever accepted. Conservation alone catches most
 * pot arithmetic mistakes, because chips have nowhere to hide.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "../pk_game.h"

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
    if (c == Z_CARD_NONE) { printf("bad card \"%s\"\n", s); exit(1); }
    return c;
}

static int32_t chips(const pk_game_t *g)
{
    int32_t t = 0;
    int i;
    for (i = 0; i < g->nseats; i++) t += g->seat[i].stack;
    return t;
}

/* Stacks PLUS the pot. Mid-hand, chips() alone is short by whatever
 * has been bet, so a conservation check that used it would fail on a
 * perfectly correct engine -- which it did, once, and the first guess
 * was that the engine had lost 390 chips. */
static int32_t total(const pk_game_t *g)
{
    return chips(g) + g->pot;
}

/* -- stacking a hold'em deck -----------------------------------------
 *
 * The deal order is two rounds of one card each, clockwise from the
 * button's left, then burn-flop-burn-turn-burn-river. Reproducing it
 * here rather than exposing it from pk_game.c is deliberate: if the
 * dealing order ever changes, these tests should break loudly rather
 * than quietly follow it.
 */
static void stack_holdem(pk_game_t *g, const char *const holes[][2],
    int nseats, const char *const board[5])
{
    uint8_t order[52];
    bool used[Z_NCARDS];
    int n = 0, k, i, next = 0;

    for (i = 0; i < Z_NCARDS; i++) used[i] = false;

    for (k = 0; k < 2; k++)
        for (i = 1; i <= nseats; i++) {
            uint8_t c = C(holes[i % nseats][k]);
            used[c] = true;
            order[n++] = c;
        }

    for (i = 0; i < 5; i++) used[C(board[i])] = true;

    for (k = 0; k < 3; k++) {
        /* A burn card, which must be one nobody was dealt. */
        while (next < Z_NCARDS && used[next]) next++;
        used[next] = true;
        order[n++] = (uint8_t)next;

        if (k == 0) {
            for (i = 0; i < 3; i++) order[n++] = C(board[i]);
        } else {
            order[n++] = C(board[2 + k]);
        }
    }

    zdeck_init(&g->deck, 1);
    if (!zdeck_stack(&g->deck, order, n)) {
        printf("FAIL: could not stack the deck\n");
        exit(1);
    }
}

static void act(pk_game_t *g, int action, int32_t to, const char *what)
{
    if (!pk_act(g, action, to)) {
        printf("FAIL: %s (%s to %d) was refused\n", what,
            pk_action_name(action), (int)to);
        failures++;
    }
    checks++;
}

/* -- blinds and act order -------------------------------------------- */

static void test_blind_order(void)
{
    pk_game_t g;

    /* Three-handed: small blind left of the button, big blind left of
     * that, and the button acts first before the flop. */
    pk_game_init(&g, &pk_variant_holdem, 3, 1000, 5, 10);
    pk_hand_begin(&g);

    check_eq(g.button, 0, "the first hand's button is seat 0");
    check_eq(g.seat[1].bet, 5, "seat 1 posts the small blind");
    check_eq(g.seat[2].bet, 10, "seat 2 posts the big blind");
    check_eq(g.actor, 0, "the button acts first before the flop");
    check_eq(g.pot, 15, "the blinds are in the pot");

    act(&g, PK_CALL, 0, "button calls");
    check_eq(g.actor, 1, "then the small blind");
    act(&g, PK_CALL, 0, "small blind completes");
    check_eq(g.actor, 2, "then the big blind, who has an option");

    /* The big blind has not acted voluntarily, so the round is not
     * over even though every bet is matched. */
    check(g.phase == PK_PHASE_BETTING, "the big blind gets its option");
    act(&g, PK_CHECK, 0, "big blind checks");

    check_eq(g.street, 1, "checking round ends the first street");
    check_eq(g.nboard, 3, "the flop is out");
    check_eq(g.actor, 1, "the small blind acts first after the flop");

    /* Heads up: the button IS the small blind and acts first before
     * the flop and last after it. This is the one everybody gets
     * wrong. */
    pk_game_init(&g, &pk_variant_holdem, 2, 1000, 5, 10);
    pk_hand_begin(&g);

    check_eq(g.button, 0, "heads-up button is seat 0");
    check_eq(g.seat[0].bet, 5, "heads-up, the button posts the small blind");
    check_eq(g.seat[1].bet, 10, "the other seat posts the big blind");
    check_eq(g.actor, 0, "heads-up, the button acts first before the flop");

    act(&g, PK_CALL, 0, "button calls");
    act(&g, PK_CHECK, 0, "big blind checks");

    check_eq(g.street, 1, "to the flop");
    check_eq(g.actor, 1, "heads-up, the button acts LAST after the flop");
}

/* -- the uncalled bet ------------------------------------------------ */

static void test_uncalled(void)
{
    pk_game_t g;
    int32_t before;

    pk_game_init(&g, &pk_variant_holdem, 2, 1000, 5, 10);
    before = chips(&g);
    pk_hand_begin(&g);

    act(&g, PK_RAISE, 100, "button raises to 100");
    act(&g, PK_FOLD, 0, "big blind folds");

    check(g.phase == PK_PHASE_COMPLETE, "the hand is over");
    check(!g.showdown, "a fold is not a showdown");

    /* The button risked 100 and got called for 10. The other 90 was
     * never at risk and comes back, so the win is the 10 the big
     * blind actually put in. */
    check_eq(g.seat[0].stack, 1010, "the winner is up the blind, not the bet");
    check_eq(g.seat[1].stack, 990, "the folder is down the blind");
    check_eq(chips(&g), before, "chips are conserved");
}

/* -- side pots ------------------------------------------------------- */

static void test_side_pots(void)
{
    pk_game_t g;
    static const char *const holes[3][2] = {
        { "3c", "4d" },   /* seat 0: nothing */
        { "Kh", "Kd" },   /* seat 1: trip kings, best hand */
        { "Jc", "Js" }    /* seat 2: trip jacks */
    };
    static const char *const board[5] = { "2c", "7d", "9s", "Jh", "Kc" };

    pk_game_init(&g, &pk_variant_holdem, 3, 1000, 5, 10);

    /* Three different stacks, so three different all-in levels. */
    g.seat[0].stack = 200;
    g.seat[1].stack = 50;
    g.seat[2].stack = 100;

    stack_holdem(&g, holes, 3, board);
    pk_hand_begin_stacked(&g);

    act(&g, PK_RAISE, 200, "seat 0 shoves 200");
    act(&g, PK_CALL, 0, "seat 1 calls all in for 50");
    act(&g, PK_CALL, 0, "seat 2 calls all in for 100");

    check(g.phase == PK_PHASE_COMPLETE, "the hand runs out and settles");
    check(g.showdown, "three players saw a showdown");
    check_eq(g.nboard, 5, "the whole board was dealt");

    /* 100 of seat 0's 200 was never matched and came back, so the
     * three commitments are 100, 50 and 100. */
    check_eq(g.npots, 2, "two pots");
    check_eq(g.pots[0].amount, 150, "main pot is three times the short stack");
    check_eq(g.pots[1].amount, 100, "side pot is what the other two matched");

    check(g.pots[0].eligible[1], "the short stack can win the main pot");
    check(!g.pots[1].eligible[1], "but not the side pot");

    check_eq(g.seat[1].stack, 150, "trip kings takes the main pot");
    check_eq(g.seat[2].stack, 100, "trip jacks takes the side pot");
    check_eq(g.seat[0].stack, 100, "seat 0 keeps its uncalled 100");
    check_eq(chips(&g), 350, "chips are conserved across the settlement");
}

/* -- the incomplete all-in raise ------------------------------------- */

static void test_incomplete_allin(void)
{
    pk_game_t g;
    pk_options_t o;

    pk_game_init(&g, &pk_variant_holdem, 3, 1000, 5, 10);
    g.seat[2].stack = 130;   /* the short stack */

    pk_hand_begin(&g);

    /* Seat 0 opens to 100. Seat 1 calls. Seat 2 can only reach 130,
     * which is a raise of 30 against a minimum of 100 -- incomplete. */
    act(&g, PK_RAISE, 100, "seat 0 raises to 100");
    act(&g, PK_CALL, 0, "seat 1 calls 100");
    check_eq(g.actor, 2, "the short stack is to act");

    pk_options(&g, 2, &o);
    check_eq(o.max_to, 130, "the short stack can only reach its stack");

    act(&g, PK_RAISE, 130, "seat 2 shoves 130");

    check_eq(g.bet_to_match, 130, "the bet moves to 130");
    check(g.phase == PK_PHASE_BETTING, "the others still have to answer");

    /* Seat 0 had already acted at 100. It may call the extra 30 or
     * fold, and may NOT re-raise. */
    check_eq(g.actor, 0, "action returns to seat 0");
    pk_options(&g, 0, &o);
    check(o.can_call, "seat 0 may call the difference");
    check(!o.can_raise, "an incomplete all-in does not reopen the betting");
    check(!pk_legal(&g, 0, PK_RAISE, 260), "and a re-raise is refused");
    check_eq(o.call_cost, 30, "the difference is 30");

    act(&g, PK_CALL, 0, "seat 0 calls the 30");
    pk_options(&g, 1, &o);
    check(!o.can_raise, "nor for seat 1, which had also already called");

    act(&g, PK_CALL, 0, "seat 1 calls the 30");
    check_eq(g.street, 1, "the street ends");
    check_eq(total(&g), 1000 + 1000 + 130, "chips are conserved mid-hand");
}

static void test_complete_allin_reopens(void)
{
    pk_game_t g;
    pk_options_t o;

    /* The same shape, but the short stack has enough for a full raise.
     * That DOES reopen the betting, and an engine that suppressed it
     * would be just as wrong in the other direction. */
    pk_game_init(&g, &pk_variant_holdem, 3, 1000, 5, 10);
    g.seat[2].stack = 250;

    pk_hand_begin(&g);

    act(&g, PK_RAISE, 100, "seat 0 raises to 100");
    act(&g, PK_CALL, 0, "seat 1 calls");
    act(&g, PK_RAISE, 250, "seat 2 shoves 250, a full raise");

    pk_options(&g, 0, &o);
    check(o.can_raise, "a full all-in raise reopens the betting");
    check_eq(o.min_to, 400, "and the next raise must be a full one again");
}

/* -- a split pot ----------------------------------------------------- */

static void test_split(void)
{
    pk_game_t g;
    static const char *const holes[2][2] = {
        { "2c", "3d" },
        { "2h", "3s" }
    };
    /* The board plays: a straight nobody can improve on. */
    static const char *const board[5] = { "Ac", "Kd", "Qs", "Jh", "Th" };

    pk_game_init(&g, &pk_variant_holdem, 2, 1000, 5, 10);
    stack_holdem(&g, holes, 2, board);
    pk_hand_begin_stacked(&g);

    act(&g, PK_CALL, 0, "button calls");
    act(&g, PK_CHECK, 0, "big blind checks");
    act(&g, PK_CHECK, 0, "check");
    act(&g, PK_CHECK, 0, "check");
    act(&g, PK_CHECK, 0, "check");
    act(&g, PK_CHECK, 0, "check");
    act(&g, PK_CHECK, 0, "check");
    act(&g, PK_CHECK, 0, "check");

    check(g.phase == PK_PHASE_COMPLETE, "the hand completes");
    check_eq(g.pots[0].nwinners, 2, "the board plays and the pot splits");
    check_eq(g.seat[0].stack, 1000, "seat 0 is even");
    check_eq(g.seat[1].stack, 1000, "seat 1 is even");

    /* An odd chip cannot be split and has to go somewhere by RULE
     * rather than by rounding. Three seats commit five each, two of
     * them tie, and the fifteenth chip goes to the first winner
     * clockwise from the button's left.
     *
     * Seats 0 and 1 hold the same pair; seat 2 plays the board and
     * loses to it. An equal blind both ways makes the pot odd. */
    {
        static const char *const h3[3][2] = {
            { "7h", "7d" }, { "7s", "7c" }, { "3h", "4d" }
        };
        static const char *const b3[5] = { "Ac", "Kd", "Qs", "9h", "2c" };
        int i;

        pk_game_init(&g, &pk_variant_holdem, 3, 1000, 5, 5);
        stack_holdem(&g, h3, 3, b3);
        pk_hand_begin_stacked(&g);

        act(&g, PK_CALL, 0, "button calls");
        act(&g, PK_CHECK, 0, "small blind checks");
        act(&g, PK_CHECK, 0, "big blind checks");
        for (i = 0; i < 9; i++) act(&g, PK_CHECK, 0, "check it down");

        check(g.phase == PK_PHASE_COMPLETE, "the odd-chip hand completes");
        check_eq(g.pots[0].amount, 15, "the pot is fifteen");
        check_eq(g.pots[0].nwinners, 2, "two seats tie");
        check_eq(g.seat[1].stack, 1003,
            "the odd chip goes to the first winner left of the button");
        check_eq(g.seat[0].stack, 1002, "the other winner gets the floor");
        check_eq(g.seat[2].stack, 995, "the loser is down its blind");
        check_eq(chips(&g), 3000, "an odd split conserves chips");
    }
}

/* -- the fixed-limit cap --------------------------------------------- */

static void test_fixed_cap(void)
{
    pk_game_t g;
    pk_options_t o;

    pk_game_init(&g, &pk_variant_holdem, 3, 1000, 5, 10);
    pk_game_set_limit(&g, PK_LIMIT_FIXED);
    pk_hand_begin(&g);

    /* The big blind counts as the first bet, so three raises close it. */
    pk_options(&g, 0, &o);
    check_eq(o.min_to, 20, "a fixed-limit raise is exactly one bet");
    check_eq(o.max_to, 20, "and no more");

    act(&g, PK_RAISE, 20, "raise two");
    act(&g, PK_RAISE, 30, "raise three");
    act(&g, PK_RAISE, 40, "raise four");

    pk_options(&g, 0, &o);
    check(!o.can_raise, "four bets caps the round");
    check(o.can_call, "but calling is still allowed");

    /* And the bet size doubles on the turn. */
    pk_game_init(&g, &pk_variant_holdem, 3, 1000, 5, 10);
    pk_game_set_limit(&g, PK_LIMIT_FIXED);
    pk_hand_begin(&g);
    act(&g, PK_CALL, 0, "call");
    act(&g, PK_CALL, 0, "call");
    act(&g, PK_CHECK, 0, "check");
    act(&g, PK_CHECK, 0, "flop check");
    act(&g, PK_CHECK, 0, "flop check");
    act(&g, PK_CHECK, 0, "flop check");

    check_eq(g.street, 2, "on the turn");
    pk_options(&g, g.actor, &o);
    check_eq(o.min_to, 20, "the bet size doubles on the turn");
}

/* -- stud ------------------------------------------------------------ */

static void test_stud_bring_in(void)
{
    pk_game_t g;
    int i;

    /* The bring-in is the LOWEST up-card, and a tie there is broken by
     * suit with clubs lowest -- the only place in poker where a suit
     * ranks. Both seats show a deuce; the club must bring it in. */
    pk_game_init(&g, &pk_variant_stud5, 3, 1000, 5, 10);

    zdeck_init(&g.deck, 1);
    {
        /* Deal order for stud5 street 0 is one down each then one up
         * each, clockwise from the button's left: seats 1, 2, 0. */
        uint8_t order[6];
        order[0] = C("Ah"); order[1] = C("Kh"); order[2] = C("Qh");
        order[3] = C("2d"); order[4] = C("2c"); order[5] = C("9s");
        if (!zdeck_stack(&g.deck, order, 6)) { printf("stack failed\n"); exit(1); }
    }
    pk_hand_begin_stacked(&g);

    check_eq(g.seat[2].hole[1], C("2c"), "seat 2 shows the two of clubs");
    check_eq(g.seat[1].hole[1], C("2d"), "seat 1 shows the two of diamonds");
    check_eq(g.seat[2].bet, g.bring_in, "the club brings it in, not the diamond");
    check_eq(g.actor, 0, "action moves to the next seat");

    for (i = 0; i < 3; i++)
        check_eq(g.seat[i].committed - g.seat[i].bet, g.ante,
            "everybody antes, and the ante is not a bet");

    /* The bring-in is a forced bet, not an action, so that seat still
     * has its option if nobody raises. */
    act(&g, PK_CALL, 0, "seat 0 calls the bring-in");
    act(&g, PK_CALL, 0, "seat 1 calls");
    check_eq(g.actor, 2, "the bring-in gets its option");
    check(g.phase == PK_PHASE_BETTING, "and the street is not over yet");
}

static void test_stud_high_board(void)
{
    pk_game_t g;

    /* From fourth street on, the best hand SHOWING acts first --
     * not the seat left of the button. */
    pk_game_init(&g, &pk_variant_stud5, 3, 1000, 5, 10);

    zdeck_init(&g.deck, 1);
    {
        uint8_t order[10];
        /* street 0: down to 1,2,0 then up to 1,2,0 */
        order[0] = C("7h"); order[1] = C("8h"); order[2] = C("9h");
        order[3] = C("3d"); order[4] = C("4c"); order[5] = C("5s");
        /* street 1: one up each, to 1,2,0 */
        order[6] = C("3s"); order[7] = C("Jc"); order[8] = C("2h");
        order[9] = C("2s");
        if (!zdeck_stack(&g.deck, order, 10)) { printf("stack failed\n"); exit(1); }
    }
    pk_hand_begin_stacked(&g);

    /* Seat 1 shows 3d and brings it in (lowest). */
    check_eq(g.seat[1].bet, g.bring_in, "the three brings it in");

    act(&g, PK_CALL, 0, "call");
    act(&g, PK_CALL, 0, "call");
    act(&g, PK_CHECK, 0, "bring-in checks its option");

    check_eq(g.street, 1, "on to fourth street");

    /* Showing: seat 0 has 5s 2h, seat 1 has 3d 3s (a pair), seat 2 has
     * 4c Jc. The pair acts first. */
    check_eq(g.actor, 1, "the pair showing acts first on fourth street");
}

static void test_stud7_eight_handed(void)
{
    pk_game_t g;
    int i, guard = 0;

    /* Eight players in seven-card stud need 56 cards. The rule is a
     * single community card on the last street instead, and that path
     * is only reachable with a full table. */
    pk_game_init(&g, &pk_variant_stud7, 8, 1000, 5, 10);
    pk_hand_begin(&g);

    while (g.phase != PK_PHASE_COMPLETE && guard++ < 500) {
        if (g.phase == PK_PHASE_BETTING) {
            pk_options_t o;
            pk_options(&g, g.actor, &o);
            if (o.can_check) pk_act(&g, PK_CHECK, 0);
            else pk_act(&g, PK_CALL, 0);
        } else if (g.phase == PK_PHASE_DRAW) {
            pk_draw(&g, NULL, 0);
        }
    }

    check(g.phase == PK_PHASE_COMPLETE, "an eight-handed stud hand completes");
    check(guard < 500, "and does not loop");
    check_eq(g.nboard, 1, "the seventh card is dealt as a community card");

    for (i = 0; i < 8; i++)
        check(pk_seat_value(&g, i, NULL) != PK_EVAL_NONE,
            "every seat still makes a five-card hand");

    check_eq(chips(&g), 8000, "chips are conserved eight-handed");
}

/* -- the draw -------------------------------------------------------- */

static void test_draw(void)
{
    pk_game_t g;
    uint8_t idx[3];
    uint8_t before[5];
    int i, same = 0;

    pk_game_init(&g, &pk_variant_draw5, 3, 1000, 5, 10);
    pk_hand_begin(&g);

    check_eq(g.seat[0].nhole, 5, "five cards each");

    act(&g, PK_CALL, 0, "call");
    act(&g, PK_CALL, 0, "call");
    act(&g, PK_CHECK, 0, "check");

    check(g.phase == PK_PHASE_DRAW, "the draw comes after the first round");
    check_eq(g.actor, 1, "the first live seat left of the button draws first");

    for (i = 0; i < 5; i++) before[i] = g.seat[1].hole[i];

    idx[0] = 0; idx[1] = 2; idx[2] = 4;
    check(pk_draw(&g, idx, 3), "drawing three is legal");
    check_eq(g.seat[1].nhole, 5, "and leaves five cards");

    for (i = 0; i < 5; i++) {
        int j;
        for (j = 0; j < 5; j++) if (g.seat[1].hole[i] == before[j]) same++;
    }
    check_eq(same, 2, "two cards were kept and three replaced");

    check(!pk_draw(&g, idx, 9), "drawing more than a hand refuses");
    idx[0] = 1; idx[1] = 1;
    check(!pk_draw(&g, idx, 2), "drawing the same card twice refuses");

    check(pk_draw(&g, NULL, 0), "standing pat is legal");
    check(pk_draw(&g, NULL, 0), "and the last seat stands pat");

    check(g.phase == PK_PHASE_BETTING, "betting resumes after the draw");
    check_eq(g.street, 1, "on the second street");
}

/* -- fuzz ------------------------------------------------------------ */

static uint32_t fuzz_state = 1;

static uint32_t frand(uint32_t n)
{
    fuzz_state ^= fuzz_state << 13;
    fuzz_state ^= fuzz_state >> 17;
    fuzz_state ^= fuzz_state << 5;
    return n ? fuzz_state % n : 0;
}

static void fuzz_variant(const pk_variant_t *v, int nseats, int limit,
    int hands, const char *label)
{
    pk_game_t g;
    int32_t start;
    int h;
    int bad_state = 0, bad_sum = 0, stuck = 0, illegal_taken = 0;
    int negatives = 0;

    pk_game_init(&g, v, nseats, 1000, 5, 10);
    pk_game_set_limit(&g, limit);
    start = chips(&g);

    for (h = 0; h < hands; h++) {
        int guard = 0;
        int32_t before = chips(&g);

        if (pk_live_count(&g) < 2 && chips(&g) == before) {
            /* Somebody has all the chips; start again so the fuzz
             * keeps exercising real hands rather than empty ones. */
            pk_game_init(&g, v, nseats, 1000, 5, 10);
            pk_game_set_limit(&g, limit);
            before = chips(&g);
        }

        pk_hand_begin(&g);

        while (g.phase != PK_PHASE_COMPLETE && guard++ < 400) {

            if (g.phase == PK_PHASE_DRAW) {
                uint8_t idx[PK_MAX_HOLE];
                int n = (int)frand(4);
                int i;
                for (i = 0; i < n; i++) idx[i] = (uint8_t)i;
                pk_draw(&g, idx, n);
                continue;
            }

            {
                pk_options_t o;
                int choice;
                int32_t to;

                pk_options(&g, g.actor, &o);

                /* An action the options say is NOT available must be
                 * refused. Checking the negative is the half that
                 * catches a permissive engine. */
                if (!o.can_raise && pk_legal(&g, g.actor, PK_RAISE,
                    g.bet_to_match + g.big_blind)) illegal_taken++;
                if (!o.can_check && pk_legal(&g, g.actor, PK_CHECK, 0))
                    illegal_taken++;
                if (o.can_raise && pk_legal(&g, g.actor, PK_RAISE,
                    o.min_to - 1) && o.min_to - 1 > g.bet_to_match)
                    illegal_taken++;

                choice = (int)frand(10);

                if (choice < 2 && o.can_fold) {
                    pk_act(&g, PK_FOLD, 0);
                } else if (choice < 7) {
                    pk_act(&g, o.can_check ? PK_CHECK : PK_CALL, 0);
                } else if (o.can_bet || o.can_raise) {
                    int32_t span = o.max_to - o.min_to;
                    to = o.min_to + (int32_t)frand((uint32_t)span + 1);
                    if (!pk_act(&g, o.can_bet ? PK_BET : PK_RAISE, to))
                        pk_act(&g, o.can_check ? PK_CHECK : PK_CALL, 0);
                } else {
                    pk_act(&g, o.can_check ? PK_CHECK : PK_CALL, 0);
                }
            }
        }

        if (g.phase != PK_PHASE_COMPLETE) { stuck++; break; }
        if (guard >= 400) stuck++;
        if (chips(&g) != before) bad_sum++;

        {
            int i;
            int32_t potsum = 0;
            for (i = 0; i < g.npots; i++) potsum += g.pots[i].amount;
            if (potsum != g.pot) bad_state++;
            for (i = 0; i < g.nseats; i++)
                if (g.seat[i].stack < 0) negatives++;
        }
    }

    {
        char msg[96];
        snprintf(msg, sizeof msg, "%s: every hand reaches a settlement", label);
        check_eq(stuck, 0, msg);
        snprintf(msg, sizeof msg, "%s: chips are conserved", label);
        check_eq(bad_sum, 0, msg);
        snprintf(msg, sizeof msg, "%s: the pots sum to the pot", label);
        check_eq(bad_state, 0, msg);
        snprintf(msg, sizeof msg, "%s: no stack goes negative", label);
        check_eq(negatives, 0, msg);
        snprintf(msg, sizeof msg, "%s: no illegal action is permitted", label);
        check_eq(illegal_taken, 0, msg);
        snprintf(msg, sizeof msg, "%s: the table's chips are unchanged", label);
        check_eq(chips(&g), start, msg);
    }
}

static void test_fuzz(void)
{
    fuzz_variant(&pk_variant_holdem, 6, PK_LIMIT_NONE, 3000, "holdem no-limit");
    fuzz_variant(&pk_variant_holdem, 2, PK_LIMIT_NONE, 3000, "holdem heads-up");
    fuzz_variant(&pk_variant_holdem, 4, PK_LIMIT_FIXED, 3000, "holdem fixed");
    fuzz_variant(&pk_variant_holdem, 5, PK_LIMIT_POT, 3000, "holdem pot-limit");
    fuzz_variant(&pk_variant_draw5, 5, PK_LIMIT_FIXED, 3000, "five-card draw");
    fuzz_variant(&pk_variant_stud5, 5, PK_LIMIT_FIXED, 3000, "five-card stud");
    fuzz_variant(&pk_variant_stud7, 7, PK_LIMIT_FIXED, 3000, "seven-card stud");
    fuzz_variant(&pk_variant_stud7, 8, PK_LIMIT_FIXED, 2000, "stud, eight-handed");
}

/* -- the variant table ----------------------------------------------- */

static void test_variants(void)
{
    int i;

    check_eq(pk_variant_count(), 4, "four variants are registered");
    check(pk_variant_find("holdem") == &pk_variant_holdem, "holdem by name");
    check(pk_variant_find("stud7") == &pk_variant_stud7, "stud7 by name");
    check(pk_variant_find("nonesuch") == NULL, "an unknown name refuses");

    for (i = 0; i < pk_variant_count(); i++) {
        const pk_variant_t *v = pk_variant_at(i);
        int s, dealt = 0;

        check(v->nstreets >= 1 && v->nstreets <= PK_MAX_STREETS,
            "the street count is in range");
        check(v->max_seats >= 2 && v->max_seats <= PK_MAX_SEATS,
            "the seat count is in range");

        for (s = 0; s < v->nstreets; s++)
            dealt += v->deal_down[s] + v->deal_up[s];

        /* Every variant must deal each player enough to make a hand,
         * with the board making up the difference. */
        check(dealt + (v == &pk_variant_holdem ? 5 : 0) >= 5,
            "each seat can reach five cards");
    }
}

int main(void)
{
    printf("poker: betting engine tests\n");

    test_variants();
    test_blind_order();
    test_uncalled();
    test_side_pots();
    test_incomplete_allin();
    test_complete_allin_reopens();
    test_split();
    test_fixed_cap();
    test_stud_bring_in();
    test_stud_high_board();
    test_stud7_eight_handed();
    test_draw();
    test_fuzz();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
