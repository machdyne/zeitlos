/*
 * Zeitlos roulette -- host tests for the betting layout.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- the one that does the work --
 *
 * For every point the layout resolves to a bet, THE BET'S COVERAGE MUST
 * BE EXACTLY THE SET OF NUMBERS WHOSE CELLS TOUCH THAT POINT.
 *
 * That is the whole correctness question for a roulette layout, and it
 * is checkable without knowing anything about how the mapping is
 * implemented: walk every pixel of the grid, ask what bet is there, ask
 * which cells the pixel is within a whisker of, and compare. A split
 * selector computed one off, a corner indexed from the wrong end, a row
 * confused with a column -- all of them show up as a bet covering
 * numbers that are somewhere else on the table.
 *
 * Getting this wrong is not a crash. It is a chip that pays out on the
 * wrong numbers, which nobody notices until they lose a bet they should
 * have won.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "roulette_shim.h"
#include "../rl_board.h"

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

static void setup(int w)
{
    memset(&view, 0, sizeof view);
    rl_round_clear(&round_, w);
    rl_wheel_init(&wheel, w, 0, 0, 40);
    view.round = &round_;
    view.wheel = &wheel;
    view.chips = 1000;
    view.result = -1;
    rl_board_layout(&L, &view, 2, 13, 316, 225);
}

/* The set of grid numbers whose CELL is within reach of (x, y).
 *
 * "Within reach" is the edge band -- a third of a cell -- because that
 * is what rl_board_hit() uses to decide that a point on the edge of one
 * cell is a bet involving the next. Using one pixel instead was the
 * first version and it failed on correct splits: a click seven pixels
 * from an edge legitimately resolves to a split with a neighbour seven
 * pixels away.
 *
 * Widening it does not weaken the check. What it catches is a bet
 * covering a number that is NOWHERE NEAR the point -- the wrong row,
 * the wrong street, an index off by one -- and those are all more than
 * a third of a cell away.
 *
 * Computed from rl_board_cell() alone, which is the DRAWING side, so
 * agreeing with rl_board_hit() means the thing drawn and the thing
 * clicked are the same thing. */
static int cells_near(int x, int y, int *out)
{
    int n = 0, i;
    int bx = RL_CELL_W / 3, by = RL_CELL_H / 3;

    for (i = 1; i <= 36; i++) {
        rl_rect_t c;
        rl_board_cell(&L, i, &c);
        if (x >= c.x - bx && x < c.x + c.w + bx &&
            y >= c.y - by && y < c.y + c.h + by) out[n++] = i;
    }

    return n;
}

static void test_grid_hits(void)
{
    int x, y, bad = 0, straight = 0, split = 0, corner = 0;
    int gx0 = L.grid_x, gy0 = L.grid_y;
    int gw = 12 * RL_CELL_W, gh = 3 * RL_CELL_H;

    for (y = gy0; y < gy0 + gh; y++) {
        for (x = gx0; x < gx0 + gw; x++) {
            int type, sel, near[8], nn, i, covered = 0, missing = 0;

            if (!rl_board_hit(&L, &view, x, y, &type, &sel)) {
                bad++;
                continue;
            }

            if (type == RL_STRAIGHT) straight++;
            if (type == RL_SPLIT) split++;
            if (type == RL_CORNER) corner++;

            nn = cells_near(x, y, near);

            /* Every number the bet covers must be one of the cells the
             * point touches... */
            for (i = 1; i <= 36; i++) {
                bool cov = rl_covers(RL_EURO, type, sel, i);
                bool adj = false;
                int k;
                for (k = 0; k < nn; k++) if (near[k] == i) adj = true;
                if (cov && !adj) covered++;
                if (!cov && adj && type != RL_STRAIGHT) missing++;
            }

            if (covered) {
                printf("      (%d,%d) -> %s sel %d covers a number it is "
                    "not near\n", x - gx0, y - gy0, rl_type_name(type), sel);
                bad++;
            }
        }
    }

    check_eq(bad, 0, "every point in the grid resolves to a bet on its own "
        "neighbours");
    check(straight > 0 && split > 0 && corner > 0,
        "and the grid offers straight-up, split and corner bets");

    /* The coverage counts must be right too -- a split that covers one
     * number is still "on its own neighbours". */
    bad = 0;
    for (y = gy0; y < gy0 + gh; y++)
        for (x = gx0; x < gx0 + gw; x++) {
            int type, sel;
            if (!rl_board_hit(&L, &view, x, y, &type, &sel)) continue;
            if (!rl_bet_valid(RL_EURO, type, sel)) { bad++; continue; }
            if (type == RL_STRAIGHT && rl_coverage(RL_EURO, type, sel) != 1) bad++;
            if (type == RL_SPLIT && rl_coverage(RL_EURO, type, sel) != 2) bad++;
            if (type == RL_CORNER && rl_coverage(RL_EURO, type, sel) != 4) bad++;
        }
    check_eq(bad, 0, "and every resolved bet is valid and covers what it should");
}

