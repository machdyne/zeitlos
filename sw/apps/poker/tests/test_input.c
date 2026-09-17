/*
 * Zeitlos poker -- host tests for the command line and the mouse.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * input.h's claim is that typing `fold` and clicking `fold` are the
 * same operation expressed differently, and that neither is a special
 * case of the other. That is a testable claim and this file tests it:
 * every action is driven both ways and the resulting game state is
 * compared.
 *
 * The rest is the parsing, which is hand-rolled because one conversion
 * specifier would link picolibc's formatter at a cost of around 100KB
 * (docs/app_runtime.md) -- and hand-rolled parsers are exactly the
 * thing that works for every input somebody thought of.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "poker_shim.h"

#include "../input.h"

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

static pk_game_t game;
static pt_view_t view;
static pt_layout_t L;

static void setup(const pk_variant_t *v, int nseats, int limit)
{
    int i;
    memset(&view, 0, sizeof view);
    zg_rng_seed(4242);
    pk_game_init(&game, v, nseats, 1000, 5, 10);
    pk_game_set_limit(&game, limit);
    pk_hand_begin(&game);
    view.g = &game;
    view.hero = 0;
    view.level = 4;
    view.pending_limit = -1;
    for (i = 0; i < nseats; i++) {
        view.name[i][0] = (char)('A' + i);
        view.name[i][1] = '\0';
    }
    pt_layout(&L, &view, 2, 13, 316, 225);
    input_bet_step(&view, 0);
}

/* Moves the action round to the hero, so a test can act as them. */
static void to_hero(void)
{
    int guard = 0;
    while (game.phase == PK_PHASE_BETTING && game.actor != view.hero &&
        guard++ < 40) {
        pk_options_t o;
        pk_options(&game, game.actor, &o);
        pk_act(&game, o.can_check ? PK_CHECK : PK_CALL, 0);
    }
}

static pi_action_t cmd(const char *s)
{
    return input_command(&view, s);
}

static pi_action_t type(const char *s)
{
    for (; *s; s++) (void)input_key(&view, (uint32_t)(unsigned char)*s);
    return input_key(&view, '\r');
}

/* -- the actions ------------------------------------------------------- */

static void test_actions(void)
{
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();

    check_eq(game.actor, view.hero, "the hero is to act");
    check_eq(cmd("f"), PI_ACTED, "f folds");
    check_eq(game.seat[0].state, PK_SEAT_FOLDED, "and the seat is folded");

    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    check_eq(cmd("fold"), PI_ACTED, "the long spelling folds too");
    check_eq(game.seat[0].state, PK_SEAT_FOLDED, "same result");

    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    {
        int32_t before = game.seat[0].stack;
        check_eq(cmd("c"), PI_ACTED, "c calls");
        check(game.seat[0].stack < before, "and chips went in");
    }

    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    check_eq(cmd("r 120"), PI_ACTED, "r with an amount raises");
    check_eq(game.seat[0].bet, 120, "to exactly that amount");

    /* A bare number is a raise. Nothing else somebody types at a poker
     * table is a bare number. */
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    check_eq(cmd("80"), PI_ACTED, "a bare number raises");
    check_eq(game.seat[0].bet, 80, "to that amount");

    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    check_eq(cmd("a"), PI_ACTED, "a goes all in");
    check_eq(game.seat[0].state, PK_SEAT_ALLIN, "and the seat is all in");
    check_eq(game.seat[0].stack, 0, "with nothing left");

    /* Too large is rounded down, not refused. Somebody typing more
     * than their stack means all of it, and being told the exact
     * figure they should have typed instead is a worse answer. */
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    check_eq(cmd("r 999999"), PI_ACTED, "an oversized raise is accepted");
    check_eq(game.seat[0].stack, 0, "as an all in");

    /* Too small IS refused, because rounding it up would put in more
     * chips than were asked for. */
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    {
        int32_t bet = game.seat[0].bet;
        check_eq(cmd("r 11"), PI_REDRAW, "an undersized raise is refused");
        check_eq(game.seat[0].bet, bet, "and nothing was put in");
        check(strstr(view.message, "smallest") != NULL,
            "and it says what the smallest is");
    }
}

