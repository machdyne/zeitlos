/*
 * Zeitlos -- host tests for sw/common/zshape.c.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * These exist because zshape.c has no MMIO in it. Every function there
 * ends in z_fb_set_pixel() or z_fb_hw_fill_rect(), both of which
 * sw/common/tests/zrender.h supplies in software -- so the geometry
 * can be drawn on a build machine and inspected, which zgfx.c's own
 * primitives cannot be.
 *
 * -- what is checked, and why it is the identities --
 *
 * The sine table is GENERATED, and the first version of zshape.c had
 * it typed out by hand instead: plausible-looking, monotonic, and
 * peaking at 3279 where a quarter turn must be exactly Z_TRIG_ONE.
 * Checking individual entries against a second list of numbers would
 * only move the problem. So what is asserted is what a wrong table
 * cannot satisfy -- a quarter turn is exactly one, sin^2 + cos^2 holds
 * all the way round, and the quadrant symmetry is exact.
 *
 * For the circles, the properties are the ones that a plausible-but-
 * wrong implementation breaks: symmetry about the centre, no gaps in
 * an outline, no notches on the diagonals of a fill, and a ring that
 * is actually solid between its two radii.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "zrender.h"
#include "../zshape.h"

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

static z_win_t win;
static z_clip_t full;

static void clear(void)
{
    int x, y;
    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            z_fb_set_pixel(x, y, 0, NULL);
}

static int get(int x, int y)
{
    return z_render_get(x, y);
}

/* -- trig ------------------------------------------------------------- */

static void test_trig(void)
{
    int32_t a;
    int bad = 0;

    check_eq(z_sin(0), 0, "sine starts at zero");
    check_eq(z_sin(Z_TRIG_TURN / 4), Z_TRIG_ONE,
        "and a quarter turn is exactly one");
    check_eq(z_sin(Z_TRIG_TURN / 2), 0, "a half turn is zero again");
    check_eq(z_sin(3 * Z_TRIG_TURN / 4), -Z_TRIG_ONE,
        "three quarters is exactly minus one");
    check_eq(z_cos(0), Z_TRIG_ONE, "cosine starts at one");
    check_eq(z_cos(Z_TRIG_TURN / 4), 0, "and a quarter turn is zero");

    /* A FULL REVOLUTION RETURNS EXACTLY WHERE IT STARTED, for every
     * angle and for angles far outside one turn. This is what makes a
     * long animation exact: an angle incremented for an hour is still
     * a valid angle, because the wrap is a mask on a power-of-two
     * turn rather than a modulo. */
    for (a = 0; a < Z_TRIG_TURN; a++) {
        if (z_sin(a) != z_sin(a + Z_TRIG_TURN)) bad++;
        if (z_sin(a) != z_sin(a + 97 * Z_TRIG_TURN)) bad++;
        if (z_sin(a) != z_sin(a - 50 * Z_TRIG_TURN)) bad++;
    }
    check_eq(bad, 0, "an angle wraps exactly, however far out it is");

    /* sin^2 + cos^2 = 1, to within the table's own rounding. At
     * Z_TRIG_ONE = 4096 each value is within half a unit, so the sum
     * of squares is within about 4096 of 4096^2 -- a tolerance of one
     * part in four thousand. A table with a wrong scale or a wrong
     * quadrant fails this by orders of magnitude. */
    bad = 0;
    for (a = 0; a < Z_TRIG_TURN; a++) {
        int64_t s = z_sin(a), c = z_cos(a);
        int64_t sum = s * s + c * c;
        int64_t one = (int64_t)Z_TRIG_ONE * Z_TRIG_ONE;
        int64_t err = sum > one ? sum - one : one - sum;
        if (err > one / 1000) bad++;
    }
    check_eq(bad, 0, "sin squared plus cos squared is one, all the way round");

    /* The quadrant reflections must be exact, not approximate --
     * they are how three quarters of the circle are reconstructed. */
    bad = 0;
    for (a = 0; a < Z_TRIG_TURN; a++) {
        if (z_sin(-a) != -z_sin(a)) bad++;
        if (z_sin(Z_TRIG_TURN / 2 - a) != z_sin(a)) bad++;
        if (z_cos(-a) != z_cos(a)) bad++;
    }
    check_eq(bad, 0, "the quadrant symmetries are exact");

    /* Monotonic through the first quarter, which catches a table
     * assembled in the wrong order. */
    bad = 0;
    for (a = 1; a <= Z_TRIG_TURN / 4; a++)
        if (z_sin(a) < z_sin(a - 1)) bad++;
    check_eq(bad, 0, "sine rises through the first quarter");
}