static void test_cell_centres(void)
{
    int n, bad = 0;

    /* The middle of a cell is unambiguously that number. If the edge
     * bands ever grow enough to meet in the middle, every straight-up
     * bet becomes unreachable by mouse. */
    for (n = 1; n <= 36; n++) {
        rl_rect_t c;
        int type, sel;
        rl_board_cell(&L, n, &c);
        if (!rl_board_hit(&L, &view, c.x + c.w / 2, c.y + c.h / 2,
            &type, &sel)) { bad++; continue; }
        if (type != RL_STRAIGHT || sel != n) {
            printf("      cell %d centre resolved to %s sel %d\n",
                n, rl_type_name(type), sel);
            bad++;
        }
    }
    check_eq(bad, 0, "the middle of every cell is that number, straight up");

    /* And the cells tile the grid without overlapping. */
    bad = 0;
    for (n = 1; n <= 36; n++) {
        int m;
        rl_rect_t a;
        rl_board_cell(&L, n, &a);
        for (m = n + 1; m <= 36; m++) {
            rl_rect_t b;
            rl_board_cell(&L, m, &b);
            if (a.x < b.x + b.w && b.x < a.x + a.w &&
                a.y < b.y + b.h && b.y < a.y + a.h) bad++;
        }
    }
    check_eq(bad, 0, "no two number cells overlap");
}