static void test_not_your_turn(void)
{
    setup(&pk_variant_holdem, 6, PK_LIMIT_NONE);

    /* Six-handed, the hero is on the button and acts last preflop. */
    while (game.actor == view.hero && game.phase == PK_PHASE_BETTING)
        pk_act(&game, PK_CHECK, 0);

    if (game.actor != view.hero) {
        int32_t before = game.seat[0].stack;
        check_eq(cmd("f"), PI_REDRAW, "acting out of turn is refused");
        check_eq(game.seat[0].stack, before, "and changes nothing");
        check(game.seat[0].state != PK_SEAT_FOLDED, "and does not fold you");
    }
}

/* -- the free shortcut -------------------------------------------------- */

static void test_return_checks(void)
{
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();

    /* Preflop with a bet to call, Return must do NOTHING. Calling by
     * accident is the mistake input.h says is worth designing out. */
    {
        int32_t before = game.seat[0].stack;
        pk_options_t o;
        pk_options(&game, 0, &o);
        if (o.can_call) {
            check_eq(input_key(&view, '\r'), PI_NONE,
                "Return does not call a bet");
            check_eq(game.seat[0].stack, before, "and puts nothing in");
            check_eq(input_key(&view, ' '), PI_NONE,
                "and neither does Space");
        }
    }

    /* Once checking is free, Return checks. */
    cmd("c");
    to_hero();
    {
        pk_options_t o;
        pk_options(&game, 0, &o);
        if (o.can_check) {
            int32_t before = game.seat[0].stack;
            check_eq(input_key(&view, '\r'), PI_ACTED, "Return checks");
            check_eq(game.seat[0].stack, before, "for free");
        }
    }
}

/* -- line editing -------------------------------------------------------- */

static void test_editing(void)
{
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);

    input_key(&view, 'l');
    input_key(&view, 'e');
    input_key(&view, 'v');
    check_eq(view.cmd_len, 3, "typing appends to the line");
    check(strcmp(view.cmd, "lev") == 0, "and the line reads back");

    input_key(&view, 0x08);
    check_eq(view.cmd_len, 2, "backspace removes a character");
    check(strcmp(view.cmd, "le") == 0, "and terminates the line");

    input_key(&view, 0x1b);
    check_eq(view.cmd_len, 0, "escape clears the line");
    check(view.cmd[0] == '\0', "and empties it");

    /* Backspace on an empty line must not walk off the front. */
    input_key(&view, 0x08);
    check_eq(view.cmd_len, 0, "backspace on an empty line is harmless");

    /* Typing must not overrun the buffer. */
    {
        int i;
        for (i = 0; i < PT_CMD_LEN * 3; i++) input_key(&view, 'x');
        check(view.cmd_len < PT_CMD_LEN, "the line cannot overflow");
        check(view.cmd[view.cmd_len] == '\0', "and stays terminated");
    }

    input_key(&view, 0x1b);
    to_hero();
    check_eq(type("fold"), PI_ACTED, "a typed line runs on Return");
    check_eq(view.cmd_len, 0, "and the line is cleared afterwards");
}

/* -- settings ------------------------------------------------------------ */

static void test_settings(void)
{
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);

    check_eq(cmd("level 7"), PI_REDRAW, "level is accepted");
    check_eq(view.level, 7, "and takes effect");
    check_eq(cmd("level 9"), PI_REDRAW, "an out-of-range level is refused");
    check_eq(view.level, 7, "and leaves the level alone");
    check_eq(cmd("level"), PI_REDRAW, "a bare level says what it wants");

    check_eq(cmd("variant stud7"), PI_NEWGAME, "variant asks for a new game");
    check(view.pending_variant == &pk_variant_stud7, "with the right one");
    check_eq(cmd("variant nonesuch"), PI_REDRAW, "an unknown variant refuses");

    check_eq(cmd("seats 8"), PI_NEWGAME, "seats asks for a new game");
    check_eq(view.pending_seats, 8, "with the right count");
    check_eq(cmd("seats 1"), PI_REDRAW, "one seat is refused");
    check_eq(cmd("seats 99"), PI_REDRAW, "ninety-nine seats is refused");

    check_eq(cmd("limit fl"), PI_REDRAW, "limit fl is accepted");
    check_eq(view.pending_limit, PK_LIMIT_FIXED, "and recorded");
    check_eq(cmd("limit zz"), PI_REDRAW, "an unknown structure is refused");

    check_eq(cmd("quit"), PI_QUIT, "quit quits");
    check_eq(cmd("game"), PI_GAME_MODE, "game toggles full screen");
    check_eq(cmd("new"), PI_NEWGAME, "new re-seats the table");

    check_eq(cmd("wibble"), PI_REDRAW, "an unknown command is refused");
    check(strstr(view.message, "unknown") != NULL, "and says so");

    check_eq(cmd(""), PI_NONE, "an empty line does nothing");
    check_eq(cmd("   "), PI_NONE, "and neither does whitespace");

    /* Case must not matter. Somebody with caps lock on is not making
     * a different request. */
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    check_eq(cmd("LEVEL 3"), PI_REDRAW, "commands are case-insensitive");
    check_eq(view.level, 3, "and take effect");
}

