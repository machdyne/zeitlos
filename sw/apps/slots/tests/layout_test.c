/*
 * Zeitlos slots -- host tests for the layout and what lands on screen.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- the one that earns its keep --
 *
 * NOTHING OVERLAPS ANYTHING ELSE. The payline markers sat with their
 * right border on the same column as the reel frame, and shared a wall
 * with each other: a full repaint drew the frame through the marker and
 * the digit next to it came and went depending on what had been drawn
 * last. It looked like a rendering fault and it was a layout one.
 *
 * Checked as arithmetic on the rectangles rather than by counting
 * pixels, so the failure names the two things that touch.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "slots_shim.h"
#include "../sl_board.h"

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

static sl_spin_t spin;
static sl_view_t view;
static sl_layout_t L;
static z_win_t win;

static void setup(int ox, int oy, int w, int h)
{
    memset(&view, 0, sizeof view);
    sl_spin_init(&spin);
    view.spin = &spin;
    view.chips = 1000;
    view.bet = 5;
    view.lines = SL_LINES;
    sl_board_layout(&L, &view, ox, oy, w, h);
}

static void test_marks_clear(void)
{
    int i, bad = 0;
    int frame_l = L.reel_x - 2;
    int frame_r = L.reel_x + SL_REELS * L.reel_pitch - 6 + 1;

    setup(2, 13, 316, 225);
    frame_l = L.reel_x - 2;
    frame_r = L.reel_x + (SL_REELS * L.reel_pitch - (L.reel_pitch - SL_ART_W))
        + 1;

    /* Every marker clears the reel frame on its side. */
    for (i = 0; i < SL_LINES; i++) {
        int x, y;

        sl_mark_pos(&L, i, false, &x, &y);
        if (x < L.ox) bad++;
        if (x + SL_MARK_W > frame_l) {
            printf("      left marker %d ends at %d, reel frame at %d\n",
                i + 1, x + SL_MARK_W - 1, frame_l);
            bad++;
        }

        sl_mark_pos(&L, i, true, &x, &y);
        if (x < frame_r) {
            printf("      right marker %d starts at %d, reel frame at %d\n",
                i + 1, x, frame_r);
            bad++;
        }
        if (x + SL_MARK_W > L.ox + L.w) bad++;
    }
    check_eq(bad, 0, "no marker touches the reel frame");

    /* And no two markers touch each other -- they shared a border,
     * which is why a pair read as one box with two digits in it. */
    bad = 0;
    for (i = 0; i < SL_LINES; i++) {
        int j, side;
        for (side = 0; side < 2; side++) {
            int xi, yi;
            sl_mark_pos(&L, i, side != 0, &xi, &yi);
            for (j = i + 1; j < SL_LINES; j++) {
                int xj, yj;
                sl_mark_pos(&L, j, side != 0, &xj, &yj);
                if (xi < xj + SL_MARK_W && xj < xi + SL_MARK_W &&
                    yi < yj + SL_MARK_W && yj < yi + SL_MARK_W) {
                    printf("      markers %d and %d overlap on the %s\n",
                        i + 1, j + 1, side ? "right" : "left");
                    bad++;
                }
            }
        }
    }
    check_eq(bad, 0, "no two markers overlap");

    /* Every marker sits beside the row its line passes through, which
     * is the whole reason they are there. */
    bad = 0;
    for (i = 0; i < SL_LINES; i++) {
        int rows[SL_REELS], x, y;
        sl_line_rows(i, rows);
        sl_mark_pos(&L, i, false, &x, &y);
        if (y < L.reel_y + rows[0] * SL_CELL) bad++;
        if (y + SL_MARK_W > L.reel_y + (rows[0] + 1) * SL_CELL) bad++;
        sl_mark_pos(&L, i, true, &x, &y);
        if (y < L.reel_y + rows[SL_REELS - 1] * SL_CELL) bad++;
    }
    check_eq(bad, 0, "each marker sits in the row its line passes through");

    /* The handle clears the reels and stays inside the window. */
    check(L.lever.x >= frame_r + 1, "the handle clears the reel frame");
    check(L.lever.x + L.lever.w <= L.pay_x, "and clears the paytable");
    check(sl_board_lever_at(&L, L.lever.x + 5, L.lever.y + 20),
        "the handle can be clicked");
    check(!sl_board_lever_at(&L, L.reel_x, L.reel_y),
        "and a click on the reels is not a pull");
}

