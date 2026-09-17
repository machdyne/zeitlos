/*
 * Zeitlos roulette -- host tests for the wheel.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- the good one --
 *
 * On a European wheel the pockets alternate red and black all the way
 * round after the zero. That is a property of the real wheel, and
 * checking it here validates TWO written-out lists against each other:
 * the pocket sequence in rl_wheel.c and the red set in rl_table.c.
 * Get a single number's colour wrong in either and the alternation
 * breaks somewhere.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* The software pixel primitives, so zshape.c links and the wheel can
 * actually be DRAWN here rather than only computed. See
 * sw/common/tests/zrender.h for what the harness is and why the
 * drawing half of a test matters as much as the arithmetic. */
#include "roulette_shim.h"

#include "../rl_wheel.h"

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

static void test_order(void)
{
    int order[RL_MAX_POCKETS];
    int seen[RL_MAX_POCKETS];
    int n, i, bad;

    /* A PERMUTATION: every pocket exactly once, nothing else. */
    n = rl_wheel_order(RL_EURO, order);
    check_eq(n, 37, "a european wheel has 37 pockets");
    for (i = 0; i < RL_MAX_POCKETS; i++) seen[i] = 0;
    bad = 0;
    for (i = 0; i < n; i++) {
        if (order[i] < 0 || order[i] > 36) { bad++; continue; }
        seen[order[i]]++;
    }
    check_eq(bad, 0, "and no pocket outside 0 to 36");
    for (i = 0; i <= 36; i++) if (seen[i] != 1) bad++;
    check_eq(bad, 0, "the european rim is a permutation of 0 to 36");
    check_eq(order[0], 0, "and it starts at zero");

    n = rl_wheel_order(RL_AMERICAN, order);
    check_eq(n, 38, "an american wheel has 38");
    for (i = 0; i < RL_MAX_POCKETS; i++) seen[i] = 0;
    bad = 0;
    for (i = 0; i < n; i++) seen[order[i]]++;
    for (i = 0; i <= 36; i++) if (seen[i] != 1) bad++;
    if (seen[RL_DOUBLE_ZERO] != 1) bad++;
    check_eq(bad, 0, "the american rim is a permutation including 00");

    /* THE ALTERNATION. After the zero, red and black strictly alternate
     * round a european wheel. Two independently written lists agreeing
     * 36 times is a far stronger statement than either being read
     * over. */
    n = rl_wheel_order(RL_EURO, order);
    bad = 0;
    for (i = 1; i < n; i++) {
        bool want_red = (i % 2) == 1;
        if (rl_is_red(order[i]) != want_red) {
            printf("      pocket %d at rim position %d is %s\n",
                order[i], i, rl_is_red(order[i]) ? "red" : "black");
            bad++;
        }
    }
    check_eq(bad, 0, "a european wheel alternates red and black throughout");

    /* Opposing pairs on the american wheel: 1 faces 2, 3 faces 4. */
    n = rl_wheel_order(RL_AMERICAN, order);
    bad = 0;
    for (i = 0; i < n; i++) {
        int a = order[i], b = order[(i + n / 2) % n];
        if (a == RL_ZERO || a == RL_DOUBLE_ZERO) continue;
        /* Consecutive numbers, one odd one even, facing each other. */
        if (!((a % 2 == 1 && b == a + 1) || (a % 2 == 0 && b == a - 1))) bad++;
    }
    check_eq(bad, 0, "an american wheel puts consecutive numbers opposite");
}

