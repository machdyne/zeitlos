/*
 * Zeitlos -- circles, arcs, and the trig they need.
 * See zshape.h for why this is its own object and not part of zgfx.c.
 */

#include "zshape.h"

/* -- trig --------------------------------------------------------------
 *
 * A quarter turn of sine, scaled by Z_TRIG_ONE. The other three
 * quarters are reflections, so only 257 entries are stored -- the
 * extra one being sin at exactly a quarter turn, which lets the
 * reflection below index [Q - i] without a bounds case.
 *
 * 514 bytes of read-only data. A generated table rather than a runtime
 * CORDIC: the table is one load where CORDIC is a dozen iterations,
 * and at 1024 steps a turn it is exact enough that a point on a
 * 36-pixel radius never moves by less than it should.
 *
 * GENERATED, not typed. The first version of this file had the numbers
 * written out by hand and they were wrong -- plausible-looking,
 * monotonic, and peaking at 3279 where a quarter turn must be exactly
 * Z_TRIG_ONE. Regenerate with:
 *
 *     python3 -c 'import math
 *     print(",".join(str(round(math.sin(i*2*math.pi/1024)*4096))
 *                    for i in range(257)))'
 *
 * tests/shape_test.c checks the identities rather than the numbers --
 * a quarter turn is exactly 1, sin^2 + cos^2 holds all the way round,
 * and the symmetry that reconstructs the other three quadrants is
 * exact. A wrong table cannot satisfy those.
 */
#define Q (Z_TRIG_TURN / 4)     /* 256 */

static const int16_t sin_q[Q + 1] = {
       0,   25,   50,   75,  101,  126,  151,  176,
     201,  226,  251,  276,  301,  326,  351,  376,
     401,  426,  451,  476,  501,  526,  551,  576,
     601,  626,  651,  675,  700,  725,  750,  774,
     799,  824,  848,  873,  897,  922,  946,  971,
     995, 1020, 1044, 1068, 1092, 1117, 1141, 1165,
    1189, 1213, 1237, 1261, 1285, 1309, 1332, 1356,
    1380, 1404, 1427, 1451, 1474, 1498, 1521, 1544,
    1567, 1591, 1614, 1637, 1660, 1683, 1706, 1729,
    1751, 1774, 1797, 1819, 1842, 1864, 1886, 1909,
    1931, 1953, 1975, 1997, 2019, 2041, 2062, 2084,
    2106, 2127, 2149, 2170, 2191, 2213, 2234, 2255,
    2276, 2296, 2317, 2338, 2359, 2379, 2399, 2420,
    2440, 2460, 2480, 2500, 2520, 2540, 2559, 2579,
    2598, 2618, 2637, 2656, 2675, 2694, 2713, 2732,
    2751, 2769, 2788, 2806, 2824, 2843, 2861, 2878,
    2896, 2914, 2932, 2949, 2967, 2984, 3001, 3018,
    3035, 3052, 3068, 3085, 3102, 3118, 3134, 3150,
    3166, 3182, 3198, 3214, 3229, 3244, 3260, 3275,
    3290, 3305, 3320, 3334, 3349, 3363, 3378, 3392,
    3406, 3420, 3433, 3447, 3461, 3474, 3487, 3500,
    3513, 3526, 3539, 3551, 3564, 3576, 3588, 3600,
    3612, 3624, 3636, 3647, 3659, 3670, 3681, 3692,
    3703, 3713, 3724, 3734, 3745, 3755, 3765, 3775,
    3784, 3794, 3803, 3812, 3822, 3831, 3839, 3848,
    3857, 3865, 3873, 3881, 3889, 3897, 3905, 3912,
    3920, 3927, 3934, 3941, 3948, 3954, 3961, 3967,
    3973, 3979, 3985, 3991, 3996, 4002, 4007, 4012,
    4017, 4022, 4027, 4031, 4036, 4040, 4044, 4048,
    4052, 4055, 4059, 4062, 4065, 4068, 4071, 4074,
    4076, 4079, 4081, 4083, 4085, 4087, 4088, 4090,
    4091, 4092, 4093, 4094, 4095, 4095, 4096, 4096,
    4096
};