static void test_polar(void)
{
    int32_t a;
    int bad = 0;
    int x, y;

    z_polar(100, 100, 30, 0, &x, &y);
    check_eq(x, 130, "angle zero points along positive x");
    check_eq(y, 100, "with no vertical offset");

    /* Screen y grows downward, so a quarter turn is DOWN. A wheel that
     * turns the other way is the visible symptom of getting this
     * wrong. */
    z_polar(100, 100, 30, Z_TRIG_TURN / 4, &x, &y);
    check_eq(x, 100, "a quarter turn is straight down");
    check_eq(y, 130, "which is +y on a screen");

    /* Every point on the circle must be within a pixel of the radius.
     * Truncating instead of rounding puts the negative side
     * systematically one pixel in, which shows as a lopsided circle
     * even though no single point is far wrong. */
    for (a = 0; a < Z_TRIG_TURN; a++) {
        int dx, dy, d2;
        z_polar(0, 0, 40, a, &x, &y);
        dx = x; dy = y;
        d2 = dx * dx + dy * dy;
        if (d2 < 39 * 39 || d2 > 41 * 41) bad++;
    }
    check_eq(bad, 0, "every polar point lands on its radius");

    /* Symmetric about the centre: the point half a turn away is the
     * exact mirror. */
    bad = 0;
    for (a = 0; a < Z_TRIG_TURN / 2; a++) {
        int x2, y2;
        z_polar(0, 0, 40, a, &x, &y);
        z_polar(0, 0, 40, a + Z_TRIG_TURN / 2, &x2, &y2);
        if (x2 != -x || y2 != -y) bad++;
    }
    check_eq(bad, 0, "opposite angles are exact mirrors");
}

/* -- circles ------------------------------------------------------------ */

static int ink_in(int x0, int y0, int w, int h)
{
    int x, y, n = 0;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            if (get(x, y)) n++;
    return n;
}

static void test_circle_outline(void)
{
    const int cx = 100, cy = 100, r = 30;
    int x, y, bad = 0, drawn = 0;

    clear();
    z_fb_circle(cx, cy, r, 1, &full);

    /* Every lit pixel is on the circle, to within half a pixel either
     * side -- nothing stray, nothing at the wrong radius. */
    for (y = cy - r - 2; y <= cy + r + 2; y++)
        for (x = cx - r - 2; x <= cx + r + 2; x++) {
            if (!get(x, y)) continue;
            drawn++;
            {
                int dx = x - cx, dy = y - cy;
                int d2 = dx * dx + dy * dy;
                if (d2 < (r - 1) * (r - 1) || d2 > (r + 1) * (r + 1)) bad++;
            }
        }

    check_eq(bad, 0, "every pixel of an outline is on the circle");
    check(drawn > 4 * r, "and there are enough of them to be a circle");

    /* FOUR-WAY SYMMETRY, exactly. An outline that is a pixel out on one
     * side is the classic midpoint off-by-one and looks fine until it
     * is next to something square. */
    bad = 0;
    for (y = -r - 1; y <= r + 1; y++)
        for (x = -r - 1; x <= r + 1; x++) {
            int a = get(cx + x, cy + y);
            if (a != get(cx - x, cy + y)) bad++;
            if (a != get(cx + x, cy - y)) bad++;
            if (a != get(cx - y, cy + x)) bad++;   /* and the diagonal */
        }
    check_eq(bad, 0, "an outline is symmetric in all four reflections");

    /* NO GAPS. Every row the circle spans must have at least two lit
     * pixels -- one on each side. A midpoint loop with the wrong
     * termination leaves holes near the diagonals. */
    bad = 0;
    for (y = cy - r; y <= cy + r; y++) {
        int n = 0;
        for (x = cx - r - 1; x <= cx + r + 1; x++) if (get(x, y)) n++;
        if (n < 2) bad++;
    }
    check_eq(bad, 0, "every row of the circle is covered");

    /* And the same by column, which is the reflection that catches a
     * loop that stops one octant short. */
    bad = 0;
    for (x = cx - r; x <= cx + r; x++) {
        int n = 0;
        for (y = cy - r - 1; y <= cy + r + 1; y++) if (get(x, y)) n++;
        if (n < 2) bad++;
    }
    check_eq(bad, 0, "and every column");

    /* Degenerate radii must not draw nonsense. */
    clear();
    z_fb_circle(cx, cy, 0, 1, &full);
    check_eq(ink_in(cx - 2, cy - 2, 5, 5), 1, "radius zero is one pixel");

    clear();
    z_fb_circle(cx, cy, -5, 1, &full);
    check_eq(ink_in(cx - 20, cy - 20, 41, 41), 0,
        "a negative radius draws nothing");
}