static void test_angles(void)
{
    int i, n, bad = 0;
    int order[RL_MAX_POCKETS];

    /* Every pocket must map to an angle that maps back to itself. This
     * is the round trip a spin relies on: the ball is aimed at an
     * angle, and whatever is read off at that angle had better be the
     * pocket that was chosen. */
    for (i = 0; i <= 36; i++) {
        int32_t a = rl_wheel_angle(RL_EURO, i);
        if (rl_wheel_at(RL_EURO, a) != i) {
            printf("      pocket %d -> angle %d -> pocket %d\n",
                i, (int)a, rl_wheel_at(RL_EURO, a));
            bad++;
        }
    }
    check_eq(bad, 0, "every european pocket round-trips through its angle");

    bad = 0;
    n = rl_wheel_order(RL_AMERICAN, order);
    for (i = 0; i < n; i++) {
        int32_t a = rl_wheel_angle(RL_AMERICAN, order[i]);
        if (rl_wheel_at(RL_AMERICAN, a) != order[i]) bad++;
    }
    check_eq(bad, 0, "and every american one");

    /* The angles are spread evenly round the rim -- no two pockets in
     * the same place, none left with a gap. */
    bad = 0;
    for (i = 0; i <= 36; i++) {
        int j;
        int32_t ai = rl_wheel_angle(RL_EURO, i);
        for (j = i + 1; j <= 36; j++)
            if (rl_wheel_angle(RL_EURO, j) == ai) bad++;
    }
    check_eq(bad, 0, "no two pockets share an angle");

    /* An angle anywhere at all lands in some valid pocket, including
     * angles outside one turn. */
    bad = 0;
    for (i = -5000; i < 5000; i += 7) {
        int p = rl_wheel_at(RL_EURO, i);
        if (p < 0 || p > 36) bad++;
    }
    check_eq(bad, 0, "any angle at all reads as a real pocket");
}