int32_t z_sin(int32_t a)
{
    int q, i;

    /* Wrapped with a mask, which is the whole reason Z_TRIG_TURN is a
     * power of two: an angle that has been incremented for an hour is
     * still exact, and there is no modulo on a core that may have no
     * divider. Negative angles fold correctly because the mask is on
     * the two's-complement value. */
    a &= (Z_TRIG_TURN - 1);

    q = (int)(a / Q);
    i = (int)(a - (int32_t)q * Q);

    switch (q) {
    case 0: return sin_q[i];
    case 1: return sin_q[Q - i];
    case 2: return -sin_q[i];
    default: return -sin_q[Q - i];
    }
}

int32_t z_cos(int32_t a)
{
    return z_sin(a + Q);
}

void z_polar(int cx, int cy, int radius, int32_t a, int *x, int *y)
{
    int32_t c = z_cos(a);
    int32_t s = z_sin(a);
    int32_t half = Z_TRIG_ONE / 2;

    /* Rounded away from zero rather than truncated toward it. Truncation
     * biases every point one pixel toward the centre on the negative
     * side only, which makes a circle of plotted points visibly
     * lopsided even though each point is within one pixel. */
    int32_t dx = (int32_t)radius * c;
    int32_t dy = (int32_t)radius * s;

    dx = dx >= 0 ? (dx + half) / Z_TRIG_ONE : -((-dx + half) / Z_TRIG_ONE);
    dy = dy >= 0 ? (dy + half) / Z_TRIG_ONE : -((-dy + half) / Z_TRIG_ONE);

    /* Screen y grows downward, so a positive angle turns clockwise on
     * screen. That is what a wheel does, and getting it wrong makes a
     * spin run backwards. */
    *x = cx + (int)dx;
    *y = cy + (int)dy;
}

/* -- spans -------------------------------------------------------------
 *
 * One hardware rectangle fill per scanline, clipped here because
 * z_fb_hw_fill_rect() clamps to the SCREEN and to the window manager's
 * visible region but knows nothing about a caller's own rectangle.
 */
static void span(int x0, int x1, int y, int color, const z_clip_t *clip)
{
    if (clip) {
        if (y < clip->y0 || y > clip->y1) return;
        if (x0 < clip->x0) x0 = clip->x0;
        if (x1 > clip->x1) x1 = clip->x1;
    }

    if (x1 < x0) return;

    z_fb_hw_fill_rect(x0, y, x1 - x0 + 1, 1, color);
}

/* -- circles ------------------------------------------------------------ */

void z_fb_circle(int cx, int cy, int r, int color, const z_clip_t *clip)
{
    int x = 0, y = r;
    int d = 1 - r;

    if (r < 0) return;

    if (r == 0) {
        z_fb_set_pixel(cx, cy, color, clip);
        return;
    }

    /* Midpoint circle. The decision variable stays an integer and the
     * eight-way symmetry means only an octant is stepped. */
    while (x <= y) {
        z_fb_set_pixel(cx + x, cy + y, color, clip);
        z_fb_set_pixel(cx - x, cy + y, color, clip);
        z_fb_set_pixel(cx + x, cy - y, color, clip);
        z_fb_set_pixel(cx - x, cy - y, color, clip);
        z_fb_set_pixel(cx + y, cy + x, color, clip);
        z_fb_set_pixel(cx - y, cy + x, color, clip);
        z_fb_set_pixel(cx + y, cy - x, color, clip);
        z_fb_set_pixel(cx - y, cy - x, color, clip);

        if (d < 0) {
            d += 2 * x + 3;
        } else {
            d += 2 * (x - y) + 5;
            y--;
        }
        x++;
    }
}

/* Half-width of the circle of radius r at vertical offset dy, or -1 if
 * that row is outside it.
 *
 * Derived from r^2 - dy^2 by integer square root rather than from the
 * midpoint decision variable, because a filled circle wants EVERY row's
 * extent and the midpoint walk only visits an octant -- reconstructing
 * the rest from it is where the classic "filled circle has notches on
 * the diagonals" bug comes from.
 */
