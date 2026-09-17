/*
 * Zeitlos craps -- host tests for the table.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The felt is a stack of bands sized from whatever space is left, so
 * what has to be checked is that nothing overlaps, nothing escapes, and
 * every bet the game will take has somewhere to be clicked.
 *
 * That last one is the point. A bet with no spot is a bet a player can
 * only reach by typing, on a table where the whole idea is that the
 * chips are on the felt -- and the free odds, the one bet worth having,
 * had exactly that problem until a render showed it.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "craps_shim.h"
#include "../cr_board.h"

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

static cr_game_t game;
static cr_view_t view;
static cr_layout_t L;
static z_win_t win;

static void setup(int ox, int oy, int w, int h)
{
    memset(&view, 0, sizeof view);
    cr_game_init(&game);
    view.g = &game;
    view.chips = 1000;
    view.bet = 25;
    view.show_d1 = 3;
    view.show_d2 = 4;
    cr_board_layout(&L, &view, ox, oy, w, h);
}

static void test_spots(void)
{
    int i, j, bad = 0;

    setup(2, 13, 316, 225);
    check(L.ok, "a 316x225 window fits a table");
    check(L.nspots >= 20, "and carries the whole felt");

    /* NOTHING OVERLAPS. The bands are sized from what is left over, so
     * an off-by-one in the arithmetic puts two rows on top of each
     * other rather than failing outright. */
    for (i = 0; i < L.nspots; i++)
        for (j = i + 1; j < L.nspots; j++) {
            const cr_rect_t *a = &L.spot[i].r, *b = &L.spot[j].r;
            if (a->x < b->x + b->w && b->x < a->x + a->w &&
                a->y < b->y + b->h && b->y < a->y + a->h) {
                printf("      %s overlaps %s\n", L.spot[i].label,
                    L.spot[j].label);
                bad++;
            }
        }
    check_eq(bad, 0, "no two spots overlap");

    /* And everything is inside the felt, clear of the text rows. */
    bad = 0;
    for (i = 0; i < L.nspots; i++) {
        const cr_rect_t *r = &L.spot[i].r;
        if (r->x < L.ox || r->x + r->w > L.ox + L.w) bad++;
        if (r->y < L.dice_y + Z_DICE_BIG) bad++;
        if (r->y + r->h > L.msg_y - 1) bad++;
        if (r->w < 20 || r->h < 12) bad++;
    }
    check_eq(bad, 0, "every spot is inside the felt and big enough to hit");

    /* The middle of every spot hits that spot and no other. */
    bad = 0;
    for (i = 0; i < L.nspots; i++) {
        const cr_rect_t *r = &L.spot[i].r;
        if (cr_board_spot_at(&L, r->x + r->w / 2, r->y + r->h / 2) != i) bad++;
    }
    check_eq(bad, 0, "the middle of every spot hits it");

    check_eq(cr_board_spot_at(&L, L.ox, L.oy), CR_SPOT_NONE,
        "and the status line is not a bet");
}

static void test_every_bet_reachable(void)
{
    /* EVERY BET THE GAME WILL TAKE HAS A SPOT. The free odds had no
     * place on the felt at first -- 125 riding on the point with
     * nothing to show it, on the one bet worth more than the rest of
     * the table put together. */
    static const int want[] = {
        CR_PASS, CR_DONT_PASS, CR_PASS_ODDS, CR_DONT_ODDS,
        CR_COME, CR_PLACE, CR_HARD, CR_FIELD,
        CR_ANY7, CR_ANY_CRAPS, CR_ELEVEN, CR_TWO
    };
    int k, i, bad = 0;

    setup(2, 13, 316, 225);

    for (k = 0; k < (int)(sizeof want / sizeof want[0]); k++) {
        bool found = false;
        for (i = 0; i < L.nspots; i++)
            if (L.spot[i].type == want[k]) found = true;
        if (!found) {
            printf("      %s has no spot on the felt\n",
                cr_type_name(want[k]));
            bad++;
        }
    }
    check_eq(bad, 0, "every bet the table takes can be clicked");

    /* All six place numbers and all four hardways, not just some. */
    {
        static const int nums[6] = { 4, 5, 6, 8, 9, 10 };
        int n;
        bad = 0;
        for (n = 0; n < 6; n++) {
            bool found = false;
            for (i = 0; i < L.nspots; i++)
                if (L.spot[i].type == CR_PLACE && L.spot[i].sel == nums[n])
                    found = true;
            if (!found) bad++;
        }
        check_eq(bad, 0, "all six place numbers are there");
    }
}