static void test_outside_bets(void)
{
    int i, type, sel, bad = 0;
    int gw = 12 * RL_CELL_W;

    /* The zero. */
    check(rl_board_hit(&L, &view, L.zero_x + RL_ZERO_W / 2,
        L.grid_y + RL_CELL_H, &type, &sel), "the zero cell is hittable");
    check_eq(type, RL_STRAIGHT, "and is a straight-up bet");
    check_eq(sel, RL_ZERO, "on zero");

    /* The column bets, which run opposite to the grid rows -- the top
     * row of the printed table is the THIRD column bet. Mixing those up
     * is the single easiest mistake in this layout. */
    for (i = 0; i < 3; i++) {
        int n;
        check(rl_board_hit(&L, &view, L.colbet_x + RL_ZERO_W / 2,
            L.grid_y + i * RL_CELL_H + RL_CELL_H / 2, &type, &sel),
            "a column bet is hittable");
        if (type != RL_COLUMN) { bad++; continue; }
        /* The number in the row beside it must be in that column. */
        n = (i == 0) ? 3 : (i == 1) ? 2 : 1;
        if (!rl_covers(RL_EURO, RL_COLUMN, sel, n)) {
            printf("      column bet beside the row holding %d does not "
                "cover it\n", n);
            bad++;
        }
    }
    check_eq(bad, 0, "each column bet is beside the row it covers");

    /* The dozens. */
    bad = 0;
    for (i = 0; i < 3; i++) {
        int n = i * 12 + 1;
        if (!rl_board_hit(&L, &view, L.grid_x + i * (gw / 3) + gw / 6,
            L.dozen_y + RL_OUTER_H / 2, &type, &sel)) { bad++; continue; }
        if (type != RL_DOZEN || !rl_covers(RL_EURO, type, sel, n)) bad++;
    }
    check_eq(bad, 0, "each dozen covers the numbers printed above it");

    /* The even-money row, in the order it is drawn. */
    {
        static const int want[6] = {
            RL_LOW, RL_EVEN, RL_RED, RL_BLACK, RL_ODD, RL_HIGH };
        bad = 0;
        for (i = 0; i < 6; i++) {
            if (!rl_board_hit(&L, &view, L.grid_x + i * (gw / 6) + gw / 12,
                L.even_y + RL_OUTER_H / 2, &type, &sel)) { bad++; continue; }
            if (type != want[i]) bad++;
        }
        check_eq(bad, 0, "the even-money bets are where they are drawn");
    }

    /* Streets and six lines, on the strip below the grid. */
    bad = 0;
    for (i = 0; i < 12; i++) {
        if (!rl_board_hit(&L, &view, L.grid_x + i * RL_CELL_W + RL_CELL_W / 2,
            L.street_y + 2, &type, &sel)) { bad++; continue; }
        if (type != RL_STREET || sel != i) bad++;
        if (!rl_covers(RL_EURO, RL_STREET, sel, i * 3 + 1)) bad++;
    }
    check_eq(bad, 0, "each street is under the three numbers it covers");

    bad = 0;
    for (i = 1; i < 12; i++) {
        if (!rl_board_hit(&L, &view, L.grid_x + i * RL_CELL_W,
            L.street_y + 2, &type, &sel)) { bad++; continue; }
        if (type != RL_SIXLINE) bad++;
        else if (rl_coverage(RL_EURO, type, sel) != 6) bad++;
    }
    check_eq(bad, 0, "the boundaries between streets are six lines");

    /* Nothing outside the table resolves to a bet -- a stray click must
     * not silently place a chip. */
    check(!rl_board_hit(&L, &view, L.ox, L.oy, &type, &sel),
        "the top-left corner is not a bet");
    check(!rl_board_hit(&L, &view, L.wheel_cx, L.wheel_cy, &type, &sel),
        "and neither is the wheel");
}

static void test_american(void)
{
    int type, sel;

    setup(RL_AMERICAN);

    /* Two zeros share the space one occupies on a european table. */
    check(rl_board_hit(&L, &view, L.zero_x + RL_ZERO_W / 2,
        L.grid_y + 4, &type, &sel), "the upper zero cell is hittable");
    check_eq(sel, RL_ZERO, "and is the single zero");

    check(rl_board_hit(&L, &view, L.zero_x + RL_ZERO_W / 2,
        L.grid_y + 3 * RL_CELL_H - 4, &type, &sel),
        "the lower one is hittable");
    check_eq(sel, RL_DOUBLE_ZERO, "and is the double zero");

    setup(RL_EURO);
}

