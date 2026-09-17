/*
 * Zeitlos roulette -- host tests for the command line and the mouse.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * rl_input.h's claim is that typing a bet and clicking it are the same
 * operation. That is testable, and this file tests it: a bet placed
 * each way must leave the round identical.
 *
 * The other half is the bank. A game that lets somebody stake chips
 * they do not have is not a game, and the check is easy to get subtly
 * wrong -- chips already on the table are spoken for even though they
 * have not left the bank yet.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "roulette_shim.h"
#include "../rl_input.h"

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

static rl_round_t round_;
static rl_wheel_t wheel;
static rl_view_t view;
static rl_layout_t L;
static z_win_t win;

static void setup(void)
{
    memset(&view, 0, sizeof view);
    rl_round_clear(&round_, RL_EURO);
    rl_wheel_init(&wheel, RL_EURO, 0, 0, 40);
    view.round = &round_;
    view.wheel = &wheel;
    view.chips = 1000;
    view.chip_sel = 1;          /* 5 */
    view.result = -1;
    rl_board_layout(&L, &view, 2, 13, 316, 225);
}

static ri_action_t cmd(const char *s) { return rl_input_command(&view, s); }

static ri_action_t type_line(const char *s)
{
    for (; *s; s++) (void)rl_input_key(&view, (uint32_t)(unsigned char)*s);
    return rl_input_key(&view, '\r');
}

static void test_bets(void)
{
    setup();

    check_eq(cmd("17"), RI_REDRAW, "a bare number bets on it");
    check_eq(rl_bet_on(&round_, RL_STRAIGHT, 17), 5,
        "for the selected chip value");

    check_eq(cmd("17 20"), RI_REDRAW, "an amount can follow");
    check_eq(rl_bet_on(&round_, RL_STRAIGHT, 17), 25, "and stacks");

    check_eq(cmd("red 10"), RI_REDRAW, "red takes an amount");
    check_eq(rl_bet_on(&round_, RL_RED, 0), 10, "and is placed");

    check_eq(cmd("dozen 2 15"), RI_REDRAW, "a dozen is one-based");
    check_eq(rl_bet_on(&round_, RL_DOZEN, 1), 15, "and zero-based inside");
    check(rl_covers(RL_EURO, RL_DOZEN, 1, 13),
        "so `dozen 2` really is the second dozen");

    check_eq(cmd("column 1 5"), RI_REDRAW, "a column too");
    check(rl_covers(RL_EURO, RL_COLUMN, 0, 1), "and covers 1, 4, 7...");

    check_eq(cmd("street 1 5"), RI_REDRAW, "a street");
    check(rl_covers(RL_EURO, RL_STREET, 0, 1), "covering 1, 2, 3");

    /* Splits and corners by their members, because they have no
     * names. */
    check_eq(cmd("split 17 20"), RI_REDRAW, "a split names its two numbers");
    {
        int i, sel = -1;
        for (i = 0; i < round_.nbets; i++)
            if (round_.bet[i].type == RL_SPLIT) sel = round_.bet[i].sel;
        check(sel >= 0, "and is placed");
        check(rl_covers(RL_EURO, RL_SPLIT, sel, 17) &&
            rl_covers(RL_EURO, RL_SPLIT, sel, 20),
            "covering exactly those two");
    }

    /* 3 and 4 are consecutive and NOT adjacent on the table -- the
     * mistake the command has to refuse clearly rather than accept. */
    check_eq(cmd("split 3 4"), RI_REDRAW, "3 and 4 are refused as a split");
    check(strstr(view.message, "next to each other") != NULL,
        "with a message that says why");

    check_eq(cmd("corner 17"), RI_REDRAW, "a corner names its lowest number");
    check_eq(cmd("corner 36"), RI_REDRAW, "and a bad one is refused");
    check(strstr(view.message, "No corner") != NULL ||
        strstr(view.message, "no corner") != NULL, "with a message");

    check_eq(cmd("37"), RI_REDRAW, "there is no 37");
    check_eq(cmd("00"), RI_REDRAW, "and no double zero on a european wheel");
    check(rl_bet_on(&round_, RL_STRAIGHT, RL_DOUBLE_ZERO) == 0,
        "so nothing was staked on it");
}

static void test_bank_limit(void)
{
    setup();
    view.chips = 30;

    check_eq(cmd("17 20"), RI_REDRAW, "a bet within the bank is taken");
    check_eq(round_.staked, 20, "and staked");

    /* CHIPS ALREADY ON THE TABLE ARE SPOKEN FOR. Checking a new bet
     * against the balance alone would let somebody bet their stack
     * twice over -- the balance has not moved yet, because it moves
     * once, when the wheel is spun. */
    check_eq(cmd("20 20"), RI_REDRAW, "a second bet beyond it is refused");
    check_eq(round_.staked, 20, "and nothing more is staked");
    check(strstr(view.message, "left") != NULL, "with what is left");

    check_eq(cmd("20 10"), RI_REDRAW, "but exactly the remainder is fine");
    check_eq(round_.staked, 30, "and takes the lot");

    check(!rl_can_stake(&view, 1), "with nothing left to stake");
    check(rl_can_stake(&view, 0) == false, "and a zero bet is not a bet");
}