static void test_nothing_escapes(void)
{
    int x, y, outside = 0, ink = 0;
    int stops[SL_REELS] = { 20, 11, 20 };
    int guard = 0;

    setup(40, 30, 316, 225);

    sl_spin_begin(&spin, stops);
    while (sl_spin_step(&spin) && guard++ < 500) { }
    view.have_result = true;
    view.last_win = sl_evaluate(stops, view.bet, view.lines, view.line_pays);

    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            z_fb_set_pixel(x, y, 0, NULL);

    /* Drawn at a NON-ZERO origin: a renderer that confuses window
     * coordinates with content coordinates draws the right shape in the
     * wrong place, and only an offset origin shows it. */
    sl_board_draw(&L, &view);

    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++) {
            if (!z_render_get(x, y)) continue;
            ink++;
            if (x < L.clip.x0 || x > L.clip.x1 ||
                y < L.clip.y0 || y > L.clip.y1) outside++;
        }

    check_eq(outside, 0, "nothing is drawn outside the content rectangle");
    check(ink > 2000, "and the machine is actually drawn");
}

static void test_digits_survive(void)
{
    int stops[SL_REELS] = { 20, 11, 20 };
    int i, guard = 0, bad = 0;

    /* THE SYMPTOM, ASSERTED DIRECTLY: every marker still has ink in it
     * after a full repaint, in both the idle and the just-won states.
     * A digit that has been drawn over is a digit with nothing left. */
    setup(2, 13, 316, 225);

    for (i = 0; i < 2; i++) {
        int k;

        if (i == 1) {
            sl_spin_begin(&spin, stops);
            while (sl_spin_step(&spin) && guard++ < 500) { }
            view.have_result = true;
            view.last_win = sl_evaluate(stops, view.bet, view.lines,
                view.line_pays);
        }

        {
            int x, y;
            for (y = 0; y < Z_SCREEN_H; y++)
                for (x = 0; x < Z_SCREEN_W; x++)
                    z_fb_set_pixel(x, y, 0, NULL);
        }
        sl_board_draw(&L, &view);

        for (k = 0; k < SL_LINES; k++) {
            int side;
            for (side = 0; side < 2; side++) {
                int x, y, px, py, on = 0, off = 0;
                sl_mark_pos(&L, k, side != 0, &x, &y);
                /* The interior only, so the border does not count. */
                for (py = y + 1; py < y + SL_MARK_W - 1; py++)
                    for (px = x + 1; px < x + SL_MARK_W - 1; px++)
                        z_render_get(px, py) ? on++ : off++;
                /* A readable digit needs both ink and background --
                 * all-on or all-off is a marker with no number in it. */
                if (on < 4 || off < 4) {
                    printf("      %s marker %d: %d lit, %d clear\n",
                        side ? "right" : "left", k + 1, on, off);
                    bad++;
                }
            }
        }
    }

    check_eq(bad, 0, "every marker still shows a digit after a repaint");
}

static void test_sizes(void)
{
    sl_layout_t a, b;

    setup(2, 13, 316, 225);
    check(L.ok, "a 316x225 window fits a machine");

    sl_board_layout(&a, &view, 2, 13, 316, 225);
    sl_board_layout(&b, &view, 2, 13, 316, 225);
    check(memcmp(&a, &b, sizeof a) == 0, "the layout is a pure function");
    sl_board_layout(&b, &view, 12, 13, 316, 225);
    check_eq(b.reel_x, a.reel_x + 10, "and follows its origin");

    sl_board_layout(&a, &view, 0, 0, 200, 225);
    check(!a.ok, "a narrow window is refused");
    sl_board_layout(&a, &view, 0, 0, 316, 100);
    check(!a.ok, "and a short one");
    sl_board_layout(&a, &view, 0, 0, 320, 240);
    check(a.ok, "a game-mode page fits");
}

int main(void)
{
    printf("slots: layout tests\n");

    if (!z_render_open(&win, 320, 240)) {
        printf("slots: cannot map the framebuffer here -- skipping\n");
        return 77;
    }
    sl_board_init();

    test_sizes();
    test_marks_clear();
    test_digits_survive();
    test_nothing_escapes();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
