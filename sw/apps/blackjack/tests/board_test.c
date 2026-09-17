/*
 * Zeitlos blackjack -- host tests for the table.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The geometry assertions, plus the one that earns its keep: NOTHING IS
 * DRAWN OUTSIDE THE CONTENT RECTANGLE. sw/apps/logic shipped its panel
 * wrong three times and all three were that -- window coordinates used
 * where content coordinates were needed. Checking each rectangle
 * individually cannot catch the general case; drawing the whole thing
 * at a non-zero origin and looking for ink where there should be none
 * can.
 *
 * The blackjack-specific worry is the CARD STRIDE. A hand has no fixed
 * length -- twelve cards is reachable -- so a stride that suits two
 * runs off the table at seven. The tests walk every hand length from
 * one to twelve, at every hand count from one to four.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "blackjack_shim.h"
#include "../bj_board.h"

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
static bj_view_t view;
static bj_layout_t L;
static z_win_t win;

static void setup(void)
{
    bj_rules_t r;
    memset(&view, 0, sizeof view);
    bj_rules_default(&r);
    bj_game_init(&g, &r);
    view.g = &g;
    view.chips = 1000;
    view.bet = 25;
    view.chip_sel = 1;
    strcpy(view.message, "deal when ready");
    bj_board_layout(&L, &view, 2, 13, 316, 225);
}

static int ink_in(int x0, int y0, int w, int h)
{
    int x, y, n = 0;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            if (z_render_get(x, y)) n++;
    return n;
}

static void clear_all(void)
{
    int x, y;
    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            z_fb_set_pixel(x, y, 0, NULL);
}

static void test_layout(void)
{
    bj_layout_t a, b;

    setup();
    check(L.ok, "a 316x225 window fits a table");

    bj_board_layout(&a, &view, 2, 13, 316, 225);
    bj_board_layout(&b, &view, 2, 13, 316, 225);
    check(memcmp(&a, &b, sizeof a) == 0, "the layout is a pure function");
    bj_board_layout(&b, &view, 12, 13, 316, 225);
    check_eq(b.dealer_x, a.dealer_x + 10, "and follows its origin");

    check(L.status_y < L.dealer_y, "status above the dealer");
    check(L.dealer_y + Z_ART_FULL_H <= L.dealer_info_y,
        "the dealer's cards are above their total");
    check(L.dealer_info_y + 8 <= L.hands_y, "which is above the hands");
    check(L.hands_y + Z_ART_FULL_H <= L.hand_info_y,
        "the hands are above their totals");
    check(L.hand_info_y + 8 <= L.shoe_y, "which are above the shoe bar");
    check(L.chip[0].y + BJ_BTN_H <= L.btn[0].y, "chips above the buttons");
    check(L.btn[0].y + BJ_BTN_H <= L.msg_y, "buttons above the message");
    check(L.msg_y + 8 <= L.cmd_y, "message above the command line");
    check(L.cmd_y + 8 <= L.clip.y1 + 1, "which fits");

    {
        int i;
        for (i = 0; i < BJ_NBTN; i++) {
            check(L.btn[i].x >= L.ox, "a button starts inside");
            check(L.btn[i].x + L.btn[i].w <= L.ox + L.w, "and ends inside");
            if (i) check(L.btn[i].x >= L.btn[i - 1].x + L.btn[i - 1].w,
                "and does not overlap its neighbour");
        }
        for (i = 0; i < BJ_NCHIPS; i++) {
            check(L.chip[i].x >= L.ox, "a chip button starts inside");
            check(L.chip[i].x + L.chip[i].w <= L.ox + L.w, "and ends inside");
        }
        /* The shoe has its own band above the chips now. It was beside
         * them, and its caption landed on the same row as the hands'
         * totals -- which no assertion here noticed, because two pieces
         * of text overlapping is not a rectangle overlapping. */
        check(L.shoe_y + 6 <= L.chip[0].y, "the shoe bar is above the chips");
        check(L.shoe_x + L.shoe_w <= L.ox + L.w, "and fits the width");
        check(L.hand_info_y + 8 <= L.shoe_y - 8,
            "and its caption clears the hands' totals");
    }

    bj_board_layout(&a, &view, 0, 0, 200, 225);
    check(!a.ok, "a narrow window is refused");
    bj_board_layout(&a, &view, 0, 0, 316, 100);
    check(!a.ok, "and a short one");
    bj_board_layout(&a, &view, 0, 0, 320, 240);
    check(a.ok, "a game-mode page fits");
}

/* -- the stride ------------------------------------------------------------ */

static void test_stride(void)
{
    int nh, n, bad = 0;

    /* EVERY HAND LENGTH AT EVERY HAND COUNT must stay inside its cell.
     * A blackjack hand can reach twelve cards, and a stride chosen for
     * two runs off the table long before that. */
    for (nh = 1; nh <= BJ_MAX_HANDS; nh++) {
        int i;
        setup();
        g.nhands = nh;
        for (i = 0; i < nh; i++) g.hand[i].bet = 25;

        for (n = 1; n <= BJ_MAX_CARDS; n++) {
            int x, y, stride;
            bool mini;
            int cw, right, cell_right;

            for (i = 0; i < nh; i++) g.hand[i].n = n;
            bj_board_layout(&L, &view, 2, 13, 316, 225);

            for (i = 0; i < nh; i++) {
                bj_hand_geom(&L, &view, i, &x, &y, &stride, &mini);
                cw = mini ? Z_ART_MINI_W : Z_ART_FULL_W;
                right = x + (n - 1) * stride + cw;
                cell_right = L.ox + (i + 1) * L.hand_w;
                if (right > cell_right) {
                    printf("      %d hands of %d cards: hand %d ends at %d, "
                        "cell ends at %d\n", nh, n, i, right - L.ox,
                        cell_right - L.ox);
                    bad++;
                }
                if (stride < 4) bad++;
            }
        }
    }

    check_eq(bad, 0, "every hand fits its cell at every length");

    /* One hand gets full cards; several get minis. */
    setup();
    g.nhands = 1; g.hand[0].n = 2;
    bj_board_layout(&L, &view, 2, 13, 316, 225);
    {
        int x, y, s; bool mini;
        bj_hand_geom(&L, &view, 0, &x, &y, &s, &mini);
        check(!mini, "a single hand is drawn with full cards");
    }
    g.nhands = 3;
    bj_board_layout(&L, &view, 2, 13, 316, 225);
    {
        int x, y, s; bool mini;
        bj_hand_geom(&L, &view, 0, &x, &y, &s, &mini);
        check(mini, "and split hands with small ones");
    }
}