static void test_spin(void)
{
    rl_wheel_t w;
    int target, bad = 0, frames;

    /* THE SPIN MUST LAND WHERE IT WAS AIMED, every time, for every
     * pocket and a range of durations. The result is decided by the
     * generator first and the animation made to reach it -- if that
     * ever stops being exact, the wheel starts paying the wrong number
     * and nothing else would notice. */
    for (target = 0; target <= 36; target++) {
        for (frames = 2; frames < 120; frames += 17) {
            int guard = 0;
            rl_wheel_init(&w, RL_EURO, 100, 100, 40);
            rl_wheel_spin(&w, target, frames);
            while (rl_wheel_step(&w) && guard++ < 1000) { }
            if (w.spinning) { bad++; continue; }
            if (rl_wheel_landed(&w) != target) bad++;
        }
    }
    check_eq(bad, 0, "a spin lands on the pocket it was aimed at");

    /* It must actually go somewhere -- several turns, not a nudge. */
    rl_wheel_init(&w, RL_EURO, 100, 100, 40);
    rl_wheel_spin(&w, 17, 60);
    check(w.travel > 5 * Z_TRIG_TURN, "a spin covers several revolutions");

    /* THE HEAD TURNS, AND THE POCKETS TURN WITH IT.
     *
     * This is the bug as reported: the wheel did not spin, only the
     * middle did. The pockets were drawn at fixed angles and the three
     * hub spokes were the only thing moving, so it read as a static
     * ring with a dot going round it.
     *
     * Asserted on the head's angle rather than on pixels, and separately
     * on the landing, because the two are coupled: if the head moves and
     * the aim does not follow it, the ball lands in the wrong pocket --
     * which is worse than a wheel that does not turn. */
    {
        int32_t a0;
        rl_wheel_init(&w, RL_EURO, 100, 100, 40);
        a0 = w.rim_a;
        rl_wheel_spin(&w, 22, 60);
        rl_wheel_step(&w);
        check(w.rim_a != a0, "the head moves on the first frame");
        while (rl_wheel_step(&w)) { }
        check(w.rim_a != a0, "and has turned by the end");
        check_eq(w.rim_a, w.rim_end, "landing exactly where the spin said");
        check_eq(rl_wheel_landed(&w), 22,
            "with the ball still in the pocket it was aimed at");

        /* And the head goes the OTHER WAY to the ball, which is what a
         * real wheel does and most of why one looks alive. */
        check(w.rim_rate < 0, "the head turns against the ball");
    }

    /* AND IT MUST DECELERATE. The first frames move much further than
     * the last ones; a linear sweep looks like a scanning cursor
     * rather than a ball losing speed. */
    {
        int32_t first, last;
        rl_wheel_init(&w, RL_EURO, 100, 100, 40);
        rl_wheel_spin(&w, 17, 90);
        rl_wheel_step(&w);
        first = (w.ball_a - w.prev_ball_a) & (Z_TRIG_TURN - 1);
        while (w.frame < w.frames - 2) rl_wheel_step(&w);
        rl_wheel_step(&w);
        last = (w.ball_a - w.prev_ball_a) & (Z_TRIG_TURN - 1);
        check(first > last * 4, "the ball slows down markedly as it lands");
    }

    /* AND THE BALL DROPS INTO THE POCKET.
     *
     * Decelerating to rest always ends in sub-pixel motion -- a cubic
     * ease over ninety frames spent its last thirty covering under half
     * a pixel, which reads as a freeze rather than as a landing. The
     * tail is filled by the ball leaving the outer track and falling
     * inward, so it is visibly moving right up to the moment it lands.
     *
     * Checked through the ball's rectangle, which is the only view of
     * its radius from outside. */
    {
        z_clip_t early, late;
        int dearly, dlate;
        rl_wheel_init(&w, RL_EURO, 160, 120, 44);
        rl_wheel_spin(&w, 17, 90);
        while (w.frame < 20) rl_wheel_step(&w);
        rl_wheel_ball_rect(&w, w.ball_a, w.ball_r, &early);
        dearly = (early.x0 + early.x1) / 2 - w.cx;
        dearly = dearly < 0 ? -dearly : dearly;
        while (rl_wheel_step(&w)) { }
        rl_wheel_ball_rect(&w, w.ball_a, w.ball_r, &late);
        (void)dlate;
        /* Once landed, the ball sits in the rim band, not out on the
         * track it was riding. */
        {
            int x = (late.x0 + late.x1) / 2 - w.cx;
            int y = (late.y0 + late.y1) / 2 - w.cy;
            int d2 = x * x + y * y;
            int ri = (w.r * 62) / 100, ro = (w.r * 86) / 100;
            check(d2 >= ri * ri && d2 <= ro * ro,
                "the ball finishes in the rim, not on the track");
        }
    }

    /* The hub turns the other way while the ball orbits. Two things at
     * different speeds is most of what reads as motion. */
    {
        rl_wheel_init(&w, RL_EURO, 100, 100, 40);
        rl_wheel_spin(&w, 5, 40);
        rl_wheel_step(&w);
        rl_wheel_step(&w);
        check(w.hub_a != w.prev_hub_a, "the hub turns too");
    }

    /* Stepping a stopped wheel is harmless. */
    rl_wheel_init(&w, RL_EURO, 100, 100, 40);
    check(!rl_wheel_step(&w), "a wheel that is not spinning reports so");
    check_eq(w.ball_a, rl_wheel_angle(RL_EURO, 0), "and does not move");

    /* A degenerate frame count must not divide by zero or spin
     * forever. */
    rl_wheel_init(&w, RL_EURO, 100, 100, 40);
    rl_wheel_spin(&w, 9, 0);
    {
        int guard = 0;
        while (rl_wheel_step(&w) && guard++ < 100) { }
        check(!w.spinning, "a zero-frame spin still terminates");
        check_eq(rl_wheel_landed(&w), 9, "and still lands right");
    }
}

