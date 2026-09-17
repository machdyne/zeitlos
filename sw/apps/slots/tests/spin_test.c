/*
 * Zeitlos slots -- host tests for the spin.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The result is chosen before a frame is drawn and the animation is
 * made to land on it, so the thing that must never break is that it
 * DOES land on it. A reel that finishes one cell out pays the wrong
 * line, and the exhaustive house edge in sl_reels.c would still be
 * exactly right while the machine paid something else entirely.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../sl_game.h"

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

static sl_spin_t s;

static void run(const int *stops)
{
    int guard = 0;
    sl_spin_begin(&s, stops);
    while (sl_spin_step(&s) && guard++ < 2000) { }
}

static void test_lands(void)
{
    int a, b, c, bad = 0;

    /* EVERY combination of stops, from every starting position the
     * previous spin could have left. */
    for (a = 0; a < SL_STOPS; a++) {
        int stops[SL_REELS];
        stops[0] = a;
        stops[1] = (a * 7 + 3) % SL_STOPS;
        stops[2] = (a * 13 + 11) % SL_STOPS;
        run(stops);
        for (c = 0; c < SL_REELS; c++) {
            if (s.pos[c] != stops[c] * SL_CELL) bad++;
            if (sl_reel_subpixel(&s, c) != 0) bad++;
            if (sl_reel_symbol(&s, c, 0) != sl_strip[c][stops[c]]) bad++;
        }
    }
    check_eq(bad, 0, "every reel lands exactly on its stop");

    /* And from a running start -- a real spin begins wherever the last
     * one ended, which is the case an implementation that assumes zero
     * gets wrong. */
    bad = 0;
    for (a = 0; a < SL_STOPS; a += 3)
        for (b = 0; b < SL_STOPS; b += 5) {
            int stops[SL_REELS];
            int r;
            stops[0] = stops[1] = stops[2] = a;
            run(stops);
            stops[0] = stops[1] = stops[2] = b;
            run(stops);
            for (r = 0; r < SL_REELS; r++)
                if (s.pos[r] != b * SL_CELL) bad++;
        }
    check_eq(bad, 0, "and lands from wherever the last spin left it");

    /* A reel with nowhere to go still has to turn: landing on the stop
     * it is already on must not be instant. */
    {
        int stops[SL_REELS] = { 5, 5, 5 };
        run(stops);
        sl_spin_begin(&s, stops);
        check(s.travel[0] >= SL_SPIN_TURNS * SL_STRIP_PX,
            "landing where it already is still turns a few times");
    }
}

static void test_order(void)
{
    int stops[SL_REELS] = { 3, 17, 29 };
    int stopped[SL_REELS] = { -1, -1, -1 };
    int f = 0, r;

    sl_spin_begin(&s, stops);
    for (;;) {
        /* Recorded AFTER the step and before the loop exits: the frame
         * on which the LAST reel stops is the one where sl_spin_step()
         * returns false, so a `while (step())` loop never gets to see
         * it and reel 3 looks as though it never stopped at all. */
        bool more = sl_spin_step(&s);
        f++;
        for (r = 0; r < SL_REELS; r++)
            if (stopped[r] < 0 && sl_reel_stopped(&s, r)) stopped[r] = f;
        if (!more || f >= 2000) break;
    }

    /* LEFT TO RIGHT, strictly. Two sevens and a reel still turning is
     * the whole experience; three reels stopping together is a dice
     * roll. */
    check(stopped[0] > 0, "reel 1 stops");
    check(stopped[1] > stopped[0], "reel 2 stops after it");
    check(stopped[2] > stopped[1], "and reel 3 after that");
    check(stopped[2] - stopped[0] >= 2 * SL_STAGGER - 2,
        "with a real gap between first and last");
}

static void test_motion(void)
{
    int stops[SL_REELS] = { 11, 4, 22 };
    int f, bad = 0;
    int first_dy = 0, late_dy = 0;

    sl_spin_begin(&s, stops);

    for (f = 0; f < 2000; f++) {
        int r;
        bool more = sl_spin_step(&s);

        for (r = 0; r < SL_REELS; r++) {
            int dy = sl_reel_dy(&s, r);

            /* FORWARD ONLY. A reel that crept backwards to reach a near
             * target would be unmistakable, and the wraparound
             * arithmetic is exactly where that creeps in. */
            if (dy < 0) bad++;
            /* And never more than a whole strip in one frame, which
             * would mean the offset had wrapped unnoticed. */
            if (dy > SL_STRIP_PX) bad++;

            if (r == 0 && f == 1) first_dy = dy;
            if (r == 0 && f == SL_BASE_FRAMES - 3) late_dy = dy;

            if (s.pos[r] < 0 || s.pos[r] >= SL_STRIP_PX) bad++;
        }

        if (!more) break;
    }

    check_eq(bad, 0, "reels only ever turn forward, and stay in range");
    check(first_dy > late_dy * 3, "a reel slows markedly before it stops");
    check(late_dy > 0, "but is still moving on its last frame");

    /* The whole spin is over in a bounded number of frames -- an app
     * pumping a message queue every frame needs that to be true. */
    check(f <= SL_BASE_FRAMES + (SL_REELS - 1) * SL_STAGGER + 2,
        "the spin ends when the last reel is due");
}

static void test_window(void)
{
    int stops[SL_REELS] = { 0, 1, 31 };
    int r, row, bad = 0;

    run(stops);

    /* A stopped reel shows its stop and the two after it, wrapping. */
    for (r = 0; r < SL_REELS; r++)
        for (row = 0; row < SL_ROWS; row++) {
            uint8_t want = sl_strip[r][(stops[r] + row) % SL_STOPS];
            if (sl_reel_symbol(&s, r, row) != want) bad++;
        }
    check_eq(bad, 0, "a stopped reel shows the window its stop implies");

    /* Which must agree with sl_window(), or the thing drawn and the
     * thing paid are different machines. */
    bad = 0;
    for (r = 0; r < SL_REELS; r++) {
        uint8_t w[SL_ROWS];
        sl_window(r, stops[r], w);
        for (row = 0; row < SL_ROWS; row++)
            if (w[row] != sl_reel_symbol(&s, r, row)) bad++;
    }
    check_eq(bad, 0, "and agrees with what the paytable is shown");

    /* Mid-spin the sub-pixel offset stays inside one cell. */
    sl_spin_begin(&s, stops);
    bad = 0;
    for (r = 0; r < 40; r++) {
        int k;
        sl_spin_step(&s);
        for (k = 0; k < SL_REELS; k++) {
            int sp = sl_reel_subpixel(&s, k);
            if (sp < 0 || sp >= SL_CELL) bad++;
        }
    }
    check_eq(bad, 0, "the sub-cell offset stays inside a cell");
}

int main(void)
{
    printf("slots: spin tests\n");

    sl_spin_init(&s);

    test_lands();
    test_order();
    test_motion();
    test_window();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