static void test_circle_fill(void)
{
    const int cx = 120, cy = 110, r = 25;
    int x, y, bad = 0;

    clear();
    z_fb_fill_circle(cx, cy, r, 1, &full);

    /* Solid inside, empty outside, with the boundary allowed a pixel
     * of slack either way. */
    for (y = cy - r - 3; y <= cy + r + 3; y++)
        for (x = cx - r - 3; x <= cx + r + 3; x++) {
            int dx = x - cx, dy = y - cy;
            int d2 = dx * dx + dy * dy;
            int on = get(x, y);
            if (d2 <= (r - 1) * (r - 1) && !on) bad++;
            if (d2 >= (r + 1) * (r + 1) && on) bad++;
        }
    check_eq(bad, 0, "a filled circle is solid inside and empty outside");

    /* NO NOTCHES ON THE DIAGONALS. Reconstructing every row's extent
     * from a midpoint octant walk is where that bug comes from, which
     * is why half_width() derives each row independently. Checked by
     * requiring each row's run to be unbroken. */
    bad = 0;
    for (y = cy - r; y <= cy + r; y++) {
        int first = -1, last = -1, gaps = 0, run = 0;
        for (x = cx - r - 2; x <= cx + r + 2; x++) {
            if (get(x, y)) {
                if (first < 0) first = x;
                last = x;
                if (!run) { run = 1; if (first != x) gaps++; }
            } else {
                if (run) run = 0;
            }
        }
        if (first >= 0) {
            for (x = first; x <= last; x++) if (!get(x, y)) gaps++;
        }
        if (gaps) bad++;
    }
    check_eq(bad, 0, "every row of a filled circle is one unbroken run");

    /* Symmetric, like the outline. */
    bad = 0;
    for (y = -r - 1; y <= r + 1; y++)
        for (x = -r - 1; x <= r + 1; x++) {
            int a = get(cx + x, cy + y);
            if (a != get(cx - x, cy + y)) bad++;
            if (a != get(cx + x, cy - y)) bad++;
            if (a != get(cx - y, cy + x)) bad++;
        }
    check_eq(bad, 0, "a filled circle is symmetric in all four reflections");

    /* The area should be close to pi r^2. Catches a fill that is right
     * in shape and wrong in scale. */
    {
        int area = ink_in(cx - r - 2, cy - r - 2, 2 * r + 5, 2 * r + 5);
        int want = (314 * r * r) / 100;
        check(area > want - 4 * r && area < want + 4 * r,
            "a filled circle has about the right area");
    }
}

static void test_ring(void)
{
    const int cx = 130, cy = 120, ri = 18, ro = 28;
    int x, y, bad = 0;

    clear();
    z_fb_fill_ring(cx, cy, ri, ro, 1, &full);

    for (y = cy - ro - 3; y <= cy + ro + 3; y++)
        for (x = cx - ro - 3; x <= cx + ro + 3; x++) {
            int dx = x - cx, dy = y - cy;
            int d2 = dx * dx + dy * dy;
            int on = get(x, y);
            /* Comfortably inside the band must be lit. */
            if (d2 >= (ri + 2) * (ri + 2) && d2 <= (ro - 2) * (ro - 2) && !on)
                bad++;
            /* Comfortably outside it must not be. */
            if (d2 <= (ri - 2) * (ri - 2) && on) bad++;
            if (d2 >= (ro + 2) * (ro + 2) && on) bad++;
        }

    check_eq(bad, 0, "a ring is solid between its radii and hollow within");

    /* THE HOLE IS ACTUALLY A HOLE. Two outlines would leave the pixels
     * between them untouched, which is a hole rather than a ring
     * whenever the radii differ by more than one -- the reason this is
     * spans and not two z_fb_circle() calls. */
    /* A box INSCRIBED in the inner circle, not one that merely fits
     * inside its bounding square -- the corners of that square are
     * outside the circle and land in the band, which is a test failing
     * on its own geometry. Half-width ri * 0.6 is comfortably inside
     * ri / sqrt(2). */
    check_eq(ink_in(cx - (ri * 6) / 10, cy - (ri * 6) / 10,
        (ri * 12) / 10, (ri * 12) / 10), 0,
        "the middle of a ring is empty");

    /* A ring one pixel wide still draws something on every row. */
    clear();
    z_fb_fill_ring(cx, cy, 20, 20, 1, &full);
    bad = 0;
    for (y = cy - 20; y <= cy + 20; y++) {
        int n = 0, want = (y == cy - 20 || y == cy + 20) ? 1 : 2;
        for (x = cx - 22; x <= cx + 22; x++) if (get(x, y)) n++;
        /* Two pixels a row -- one per side -- EXCEPT at the apex and
         * the nadir, where a circle genuinely has one. Demanding two
         * there is a test asserting something untrue about circles. */
        if (n < want) bad++;
    }
    check_eq(bad, 0, "a one-pixel ring is still continuous");

    /* Inner larger than outer draws nothing rather than something
     * strange. */
    clear();
    z_fb_fill_ring(cx, cy, 30, 10, 1, &full);
    check_eq(ink_in(cx - 40, cy - 40, 81, 81), 0,
        "an inside-out ring draws nothing");
}