/* -- the draw ------------------------------------------------------------- */

static void test_draw(void)
{
    int guard = 0;

    setup(&pk_variant_draw5, 3, PK_LIMIT_FIXED);

    while (game.phase == PK_PHASE_BETTING && guard++ < 30) {
        pk_options_t o;
        pk_options(&game, game.actor, &o);
        pk_act(&game, o.can_check ? PK_CHECK : PK_CALL, 0);
    }

    while (game.phase == PK_PHASE_DRAW && game.actor != view.hero)
        pk_draw(&game, NULL, 0);

    check(game.phase == PK_PHASE_DRAW, "the draw phase is reached");
    check_eq(game.actor, view.hero, "and it is the hero's draw");

    check_eq(cmd("d 1 3 5"), PI_ACTED, "discarding three is accepted");
    check(strstr(view.message, "3") != NULL, "and says how many");

    /* Standing pat, from the other side of the same command. */
    setup(&pk_variant_draw5, 3, PK_LIMIT_FIXED);
    guard = 0;
    while (game.phase == PK_PHASE_BETTING && guard++ < 30) {
        pk_options_t o;
        pk_options(&game, game.actor, &o);
        pk_act(&game, o.can_check ? PK_CHECK : PK_CALL, 0);
    }
    while (game.phase == PK_PHASE_DRAW && game.actor != view.hero)
        pk_draw(&game, NULL, 0);

    {
        uint8_t before[5];
        int i;
        for (i = 0; i < 5; i++) before[i] = game.seat[0].hole[i];
        check_eq(cmd("p"), PI_ACTED, "p stands pat");
        for (i = 0; i < 5; i++)
            check_eq(game.seat[0].hole[i], before[i],
                "and changes no card");
    }

    /* A nonsense card number must be refused rather than wrapped. */
    setup(&pk_variant_draw5, 3, PK_LIMIT_FIXED);
    guard = 0;
    while (game.phase == PK_PHASE_BETTING && guard++ < 30) {
        pk_options_t o;
        pk_options(&game, game.actor, &o);
        pk_act(&game, o.can_check ? PK_CHECK : PK_CALL, 0);
    }
    while (game.phase == PK_PHASE_DRAW && game.actor != view.hero)
        pk_draw(&game, NULL, 0);

    check_eq(cmd("d 9"), PI_REDRAW, "a card number out of range is refused");
    check(game.phase == PK_PHASE_DRAW, "and the draw is still pending");
    check_eq(cmd("d 0"), PI_REDRAW, "and so is zero");
}

/* -- the bet amount ------------------------------------------------------- */

static void test_bet_amount(void)
{
    pk_options_t o;

    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();

    pk_options(&game, 0, &o);
    input_bet_step(&view, 0);
    check(view.bet_to >= o.min_to && view.bet_to <= o.max_to,
        "the amount starts inside the legal range");

    input_bet_step(&view, 1);
    check(view.bet_to <= o.max_to, "stepping up stays legal");
    input_bet_step(&view, -20);
    check(view.bet_to >= o.min_to,
        "and stepping far down clamps to the minimum");
    input_bet_step(&view, 200);
    check_eq(view.bet_to, o.max_to, "stepping far up clamps to all in");

    /* Fixed limit has exactly one legal size, so every step collapses
     * onto it -- which is the behaviour that makes the same two
     * buttons work for both structures. */
    setup(&pk_variant_holdem, 3, PK_LIMIT_FIXED);
    to_hero();
    pk_options(&game, 0, &o);
    input_bet_step(&view, 0);
    check_eq(view.bet_to, o.min_to, "fixed limit pins the amount");
    input_bet_step(&view, 5);
    check_eq(view.bet_to, o.min_to, "and stepping cannot move it");
}

/* -- clicking ------------------------------------------------------------- */

static void click_button(int b, pi_action_t *out)
{
    int cx = L.btn[b].x + L.btn[b].w / 2;
    int cy = L.btn[b].y + L.btn[b].h / 2;
    *out = input_click(&view, &L, cx, cy);
}