static void test_geometry(void)
{
    rl_wheel_t w;
    z_clip_t r;

    rl_wheel_init(&w, RL_EURO, 100, 100, 10);
    check(w.r >= RL_WHEEL_MIN_R, "a tiny wheel is clamped to a usable size");

    rl_wheel_init(&w, RL_EURO, 160, 120, 40);
    rl_wheel_ball_rect(&w, 0, w.ball_r, &r);
    check(r.x1 > r.x0 && r.y1 > r.y0, "the ball rect is a real rectangle");
    check(r.x1 - r.x0 < 16 && r.y1 - r.y0 < 16,
        "and small -- it is what makes a frame cheap");

    /* The ball rect must actually contain the ball, wherever it is. */
    {
        int32_t a;
        int bad = 0;
        for (a = 0; a < Z_TRIG_TURN; a += 13) {
            int x, y;
            rl_wheel_ball_rect(&w, a, w.ball_r, &r);
            z_polar(w.cx, w.cy, w.ball_r, a, &x, &y);
            if (x < r.x0 || x > r.x1 || y < r.y0 || y > r.y1) bad++;
        }
        check_eq(bad, 0, "the ball rect contains the ball at every angle");
    }

    rl_wheel_hub_rect(&w, &r);
    check(r.x0 < w.cx && r.x1 > w.cx, "the hub rect straddles the centre");
}

/* -- what actually lands on the framebuffer ---------------------------- */

static z_win_t win;

static void clear_all(void)
{
    int x, y;
    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            z_fb_set_pixel(x, y, 0, NULL);
}

static int ink_in(int x0, int y0, int w, int h)
{
    int x, y, n = 0;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            if (z_render_get(x, y)) n++;
    return n;
}