static void test_hits(void)
{
    int i;

    setup();

    for (i = 0; i < BJ_NBTN; i++) {
        int cx = L.btn[i].x + L.btn[i].w / 2;
        int cy = L.btn[i].y + L.btn[i].h / 2;
        check_eq(bj_board_btn_at(&L, cx, cy), i, "a button's centre hits it");
    }
    check_eq(bj_board_btn_at(&L, L.btn[0].x, L.btn[0].y - 1), -1,
        "a point above the buttons hits nothing");

    for (i = 0; i < BJ_NCHIPS; i++) {
        int cx = L.chip[i].x + L.chip[i].w / 2;
        int cy = L.chip[i].y + L.chip[i].h / 2;
        check_eq(bj_board_chip_at(&L, cx, cy), i, "a chip's centre hits it");
    }
    check_eq(bj_board_chip_at(&L, L.ox, L.oy), -1,
        "and the corner hits no chip");
}

static void test_drawing(void)
{
    int x, y, outside = 0;
    int guard = 0;

    setup();
    bj_round_begin(&g, 25);
    while (g.phase == BJ_PHASE_INSURANCE && guard++ < 3) bj_insure(&g, 0);

    clear_all();
    /* At a NON-ZERO origin: a renderer that confuses window coordinates
     * with content coordinates draws the right shape in the wrong
     * place, and only an offset origin shows it. */
    bj_board_layout(&L, &view, 40, 30, 316, 225);
    bj_board_draw(&L, &view);

    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            if (z_render_get(x, y) &&
                (x < L.clip.x0 || x > L.clip.x1 ||
                 y < L.clip.y0 || y > L.clip.y1)) outside++;

    check_eq(outside, 0, "nothing is drawn outside the content rectangle");
    check(ink_in(L.clip.x0, L.clip.y0, L.w, L.h) > 1500,
        "and the table is actually drawn");

    /* THE HOLE CARD STAYS DOWN. Drawing it face up and relying on the
     * player not to look is the same bug as dealing it face up, and it
     * is invisible to every geometry assertion. */
    if (g.phase == BJ_PHASE_PLAYER && g.ndealer >= 2) {
        static uint8_t down[Z_ART_FULL_W * Z_ART_FULL_H];
        int n = 0, px, py, diff = 0;
        const uint32_t *back = z_back_full;
        int hx = L.dealer_x + Z_ART_FULL_W + 2;

        for (py = 0; py < Z_ART_FULL_H; py++)
            for (px = 0; px < Z_ART_FULL_W; px++)
                down[n++] = (uint8_t)z_render_get(hx + px, L.dealer_y + py);

        n = 0;
        for (py = 0; py < Z_ART_FULL_H; py++)
            for (px = 0; px < Z_ART_FULL_W; px++, n++)
                if (down[n] != (uint8_t)((back[py] >> px) & 1u)) diff++;

        check_eq(diff, 0, "the hole card is drawn face down, exactly");
    }

    /* And once the round is over it is shown. */
    while (g.phase != BJ_PHASE_DONE && guard++ < 40) {
        bj_options_t o;
        bj_options(&g, &o);
        if (o.can_stand) bj_act(&g, BJ_STAND);
        else break;
    }

    if (g.phase == BJ_PHASE_DONE && g.ndealer >= 2) {
        clear_all();
        bj_board_draw(&L, &view);
        {
            const uint32_t *back = z_back_full;
            int hx = L.dealer_x + Z_ART_FULL_W + 2;
            int px, py, diff = 0;
            for (py = 0; py < Z_ART_FULL_H; py++)
                for (px = 0; px < Z_ART_FULL_W; px++)
                    if (z_render_get(hx + px, L.dealer_y + py) !=
                        (int)((back[py] >> px) & 1u)) diff++;
            check(diff > 0, "and face up once the round is over");
        }
    }

    /* A layout too small to use must still draw without escaping. */
    clear_all();
    bj_board_layout(&L, &view, 40, 40, 120, 60);
    bj_board_draw(&L, &view);
    outside = 0;
    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            if (z_render_get(x, y) &&
                (x < L.clip.x0 || x > L.clip.x1 ||
                 y < L.clip.y0 || y > L.clip.y1)) outside++;
    check_eq(outside, 0, "a refused layout stays inside its rectangle too");
}

int main(void)
{
    printf("blackjack: board tests\n");

    if (!z_render_open(&win, 320, 240)) {
        printf("blackjack: cannot map the framebuffer here -- skipping\n");
        return 77;
    }
    bj_board_init();

    test_layout();
    test_stride();
    test_hits();
    test_drawing();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