static int half_width(int r, int dy)
{
    int t = r * r - dy * dy;
    int x;

    if (t < 0) return -1;

    /* Integer square root by bisection on the result, not on the
     * radicand: at most six iterations for any radius that fits a
     * screen, and no division. */
    x = 0;
    while ((x + 1) * (x + 1) <= t) x++;

    return x;
}

void z_fb_fill_circle(int cx, int cy, int r, int color, const z_clip_t *clip)
{
    int dy;

    if (r < 0) return;

    for (dy = -r; dy <= r; dy++) {
        int w = half_width(r, dy);
        if (w < 0) continue;
        span(cx - w, cx + w, cy + dy, color, clip);
    }
}

void z_fb_fill_ring(int cx, int cy, int r_inner, int r_outer, int color,
    const z_clip_t *clip)
{
    int dy;

    if (r_outer < 0) return;
    if (r_inner < 0) r_inner = 0;
    if (r_inner > r_outer) return;

    for (dy = -r_outer; dy <= r_outer; dy++) {
        int wo = half_width(r_outer, dy);

        /* r_inner - 1, not r_inner.
         *
         * The band is INCLUSIVE of both radii, so what has to be cut
         * out is everything strictly inside r_inner. Cutting out
         * r_inner itself makes a ring whose radii are equal draw
         * nothing at all -- the two spans collapse to zero width and
         * a one-pixel ring silently disappears, which is exactly the
         * case a gauge bezel or a wheel rim asks for. */
        int wi = (r_inner > 0) ? half_width(r_inner - 1, dy) : -1;

        if (wo < 0) continue;

        /* Above and below the inner circle the ring is solid, so one
         * span covers the row. */
        if (wi < 0) {
            span(cx - wo, cx + wo, cy + dy, color, clip);
            continue;
        }

        span(cx - wo, cx - wi - 1, cy + dy, color, clip);
        span(cx + wi + 1, cx + wo, cy + dy, color, clip);
    }
}

void z_fb_fill_quad(const int *xs, const int *ys, int color,
    const z_clip_t *clip)
{
    int ymin = ys[0], ymax = ys[0];
    int i, y;

    for (i = 1; i < 4; i++) {
        if (ys[i] < ymin) ymin = ys[i];
        if (ys[i] > ymax) ymax = ys[i];
    }

    if (clip) {
        if (ymin < clip->y0) ymin = clip->y0;
        if (ymax > clip->y1) ymax = clip->y1;
    }

    for (y = ymin; y <= ymax; y++) {
        int xlo = 0, xhi = 0;
        bool any = false;

        /* For a CONVEX shape, the span at any row runs from the
         * leftmost to the rightmost edge crossing -- there is no need
         * to sort crossings or pair them up, which is what a general
         * polygon filler spends its time on. */
        for (i = 0; i < 4; i++) {
            int j = (i + 1) & 3;
            int y0 = ys[i], y1 = ys[j];
            int x0 = xs[i], x1 = xs[j];
            int x;

            if (y0 == y1) {
                /* A horizontal edge contributes both its ends. */
                if (y != y0) continue;
                if (!any) { xlo = xhi = x0; any = true; }
                if (x0 < xlo) xlo = x0;
                if (x0 > xhi) xhi = x0;
                if (x1 < xlo) xlo = x1;
                if (x1 > xhi) xhi = x1;
                continue;
            }

            if (y < (y0 < y1 ? y0 : y1)) continue;
            if (y > (y0 < y1 ? y1 : y0)) continue;

            x = x0 + ((x1 - x0) * (y - y0)) / (y1 - y0);

            if (!any) { xlo = xhi = x; any = true; }
            if (x < xlo) xlo = x;
            if (x > xhi) xhi = x;
        }

        if (any) span(xlo, xhi, y, color, clip);
    }
}

void z_fb_spoke(int cx, int cy, int r0, int r1, int32_t a, int color,
    const z_clip_t *clip)
{
    int x0, y0, x1, y1;

    z_polar(cx, cy, r0, a, &x0, &y0);
    z_polar(cx, cy, r1, a, &x1, &y1);

    z_fb_hw_line(x0, y0, x1, y1, color, clip);
}