static void test_keys(void)
{
    setup();

    rl_input_key(&view, 'r');
    rl_input_key(&view, 'e');
    rl_input_key(&view, 'd');
    check_eq(view.cmd_len, 3, "typing fills the line");
    rl_input_key(&view, 0x08);
    check_eq(view.cmd_len, 2, "backspace removes");
    rl_input_key(&view, 0x1b);
    check_eq(view.cmd_len, 0, "escape clears");
    rl_input_key(&view, 0x08);
    check_eq(view.cmd_len, 0, "and backspace on an empty line is harmless");

    check_eq(type_line("red"), RI_REDRAW, "a typed line runs on Return");
    check_eq(rl_bet_on(&round_, RL_RED, 0), 5, "and places the bet");

    /* Return on an empty line spins -- but only with something on the
     * table. It commits what is already staked rather than adding to
     * it, which is why it is safe as a single key. */
    check_eq(rl_input_key(&view, '\r'), RI_SPIN, "Return spins");
    rl_round_clear(&round_, RL_EURO);
    check_eq(rl_input_key(&view, '\r'), RI_REDRAW,
        "but not with an empty table");

    /* The chip denomination. */
    view.chip_sel = 0;
    rl_input_key(&view, '+');
    check_eq(view.chip_sel, 1, "plus raises the chip value");
    rl_input_key(&view, '-');
    rl_input_key(&view, '-');
    check_eq(view.chip_sel, 0, "minus lowers it and stops at the bottom");
    view.chip_sel = RL_NCHIPS - 1;
    rl_input_key(&view, '+');
    check_eq(view.chip_sel, RL_NCHIPS - 1, "and stops at the top");

    /* The line cannot overflow. */
    {
        int i;
        for (i = 0; i < RL_CMD_LEN * 3; i++) rl_input_key(&view, 'x');
        check(view.cmd_len < RL_CMD_LEN, "the line cannot overflow");
        check(view.cmd[view.cmd_len] == '\0', "and stays terminated");
    }
}

static void test_clicks(void)
{
    rl_rect_t cell;
    int32_t typed, clicked;

    /* THE CLAIM IN rl_input.h, TESTED. A bet placed by click and the
     * same bet typed must leave the round identical. */
    setup();
    cmd("17");
    typed = rl_bet_on(&round_, RL_STRAIGHT, 17);

    setup();
    rl_board_cell(&L, 17, &cell);
    check_eq(rl_input_click(&view, &L, cell.x + cell.w / 2,
        cell.y + cell.h / 2, false), RI_REDRAW, "clicking a cell bets");
    clicked = rl_bet_on(&round_, RL_STRAIGHT, 17);
    check_eq(clicked, typed, "both routes stake the same");

    /* The right button takes a chip back. */
    check_eq(rl_input_click(&view, &L, cell.x + cell.w / 2,
        cell.y + cell.h / 2, true), RI_REDRAW, "right-click removes");
    check_eq(rl_bet_on(&round_, RL_STRAIGHT, 17), 0, "the whole chip");
    check_eq(rl_input_click(&view, &L, cell.x + cell.w / 2,
        cell.y + cell.h / 2, true), RI_NONE, "and again does nothing");

    /* The chip selector and the two buttons. */
    check_eq(rl_input_click(&view, &L, L.chip[3].x + 2, L.chip[3].y + 2,
        false), RI_REDRAW, "clicking a denomination selects it");
    check_eq(view.chip_sel, 3, "the right one");

    cmd("red");
    check_eq(rl_input_click(&view, &L, L.spin.x + 2, L.spin.y + 2, false),
        RI_SPIN, "clicking spin spins");
    check_eq(rl_input_click(&view, &L, L.clear.x + 2, L.clear.y + 2, false),
        RI_REDRAW, "clicking clear clears");
    check_eq(round_.nbets, 0, "the table");

    /* A click on nothing is nothing -- it must not silently place a
     * chip somewhere. */
    check_eq(rl_input_click(&view, &L, L.ox, L.oy, false), RI_NONE,
        "a click on the title bar does nothing");

    /* And nothing at all responds while the wheel is turning. */
    setup();
    view.spinning = true;
    rl_board_cell(&L, 5, &cell);
    check_eq(rl_input_click(&view, &L, cell.x + 2, cell.y + 2, false),
        RI_NONE, "no bet lands while the wheel is turning");
    check_eq(cmd("17"), RI_REDRAW, "and a typed one is refused");
    check_eq(round_.nbets, 0, "with nothing staked");
}

static void test_settings(void)
{
    setup();

    check_eq(cmd("chip 100"), RI_REDRAW, "the chip value can be set");
    check_eq(view.chip_sel, 3, "to a real denomination");
    check_eq(cmd("chip 7"), RI_REDRAW, "and a fake one is refused");
    check_eq(view.chip_sel, 3, "leaving it alone");

    check_eq(cmd("wheel american"), RI_NEWGAME, "the wheel can be changed");
    check_eq(view.round->wheel, RL_AMERICAN, "to american");
    check_eq(cmd("00 5"), RI_REDRAW, "where the double zero exists");
    check_eq(rl_bet_on(&round_, RL_STRAIGHT, RL_DOUBLE_ZERO), 5,
        "and can be bet on");

    check_eq(cmd("wheel euro"), RI_NEWGAME, "and back");
    check_eq(cmd("wheel sideways"), RI_REDRAW, "a nonsense wheel is refused");

    check_eq(cmd("quit"), RI_QUIT, "quit quits");
    check_eq(cmd("game"), RI_GAME_MODE, "game toggles full screen");
    check_eq(cmd("buyin"), RI_BUYIN, "buyin asks for a top-up");
    check_eq(cmd("wibble"), RI_REDRAW, "an unknown command is refused");
    check(strstr(view.message, "unknown") != NULL, "and says so");
    check_eq(cmd(""), RI_NONE, "an empty line does nothing");
}

int main(void)
{
    printf("roulette: input tests\n");

    if (!z_render_open(&win, 320, 240)) {
        printf("roulette: cannot map the framebuffer here -- skipping\n");
        return 77;
    }

    test_bets();
    test_bank_limit();
    test_keys();
    test_clicks();
    test_settings();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