static void test_clicks(void)
{
    pi_action_t a;
    int32_t typed_stack, clicked_stack;

    /* THE CLAIM IN input.h, TESTED. Folding by click and folding by
     * command must leave the game in the same state -- otherwise one
     * of them is a special case of the other and they will drift. */
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    cmd("f");
    typed_stack = game.seat[0].stack;
    check_eq(game.seat[0].state, PK_SEAT_FOLDED, "typing folds");

    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    click_button(PT_BTN_FOLD, &a);
    clicked_stack = game.seat[0].stack;
    check_eq(a, PI_ACTED, "clicking fold acts");
    check_eq(game.seat[0].state, PK_SEAT_FOLDED, "and folds");
    check_eq(clicked_stack, typed_stack, "both routes end in the same place");

    /* Calling, the same way. */
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    cmd("c");
    typed_stack = game.seat[0].stack;

    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    click_button(PT_BTN_CALL, &a);
    check_eq(a, PI_ACTED, "clicking call acts");
    check_eq(game.seat[0].stack, typed_stack, "for the same chips");

    /* All in. */
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    click_button(PT_BTN_ALLIN, &a);
    check_eq(a, PI_ACTED, "clicking all in acts");
    check_eq(game.seat[0].stack, 0, "for everything");

    /* Raising by click must use the amount in the box, which is what
     * makes the -/+ controls mean anything. */
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    input_bet_step(&view, 0);
    input_bet_step(&view, 2);
    {
        int32_t want = view.bet_to;
        click_button(PT_BTN_RAISE, &a);
        check_eq(a, PI_ACTED, "clicking raise acts");
        check_eq(game.seat[0].bet, want, "for the amount in the box");
    }

    /* The -/+ buttons are the same operation as the -/+ keys. */
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    input_bet_step(&view, 0);
    {
        int32_t base = view.bet_to, by_button, by_key;
        click_button(PT_BTN_MORE, &a);
        by_button = view.bet_to;
        check_eq(a, PI_REDRAW, "clicking plus only redraws");
        view.bet_to = base;
        input_key(&view, '+');
        by_key = view.bet_to;
        check_eq(by_button, by_key, "the plus key and button agree");
    }

    /* A click on empty felt must do nothing at all. */
    setup(&pk_variant_holdem, 3, PK_LIMIT_NONE);
    to_hero();
    check_eq(input_click(&view, &L, L.ox + 1, L.pot_y), PI_NONE,
        "clicking the table does nothing");
    check_eq(game.seat[0].state, PK_SEAT_LIVE, "and does not act");
}

static void test_card_clicks(void)
{
    int guard = 0, x, y;

    setup(&pk_variant_draw5, 3, PK_LIMIT_FIXED);

    /* Outside a draw, clicking a card must do nothing -- there is no
     * selection in poker and a click that appeared to select one would
     * be a control that does not exist. */
    pt_hero_card_xy(&L, 1, &x, &y);
    check_eq(input_click(&view, &L, x + 2, y + 2), PI_NONE,
        "clicking a card outside a draw does nothing");
    check(!view.discard[1], "and marks nothing");

    while (game.phase == PK_PHASE_BETTING && guard++ < 30) {
        pk_options_t o;
        pk_options(&game, game.actor, &o);
        pk_act(&game, o.can_check ? PK_CHECK : PK_CALL, 0);
    }
    while (game.phase == PK_PHASE_DRAW && game.actor != view.hero)
        pk_draw(&game, NULL, 0);

    pt_layout(&L, &view, 2, 13, 316, 225);

    pt_hero_card_xy(&L, 2, &x, &y);
    check_eq(input_click(&view, &L, x + 2, y + 2), PI_REDRAW,
        "clicking a card in a draw marks it");
    check(view.discard[2], "the right card");
    check(!view.discard[1] && !view.discard[3], "and only that one");

    check_eq(input_click(&view, &L, x + 2, y + 2), PI_REDRAW,
        "clicking it again unmarks it");
    check(!view.discard[2], "and it is clear");
}

int main(void)
{
    printf("poker: input tests\n");

    /* No framebuffer is mapped and none is needed. Nothing in this
     * file draws: pt_layout() is pure, and the hit tests are
     * arithmetic on the layout it produced. That is worth saying,
     * because poker_shim.h is included for the types and would happily
     * let a stray draw call write to an address that is not there. */

    test_actions();
    test_not_your_turn();
    test_return_checks();
    test_editing();
    test_settings();
    test_draw();
    test_bet_amount();
    test_clicks();
    test_card_clicks();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