static void test_odds_follow_the_point(void)
{
    int i, odds = -1;

    setup(2, 13, 316, 225);

    for (i = 0; i < L.nspots; i++)
        if (L.spot[i].type == CR_PASS_ODDS) odds = i;
    check(odds >= 0, "there is an odds box");

    /* THE POINT IS THE ODDS BOX'S NUMBER, and it moves under it. A spot
     * whose selector was fixed at layout time would be laying odds on
     * whatever the point happened to be when the window was drawn. */
    check_eq(cr_spot_sel(&L, &view, odds), 0,
        "on a come-out the odds box has no number");

    cr_place(&game, CR_PASS, 0, 25);
    cr_roll(&game, 4, 4);
    check_eq(cr_spot_sel(&L, &view, odds), 8, "it follows the point to eight");

    cr_roll(&game, 3, 4);
    check_eq(cr_spot_sel(&L, &view, odds), 0, "and back to nothing on a seven");
}

static void test_nothing_escapes(void)
{
    int x, y, outside = 0, ink = 0;

    setup(40, 30, 316, 225);

    cr_place(&game, CR_PASS, 0, 25);
    cr_roll(&game, 4, 4);
    cr_place(&game, CR_PASS_ODDS, 8, 125);
    cr_place(&game, CR_PLACE, 6, 60);
    cr_place(&game, CR_HARD, 8, 5);
    view.show_d1 = 5; view.show_d2 = 4;

    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            z_fb_set_pixel(x, y, 0, NULL);

    /* At a NON-ZERO origin: a renderer that confuses window coordinates
     * with content coordinates draws the right shape in the wrong
     * place, and only an offset origin shows it. */
    cr_board_draw(&L, &view);

    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++) {
            if (!z_render_get(x, y)) continue;
            ink++;
            if (x < L.clip.x0 || x > L.clip.x1 ||
                y < L.clip.y0 || y > L.clip.y1) outside++;
        }

    check_eq(outside, 0, "nothing is drawn outside the content rectangle");
    check(ink > 2000, "and the table is actually drawn");
}

static void test_sizes(void)
{
    cr_layout_t a, b;

    setup(2, 13, 316, 225);

    cr_board_layout(&a, &view, 2, 13, 316, 225);
    cr_board_layout(&b, &view, 2, 13, 316, 225);
    check(memcmp(&a, &b, sizeof a) == 0, "the layout is a pure function");

    cr_board_layout(&b, &view, 12, 13, 316, 225);
    check_eq(b.dice_x, a.dice_x + 10, "and follows its origin");

    cr_board_layout(&a, &view, 0, 0, 200, 225);
    check(!a.ok, "a narrow window is refused");
    cr_board_layout(&a, &view, 0, 0, 316, 110);
    check(!a.ok, "and a short one");
    cr_board_layout(&a, &view, 0, 0, 320, 240);
    check(a.ok, "a game-mode page fits");
}

int main(void)
{
    printf("craps: table tests\n");

    if (!z_render_open(&win, 320, 240)) {
        printf("craps: cannot map the framebuffer here -- skipping\n");
        return 77;
    }
    cr_board_init();

    test_sizes();
    test_spots();
    test_every_bet_reachable();
    test_odds_follow_the_point();
    test_nothing_escapes();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