static void test_clipping(void)
{
    z_clip_t c;
    int x, y, outside = 0;

    /* NOTHING MAY LAND OUTSIDE THE CLIP. z_fb_hw_fill_rect() clamps to
     * the screen and to the window manager's visible region but knows
     * nothing about a caller's own rectangle, so every span has to be
     * clipped here -- and a filled circle is all spans. */
    c.x0 = 80; c.y0 = 80; c.x1 = 120; c.y1 = 120;

    clear();
    z_fb_fill_circle(100, 100, 40, 1, &c);
    z_fb_circle(100, 100, 45, 1, &c);
    z_fb_fill_ring(100, 100, 30, 50, 1, &c);
    z_fb_spoke(100, 100, 0, 60, 137, 1, &c);

    for (y = 0; y < Z_SCREEN_H; y++)
        for (x = 0; x < Z_SCREEN_W; x++)
            if (get(x, y) && (x < c.x0 || x > c.x1 || y < c.y0 || y > c.y1))
                outside++;

    check_eq(outside, 0, "nothing is drawn outside the clip rectangle");
    check(ink_in(c.x0, c.y0, 41, 41) > 100, "and plenty is drawn inside it");

    /* A shape entirely outside its clip draws nothing at all. */
    clear();
    z_fb_fill_circle(10, 10, 5, 1, &c);
    check_eq(ink_in(0, 0, Z_SCREEN_W, Z_SCREEN_H), 0,
        "a shape outside the clip draws nothing");

    /* And one straddling the screen edge must not wrap to the other
     * side, which is what an unclamped span does. */
    clear();
    z_fb_fill_circle(3, 100, 20, 1, NULL);
    check_eq(ink_in(Z_SCREEN_W - 20, 80, 20, 41), 0,
        "a circle at the left edge does not wrap to the right");
}

static void test_spoke(void)
{
    int x0, y0, x1, y1;

    clear();
    z_fb_spoke(100, 100, 10, 30, 0, 1, &full);

    /* A spoke runs between its two radii and nowhere else. */
    z_polar(100, 100, 10, 0, &x0, &y0);
    z_polar(100, 100, 30, 0, &x1, &y1);
    check(get(x0, y0), "a spoke starts at its inner radius");
    check(get(x1, y1), "and ends at its outer one");
    check_eq(ink_in(100 - 5, 100 - 5, 11, 11), 0,
        "and leaves the hub alone");
}

int main(void)
{
    printf("zshape: circle and trig tests\n");

    if (!z_render_open(&win, 320, 240)) {
        printf("zshape: cannot map the framebuffer here "
            "(Linux/x86-64 only) -- skipping\n");
        return 77;
    }

    full.x0 = 0; full.y0 = 0;
    full.x1 = Z_SCREEN_W - 1; full.y1 = Z_SCREEN_H - 1;

    test_trig();
    test_polar();
    test_circle_outline();
    test_circle_fill();
    test_ring();
    test_spoke();
    test_clipping();

    printf("%d checks, %d failures\n", checks, failures);

    return failures ? 1 : 0;
}