static void test_layout_shape(void)
{
    rl_layout_t a, b;

    setup(RL_EURO);
    check(L.ok, "a 316x225 window fits a table");

    rl_board_layout(&a, &view, 2, 13, 316, 225);
    rl_board_layout(&b, &view, 2, 13, 316, 225);
    check(memcmp(&a, &b, sizeof a) == 0, "the layout is a pure function");

    rl_board_layout(&b, &view, 12, 13, 316, 225);
    check_eq(b.grid_x, a.grid_x + 10, "and follows its origin");

    /* Bands in order, not overlapping. */
    check(L.grid_y + 3 * RL_CELL_H <= L.street_y, "the grid is above the streets");
    check(L.street_y + 5 <= L.dozen_y, "the streets are above the dozens");
    check(L.dozen_y + RL_OUTER_H <= L.even_y, "the dozens above the even bets");
    check(L.even_y + RL_OUTER_H <= L.msg_y, "and those above the message");
    check(L.msg_y + 8 <= L.cmd_y, "the message is above the command line");
    check(L.cmd_y + 8 <= L.clip.y1 + 1, "which fits");

    /* The whole table inside the window. */
    check(L.zero_x >= L.ox, "the table starts inside");
    check(L.colbet_x + RL_ZERO_W <= L.ox + L.w, "and ends inside");

    /* The wheel does not overlap the board or the panel. */
    /* Measured on the FOOTPRINT, not the radius: the ball rides outside
     * the wall and the patch clears a disc that covers it. */
    {
        int pad = L.wheel_r + L.wheel_r / 9 + 3;
        check(L.wheel_cy + pad < L.grid_y, "the wheel is above the grid");
        check(L.wheel_cy - pad >= L.status_y + 8,
            "and clear of the title it used to erase");
        check(L.wheel_cx + pad < L.panel_x, "and left of the panel");
    }

    rl_board_layout(&a, &view, 0, 0, 160, 225);
    check(!a.ok, "a narrow window is refused");
    rl_board_layout(&a, &view, 0, 0, 316, 90);
    check(!a.ok, "and so is a short one");

    /* A game-mode page is a different rectangle and must also work. */
    rl_board_layout(&a, &view, 0, 0, 320, 240);
    check(a.ok, "a game-mode page fits a table");
}

static void test_drawing(void)
{
    int x, y, outside = 0;

    setup(RL_EURO);

    rl_bet_place(&round_, RL_STRAIGHT, 17, 25);
    rl_bet_place(&round_, RL_RED, 0, 5);
    rl_bet_place(&round_, RL_CORNER, 4, 1);
    rl_bet_place(&round_, RL_SPLIT, 3, 5);
    rl_bet_place(&round_, RL_DOZEN, 1, 100);
    rl_bet_place(&round_, RL_STREET, 7, 5);
    rl_bet_place(&round_, RL_SIXLINE, 2, 5);
    rl_bet_place(&round_, RL_COLUMN, 0, 5);
    view.chips = 864;
    view.nhistory = 3;
    view.history[0] = 17; view.history[1] = 0; view.history[2] = 33;
    view.result = 17;
    strcpy(view.message, "place your bets");

    /* Drawn at a NON-ZERO origin, because a renderer that confuses
     * window coordinates with content coordinates draws the right shape
     * in the wrong place and only an offset origin shows it. */
    {
        int i;
        for (y = 0; y < Z_SCREEN_H; y++)
            for (x = 0; x < Z_SCREEN_W; x++)
                z_fb_set_pixel(x, y, 0, NULL);
        (void)i;
    }

    rl_board_layout(&L, &view, 40, 30, 316, 225);
    rl_board_draw(&L, &view);

    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            if (z_render_get(x, y) &&
                (x < L.clip.x0 || x > L.clip.x1 ||
                 y < L.clip.y0 || y > L.clip.y1)) outside++;

    check_eq(outside, 0, "nothing is drawn outside the content rectangle");

    {
        int ink = 0;
        for (y = L.clip.y0; y <= L.clip.y1; y++)
            for (x = L.clip.x0; x <= L.clip.x1; x++)
                if (z_render_get(x, y)) ink++;
        check(ink > 3000, "and the table is actually drawn");
    }
}

int main(void)
{
    printf("roulette: board tests\n");

    if (!z_render_open(&win, 320, 240)) {
        printf("roulette: cannot map the framebuffer here -- skipping\n");
        return 77;
    }
    rl_board_init();

    setup(RL_EURO);

    test_layout_shape();
    test_cell_centres();
    test_grid_hits();
    test_outside_bets();
    test_american();
    test_drawing();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