static void test_drawing(void)
{
    rl_wheel_t w;
    z_clip_t clip, ball;
    int x, y, outside = 0;

    clip.x0 = 0; clip.y0 = 0;
    clip.x1 = Z_SCREEN_W - 1; clip.y1 = Z_SCREEN_H - 1;

    rl_wheel_init(&w, RL_EURO, 160, 120, 44);

    clear_all();
    rl_wheel_draw(&w, &clip);

    check(ink_in(0, 0, Z_SCREEN_W, Z_SCREEN_H) > 500, "the wheel is drawn");

    /* Nothing outside the wheel's own bounding box. A rim drawn with
     * spokes is all trigonometry, and a sign error puts pixels on the
     * far side of the screen while still looking like a wheel where it
     * is supposed to be. */
    outside = 0;
    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++) {
            int dx = x - w.cx, dy = y - w.cy;
            if (z_render_get(x, y) && dx * dx + dy * dy > (w.r + 2) * (w.r + 2))
                outside++;
        }
    check_eq(outside, 0, "and entirely inside its own radius");

    /* Clipped to a corner of itself, nothing may escape. */
    clear_all();
    clip.x0 = w.cx; clip.y0 = w.cy;
    clip.x1 = w.cx + 20; clip.y1 = w.cy + 20;
    rl_wheel_draw(&w, &clip);
    outside = 0;
    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            if (z_render_get(x, y) &&
                (x < clip.x0 || x > clip.x1 || y < clip.y0 || y > clip.y1))
                outside++;
    check_eq(outside, 0, "a clipped wheel stays inside its clip");

    /* THE REPAIR THAT MAKES THE ANIMATION CHEAP.
     *
     * Redrawing the wheel clipped to the ball's OLD rectangle must
     * restore that patch exactly as it was before the ball was ever
     * there -- same drawing code, so it has to. If it does not, every
     * frame leaves a smear and the only fix is a full redraw, which is
     * what this design exists to avoid. */
    {
        static uint8_t before[32 * 32], after[32 * 32];
        int n, bw, bh;

        clip.x0 = 0; clip.y0 = 0;
        clip.x1 = Z_SCREEN_W - 1; clip.y1 = Z_SCREEN_H - 1;

        /* Wheel with the ball somewhere else entirely, AT THE TRACK
         * RADIUS -- where it actually is during a spin.
         *
         * rl_wheel_init() leaves ball_r at the landed radius, inside
         * the rim band, and the ring fill repaints that band anyway. So
         * the first version of this test proved the repair worked in
         * the one place the bug could not show, and a spin left every
         * ball position on screen. */
        rl_wheel_init(&w, RL_EURO, 160, 120, 44);
        w.ball_r = (w.r * 94) / 100;
        w.ball_a = Z_TRIG_TURN / 2;
        clear_all();
        rl_wheel_draw(&w, &clip);

        rl_wheel_ball_rect(&w, 0, w.ball_r, &ball);
        bw = ball.x1 - ball.x0 + 1;
        bh = ball.y1 - ball.y0 + 1;
        n = 0;
        for (y = ball.y0; y <= ball.y1; y++)
            for (x = ball.x0; x <= ball.x1; x++)
                before[n++] = (uint8_t)z_render_get(x, y);

        /* Now put the ball there, then repair by redrawing clipped to
         * that rectangle with the ball moved away again. */
        w.ball_a = 0;
        rl_wheel_draw(&w, &clip);
        w.ball_a = Z_TRIG_TURN / 2;
        rl_wheel_draw(&w, &ball);

        n = 0;
        for (y = ball.y0; y <= ball.y1; y++)
            for (x = ball.x0; x <= ball.x1; x++)
                after[n++] = (uint8_t)z_render_get(x, y);

        {
            int diff = 0, i;
            for (i = 0; i < bw * bh; i++) if (before[i] != after[i]) diff++;
            check_eq(diff, 0, "repairing the ball's old patch restores it exactly");
        }
    }

    /* A WHOLE SPIN LEAVES EXACTLY ONE BALL BEHIND.
     *
     * This is the shape the bug actually took on hardware: every
     * position the ball passed through stayed on screen, so the wheel
     * ended a spin wearing a necklace. The per-frame repair test above
     * is the precise version; this is the one that looks like the
     * symptom, and it runs the real loop from roulette.c -- capture the
     * footprint, step, repair, draw.
     *
     * The count is of lit pixels in the ball's track annulus, with the
     * outer wall outline excluded by staying inside it. One ball is a
     * few dozen; a full trail is hundreds. */
    {
        rl_wheel_t sw;
        int px, py, ink = 0, guard = 0;
        int rlo, rhi;

        clear_all();
        rl_wheel_init(&sw, RL_EURO, 160, 120, 44);
        rl_wheel_spin(&sw, 17, 60);
        rl_wheel_draw(&sw, &clip);

        for (;;) {
            z_clip_t area;
            bool more;
            int pad = sw.r + (sw.r / 9) + 4;

            more = rl_wheel_step(&sw);

            /* The real frame from roulette.c: repaint the rim in place,
             * clear the ball's track, draw the streak. */
            area.x0 = sw.cx - pad;
            area.y0 = sw.cy - pad;
            area.x1 = sw.cx + pad;
            area.y1 = sw.cy + pad;

            rl_wheel_rim(&sw, &area);
            rl_wheel_track_clear(&sw, &area);
            rl_wheel_ball(&sw, &area);

            if (!more || guard++ > 200) break;
        }

        /* Strictly between the rim's outer edge and the wall, so
         * neither the pockets nor the wall outline is counted. */
        rlo = (sw.r * 88) / 100;
        rhi = sw.r - 1;

        for (py = sw.cy - sw.r; py <= sw.cy + sw.r; py++)
            for (px = sw.cx - sw.r; px <= sw.cx + sw.r; px++) {
                int dx = px - sw.cx, dy = py - sw.cy;
                int d2 = dx * dx + dy * dy;
                if (d2 < rlo * rlo || d2 > rhi * rhi) continue;
                if (z_render_get(px, py)) ink++;
            }

        /* One ball, not a trail. The streak is drawn afresh each frame
         * and the track is cleared before it, so what survives to the
         * last frame is a single settled ball -- by then the streak has
         * shortened to a dot, because the ball has stopped. */
        check(ink < 120, "a finished spin leaves one ball, not a trail");
        if (ink >= 80)
            printf("      %d lit pixels on the ball track\n", ink);
    }

}

int main(void)
{
    printf("roulette: wheel tests\n");

    test_order();
    test_angles();
    test_spin();
    test_geometry();

    if (!z_render_open(&win, 320, 240)) {
        printf("roulette: cannot map the framebuffer here -- skipping "
            "the drawing checks\n");
    } else {
        test_drawing();
    }

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
