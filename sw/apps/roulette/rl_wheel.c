/*
 * Zeitlos roulette -- the wheel, drawn and spun.
 * See rl_wheel.h for the pocket order and how the spin is animated.
 */

#include "rl_wheel.h"

/* -- the rim -----------------------------------------------------------
 *
 * The real sequences, written out. See rl_wheel.h: on the European
 * wheel these alternate red and black all the way round after the zero,
 * which tests/wheel_test.c checks against rl_table.c's red set -- one
 * written-out list validating another.
 */
static const uint8_t order_euro[37] = {
     0, 32, 15, 19,  4, 21,  2, 25, 17, 34,
     6, 27, 13, 36, 11, 30,  8, 23, 10,  5,
    24, 16, 33,  1, 20, 14, 31,  9, 22, 18,
    29,  7, 28, 12, 35,  3, 26
};

/* The American wheel is a different sequence and is laid out in
 * opposing pairs: 1 faces 2, 3 faces 4, and so on, with the two zeros
 * opposite each other. Its colours do NOT alternate cleanly, which is
 * why the alternation test applies to the European wheel only. */
static const uint8_t order_amer[38] = {
     0, 28,  9, 26, 30, 11,  7, 20, 32, 17,
     5, 22, 34, 15,  3, 24, 36, 13,  1, RL_DOUBLE_ZERO,
    27, 10, 25, 29, 12,  8, 19, 31, 18,  6,
    21, 33, 16,  4, 23, 35, 14,  2
};

int rl_wheel_order(int wheel, int *out)
{
    const uint8_t *src = (wheel == RL_AMERICAN) ? order_amer : order_euro;
    int n = rl_pockets(wheel);
    int i;

    for (i = 0; i < n; i++) out[i] = src[i];

    return n;
}

static int rim_index(int wheel, int p)
{
    const uint8_t *src = (wheel == RL_AMERICAN) ? order_amer : order_euro;
    int n = rl_pockets(wheel);
    int i;

    for (i = 0; i < n; i++) if (src[i] == p) return i;

    return -1;
}

int32_t rl_wheel_angle(int wheel, int p)
{
    int i = rim_index(wheel, p);
    int n = rl_pockets(wheel);

    if (i < 0) return 0;

    /* The CENTRE of the pocket, not its leading edge -- so the ball
     * comes to rest in the middle of a cell rather than on the line
     * between two of them. */
    return (int32_t)(((int32_t)i * 2 + 1) * Z_TRIG_TURN) / (2 * n);
}

int rl_wheel_at(int wheel, int32_t a)
{
    const uint8_t *src = (wheel == RL_AMERICAN) ? order_amer : order_euro;
    int n = rl_pockets(wheel);
    int i;

    a &= (Z_TRIG_TURN - 1);

    /* Plain floor division. Cell i spans [i*TURN/n, (i+1)*TURN/n), and
     * rl_wheel_angle() returns a point INSIDE that range, so no offset
     * is wanted here.
     *
     * A half-cell offset was added at first, reasoning that the angles
     * are cell centres -- which they are, and which is exactly why the
     * offset is wrong: a centre already sits in its own cell, and
     * nudging it forward pushes every second pocket into its
     * neighbour. The round-trip test caught it on pocket 10. */
    i = (int)((a * n) / Z_TRIG_TURN);
    if (i >= n) i -= n;

    return src[i];
}

/* A spin is long, and most of it is at speed.
 *
 * The first version eased from the very first frame, so the ball made
 * almost all of its travel in the opening half-second and then crept --
 * reported from the device as "spins very fast then stops suddenly".
 * That is what a pure ease-out looks like when you watch it rather than
 * plot it: the deceleration is all at the start, where it reads as the
 * ball being flung, and the long tail reads as a stop.
 *
 * A real ball holds its speed on the track for a while, slows over
 * several revolutions, then drops. So the profile below is piecewise --
 * see ease() -- and the spin is longer to give each phase room. */
/* Six, not nine.
 *
 * At nine over 120 frames the peak is 43 degrees per frame -- on a
 * 41-pixel track that is 31 pixels between one frame and the next, for
 * a ball five pixels across. It never lands next to where it just was,
 * so it reads as a dot blinking at scattered positions rather than as
 * something moving. Six brings the peak to about 21 pixels, and the
 * streak in rl_wheel_ball() covers the rest. */
#define SPIN_TURNS 6        /* full revolutions before it settles */

/* How far through the spin the ball leaves the outer track and falls
 * inward into a pocket, as a fraction of the way through.
 *
 * -- why this exists at all --
 *
 * Any ease-out that brings the ball to REST has a tail where it moves
 * less than a pixel per frame. A cubic ease over ninety frames spends
 * its last thirty covering under half a pixel in total: the animation
 * is technically still running and looks frozen, which is worse than
 * being over.
 *
 * Shortening the ease does not fix it -- decelerating to zero always
 * ends in sub-pixel motion, and that is also what a real ball does.
 * What a real ball ALSO does is drop off the track into a pocket, and
 * that movement is radial rather than angular. So the tail is filled
 * with the drop. The ball keeps visibly moving right up to the moment
 * it lands, because for the last third it is moving inward rather than
 * round.
 */
#define DROP_AT ((Z_TRIG_ONE * 80) / 100)

/* The speed profile, as fractions of the way through a spin.
 *
 *   0 .. HOLD_TO     full speed, constant
 *   HOLD_TO .. SLOW_TO   decelerating, linearly in speed
 *   SLOW_TO .. 1      a slow crawl into the pocket
 *
 * Constant speed first is the part that was missing. Deceleration that
 * starts immediately is not a spin, it is a flick. */
#define HOLD_TO  ((Z_TRIG_ONE * 30) / 100)
#define SLOW_TO  ((Z_TRIG_ONE * 88) / 100)
#define SPEED_MIN (Z_TRIG_ONE / 9)      /* crawl speed, as a fraction */

/* How far through the TRAVEL the ball is at `u` through the spin: the
 * integral of the speed profile above, normalised so ease(ONE) == ONE.
 *
 * Integer throughout -- some boards here are rv32i, where a single
 * double links the soft-float runtime. */
static int32_t ease_raw(int32_t u)
{
    int64_t e;

    if (u <= HOLD_TO) return u;

    if (u <= SLOW_TO) {
        int64_t span = SLOW_TO - HOLD_TO;
        int64_t t = ((int64_t)(u - HOLD_TO) * Z_TRIG_ONE) / span;
        /* v(t) = SPEED_MIN + (1 - SPEED_MIN)(1 - t), integrated. */
        int64_t lin = ((int64_t)SPEED_MIN * t) / Z_TRIG_ONE;
        int64_t dec = ((int64_t)(Z_TRIG_ONE - SPEED_MIN) *
            (t - (t * t) / (2 * Z_TRIG_ONE))) / Z_TRIG_ONE;
        e = HOLD_TO + (span * (lin + dec)) / Z_TRIG_ONE;
        return (int32_t)e;
    }

    {
        int64_t span = SLOW_TO - HOLD_TO;
        int64_t at_b = HOLD_TO + (span * ((Z_TRIG_ONE + SPEED_MIN) / 2)) /
            Z_TRIG_ONE;
        e = at_b + ((int64_t)SPEED_MIN * (u - SLOW_TO)) / Z_TRIG_ONE;
        return (int32_t)e;
    }
}

static int32_t ease(int32_t u)
{
    int32_t full = ease_raw(Z_TRIG_ONE);
    if (full <= 0) return u;
    return (int32_t)(((int64_t)ease_raw(u) * Z_TRIG_ONE) / full);
}

/* The head's rotation, in turns-units per frame, opposite the ball.
 * Slow: a real head turns a few times while the ball goes round dozens,
 * and a fast head makes the pockets strobe rather than sweep. */
#define RIM_RATE (-2)

/* -- geometry ----------------------------------------------------------
 *
 * Radii as fractions of the outer radius, so a wheel drawn at 30 pixels
 * and one drawn at 60 have the same proportions.
 */
/* The pockets sit INSIDE an outer wall, with a dark gap between them
 * for the ball to run in -- which is both what a real wheel looks like
 * and the only way a white ball is visible on a two-colour display.
 *
 * The first version put the ball track at 88% of the radius, in the
 * middle of the lit rim, and a white ball on a white pocket was very
 * nearly invisible. Nothing in the tests noticed, because a ball drawn
 * in exactly the right place is in exactly the right place whether or
 * not you can see it. The render showed it immediately. */
static int r_rim_out(const rl_wheel_t *w)  { return (w->r * 86) / 100; }
static int r_rim_in(const rl_wheel_t *w)   { return (w->r * 62) / 100; }
static int r_track(const rl_wheel_t *w)    { return (w->r * 94) / 100; }
static int r_hub(const rl_wheel_t *w)      { return (w->r * 40) / 100; }
/* Small again. r/7 was tried to make the ball easier to follow and was
 * reported as too big -- which it was, because size was the wrong
 * lever: the ball was hard to see because it JUMPED, not because it was
 * faint. See rl_wheel_ball(). */
static int r_ball(const rl_wheel_t *w)     { int b = w->r / 9; return b < 2 ? 2 : b; }

/* Where the ball comes to rest: toward the OUTER edge of the pocket
 * rather than the middle of it.
 *
 * That is where a ball actually sits -- it is thrown outward -- and it
 * also halves how much of it the rim repaint covers each frame, which
 * is the difference between a ball that flickers under the wheel while
 * it slows and one that sits on top of it. */
static int r_pocket(const rl_wheel_t *w)
{
    return (r_rim_in(w) + 3 * r_rim_out(w)) / 4;
}

/* Where the ball is, radially. It rides the outer track for most of a
 * spin and DROPS INTO A POCKET at the end -- see rl_wheel_step(). */
static int ball_radius(const rl_wheel_t *w)
{
    int32_t u, k;
    int track = r_track(w), pocket = r_pocket(w);

    if (!w->spinning) return pocket;
    if (w->frames <= 0) return pocket;

    u = ((int32_t)w->frame * Z_TRIG_ONE) / w->frames;
    if (u < DROP_AT) return track;

    k = ((u - DROP_AT) * Z_TRIG_ONE) / (Z_TRIG_ONE - DROP_AT);

    return track + (int)(((int64_t)(pocket - track) * k) / Z_TRIG_ONE);
}

void rl_wheel_init(rl_wheel_t *w, int wheel, int cx, int cy, int r)
{
    int i;

    for (i = 0; i < (int)sizeof(rl_wheel_t); i++) ((char *)w)[i] = 0;

    w->wheel = wheel;
    w->cx = cx;
    w->cy = cy;
    w->r = r < RL_WHEEL_MIN_R ? RL_WHEEL_MIN_R : r;
    w->result = -1;
    w->rim_a = 0;
    w->rim_drawn = 0;
    w->rim_rate = RIM_RATE;
    w->rim_end = 0;
    w->ball_a = rl_wheel_angle(wheel, 0);
    w->ball_r = r_pocket(w);
    w->prev_ball_a = w->ball_a;
    w->prev_ball_r = w->ball_r;
}

/* -- the spin ----------------------------------------------------------- */



int rl_wheel_landed(const rl_wheel_t *w)
{
    return rl_wheel_at(w->wheel, w->ball_a - w->rim_a);
}

void rl_wheel_spin(rl_wheel_t *w, int result, int frames)
{
    int32_t target;
    int32_t delta;

    if (frames < 2) frames = 2;

    /* WHERE THE HEAD WILL BE, not where it is.
     *
     * The pockets move, so the ball has to be aimed at the result's
     * position at the END of the spin. The head turns at a fixed rate
     * for a known number of frames, so that position is arithmetic --
     * nothing depends on how long a frame actually took, which is what
     * keeps the odds independent of the board's speed. */
    w->rim_rate = RIM_RATE;
    w->rim_end = (w->rim_a + w->rim_rate * frames) & (Z_TRIG_TURN - 1);
    target = (w->rim_end + rl_wheel_angle(w->wheel, result)) &
        (Z_TRIG_TURN - 1);

    w->result = result;
    w->start_a = w->ball_a;
    w->frame = 0;
    w->frames = frames;
    w->spinning = true;

    /* Forwards only. The remainder is taken on the positive side so the
     * ball never appears to reverse at the last moment to reach a
     * target that was just behind it. */
    delta = (target - w->start_a) & (Z_TRIG_TURN - 1);
    w->travel = (int32_t)SPIN_TURNS * Z_TRIG_TURN + delta;
}

bool rl_wheel_step(rl_wheel_t *w)
{
    int32_t u, e, inv;

    w->prev_ball_a = w->ball_a;
    w->prev_ball_r = w->ball_r;
    w->prev_hub_a = w->hub_a;

    if (!w->spinning) return false;

    w->rim_a = (w->rim_a + w->rim_rate) & (Z_TRIG_TURN - 1);

    w->frame++;
    if (w->frame >= w->frames) {
        /* Landed. Set from the target rather than from the easing
         * function's last step, so the final position is exact even if
         * the arithmetic below rounded. */
        /* Set from the target rather than from the easing function's
         * last step, so the ball ends exactly in its pocket even if the
         * arithmetic rounded -- and the head is placed exactly where
         * the spin said it would be, for the same reason. */
        w->rim_a = w->rim_end;
        w->ball_a = (w->rim_end + rl_wheel_angle(w->wheel, w->result)) &
            (Z_TRIG_TURN - 1);
        w->spinning = false;
        w->ball_r = r_pocket(w);
        return false;
    }

    /* Computed from the FRAME NUMBER, not by integrating a velocity.
     * An integrator drifts over a few hundred frames and the usual fix
     * is a correction on the last frame, which looks exactly like what
     * it is. This arrives on the target because it is defined to. */
    u = ((int32_t)w->frame * Z_TRIG_ONE) / w->frames;
    (void)inv;
    e = ease(u);

    w->ball_a = (w->start_a +
        (int32_t)(((int64_t)w->travel * e) / Z_TRIG_ONE)) &
        (Z_TRIG_TURN - 1);

    /* The hub turns the other way, slowly, and keeps turning at a
     * constant rate. A real wheel's head spins while the ball orbits
     * against it, and two things moving at different speeds is most of
     * what makes this read as motion rather than as a sliding dot. */
    w->ball_r = ball_radius(w);

    w->hub_a = (w->hub_a - 3) & (Z_TRIG_TURN - 1);

    return true;
}

/* -- drawing ------------------------------------------------------------ */

void rl_wheel_ball_rect(const rl_wheel_t *w, int32_t a, int r, z_clip_t *out)
{
    int x, y, br = r_ball(w) + 2;

    z_polar(w->cx, w->cy, r, a, &x, &y);

    out->x0 = x - br;
    out->y0 = y - br;
    out->x1 = x + br;
    out->y1 = y + br;
}

void rl_wheel_hub_rect(const rl_wheel_t *w, z_clip_t *out)
{
    int hr = r_hub(w) + 1;

    out->x0 = w->cx - hr;
    out->y0 = w->cy - hr;
    out->x1 = w->cx + hr;
    out->y1 = w->cy + hr;
}

/* -- drawing, split so a frame does not have to clear anything big --
 *
 * The first version cleared a disc and then filled the whole rim white
 * before cutting the red pockets back out of it, every frame. Three
 * full repaints of the same annulus, straight to the visible page,
 * which on the device is exactly what it sounds like: a flashing ring.
 *
 * Each pocket is painted in its OWN colour now. The pockets tile the
 * band, so painting all of them covers whatever was there before --
 * every pixel is written once per frame instead of three times, and
 * there is no moment when the ring is blank. The only thing still
 * cleared is the handful of pixels the ball just left.
 */
void rl_wheel_rim(const rl_wheel_t *w, const z_clip_t *clip)
{
    int n = rl_pockets(w->wheel);
    int order[RL_MAX_POCKETS];
    int i, k, start;
    int ro = r_rim_out(w), ri = r_rim_in(w);

    rl_wheel_order(w->wheel, order);

    /* THE POCKET UNDER THE BALL IS PAINTED LAST.
     *
     * Every frame the rim repaint covers the ball and the ball is drawn
     * again afterwards, so there is a window in which it is not on the
     * screen. While the ball is out on the track that costs a pixel of
     * overlap; during the DROP it is inside the band and the rim
     * repaints all of it, which is why it was reported as the wheel
     * being in front of the ball -- and only while slowing.
     *
     * Starting the loop just past the ball's own pocket puts the cell
     * that erases it at the very end, so the gap is the tail of the rim
     * rather than the whole of it. It cannot be closed completely while
     * drawing straight to the visible page; this makes it small. */
    {
        int32_t rel = (w->ball_a - w->rim_a) & (Z_TRIG_TURN - 1);
        start = (int)((rel * n) / Z_TRIG_TURN) + 1;
    }

    for (k = 0; k < n; k++) {
        int i = (start + k) % n;
        int p = order[i];
        int32_t a0 = w->rim_a + ((int32_t)i * Z_TRIG_TURN) / n;
        int32_t a1 = w->rim_a + ((int32_t)(i + 1) * Z_TRIG_TURN) / n;
        int xs[4], ys[4];

        /* A pocket is a QUAD, not a fan of radial lines: closely spaced
         * spokes diverge as the radius grows and leave the outer end
         * speckled. The chord-versus-arc error at 38 cells is about a
         * third of a pixel. */
        z_polar(w->cx, w->cy, ri, a0, &xs[0], &ys[0]);
        z_polar(w->cx, w->cy, ro, a0, &xs[1], &ys[1]);
        z_polar(w->cx, w->cy, ro, a1, &xs[2], &ys[2]);
        z_polar(w->cx, w->cy, ri, a1, &xs[3], &ys[3]);

        z_fb_fill_quad(xs, ys, rl_is_red(p) ? 0 : 1, clip);
    }

    /* The separators, over the top, so every cell is bounded. */
    for (i = 0; i < n; i++) {
        int32_t a = w->rim_a + ((int32_t)i * Z_TRIG_TURN) / n;
        z_fb_spoke(w->cx, w->cy, ri, ro, a, 1, clip);
    }

    /* THE ZEROS GET A SLASH. They are lit like the black pockets, so
     * without it the one pocket that is neither colour looks like
     * eighteen others -- and on a wheel with no numbers it is the only
     * landmark there is. */
    for (i = 0; i < n; i++) {
        int p = order[i];
        int32_t am;
        if (p != RL_ZERO && p != RL_DOUBLE_ZERO) continue;
        am = w->rim_a + (((int32_t)i * 2 + 1) * Z_TRIG_TURN) / (2 * n);
        z_fb_spoke(w->cx, w->cy, ri + 1, ro - 1, am, 0, clip);
    }

    z_fb_circle(w->cx, w->cy, ro, 1, clip);
    z_fb_circle(w->cx, w->cy, ri, 1, clip);

    /* The hub, which is part of the head and turns with it. */
    z_fb_fill_circle(w->cx, w->cy, r_hub(w), 0, clip);
    z_fb_circle(w->cx, w->cy, r_hub(w), 1, clip);

    for (i = 0; i < 3; i++) {
        int32_t a = w->hub_a + (int32_t)i * (Z_TRIG_TURN / 3);
        z_fb_spoke(w->cx, w->cy, 1, r_hub(w) - 1, a, 1, clip);
    }

    z_fb_fill_circle(w->cx, w->cy, 1, 1, clip);
}

/* Restores a patch of the wheel from scratch -- used to erase the
 * ball's previous position, which is the only thing that needs a
 * clear. Small, so the clear is invisible. */
void rl_wheel_patch(const rl_wheel_t *w, const z_clip_t *clip)
{
    z_fb_fill_circle(w->cx, w->cy, w->r + r_ball(w) + 2, 0, clip);
    rl_wheel_rim(w, clip);
    z_fb_circle(w->cx, w->cy, w->r, 1, clip);
}

/* Clears the ball's track -- the annulus between the rim and the outer
 * wall.
 *
 * FREE, VISUALLY. That band holds nothing but the ball, so clearing it
 * is black over black everywhere the ball is not. It replaces the
 * old-position bookkeeping entirely: no rectangle to remember, no
 * chance of the erase and the draw disagreeing about where the ball
 * was.
 *
 * It is also why XOR is not needed here. XOR-to-erase restores what was
 * underneath by writing the same pixels twice -- which assumes the
 * background has not changed in between. This one rotates.
 */
/* True when the head has turned far enough that repainting the rim
 * would actually change a pixel -- and records that it has been
 * repainted.
 *
 * THE RIM BARELY MOVES. At RIM_RATE the head turns about four tenths of
 * a pixel per frame at the outer edge of the band, so most frames would
 * repaint it into exactly the same pixels. That is wasted work, and
 * worse, every one of those repaints covers the ball and opens the
 * window in which it is not on the screen.
 *
 * Skipping the ones that change nothing is invisible -- the rim is in
 * the same place either way -- and it cuts both the cost and the
 * flicker by the same factor, which at this rate is about three.
 */
bool rl_wheel_rim_due(rl_wheel_t *w)
{
    int32_t moved = (w->rim_a - w->rim_drawn) & (Z_TRIG_TURN - 1);
    int px;

    if (moved > Z_TRIG_TURN / 2) moved = Z_TRIG_TURN - moved;

    /* Arc in pixels at the rim's outer edge, 2*pi*r*moved/TURN with
     * pi*2 as 6284/1000 -- integer, for the rv32i boards. */
    px = (int)(((int64_t)r_rim_out(w) * 6284 * moved) /
        (1000 * Z_TRIG_TURN));

    if (px < 1) return false;

    w->rim_drawn = w->rim_a;
    return true;
}

/* The rectangle the ball and its streak occupy this frame, so a frame
 * that skips the rim can still restore what the ball was covering. */
void rl_wheel_ball_bbox(const rl_wheel_t *w, z_clip_t *out)
{
    int x0, y0, x1, y1, pad = r_ball(w) + 3;

    z_polar(w->cx, w->cy, w->prev_ball_r, w->prev_ball_a, &x0, &y0);
    z_polar(w->cx, w->cy, w->ball_r, w->ball_a, &x1, &y1);

    out->x0 = (x0 < x1 ? x0 : x1) - pad;
    out->y0 = (y0 < y1 ? y0 : y1) - pad;
    out->x1 = (x0 > x1 ? x0 : x1) + pad;
    out->y1 = (y0 > y1 ? y0 : y1) + pad;
}

void rl_wheel_track_clear(const rl_wheel_t *w, const z_clip_t *clip)
{
    z_fb_fill_ring(w->cx, w->cy, r_rim_out(w) + 1,
        w->r + r_ball(w) + 2, 0, clip);

    /* The wall lives in the band being cleared, so it goes with it. Put
     * it back. The first version did not, and the wheel lost its outer
     * ring the moment a spin started -- which the render showed and no
     * assertion would have, because the wall is not something any test
     * had a reason to count. */
    z_fb_circle(w->cx, w->cy, w->r, 1, clip);
}

/* The ball, drawn as the distance it travelled this frame rather than
 * as a point.
 *
 * A ball moving twenty pixels between frames is not a ball, it is a
 * dot appearing in a new place -- and no amount of making it bigger
 * fixes that, which is what the last attempt got wrong. Drawn as a
 * streak from where it was to where it is, it is continuous at any
 * speed the wheel can reach, and the streak shortens to a dot as it
 * slows. That is motion blur, and it is what a camera would show.
 *
 * Tapered, head to tail, so it reads as a direction rather than as a
 * sausage. Only the head gets the dark halo: it is what keeps the ball
 * visible when the drop takes it onto a lit pocket, and a halo on every
 * segment would smear the tail into a grey smudge.
 */
void rl_wheel_ball(const rl_wheel_t *w, const z_clip_t *clip)
{
    int32_t da = (w->ball_a - w->prev_ball_a) & (Z_TRIG_TURN - 1);
    int rb = r_ball(w);
    int steps, i, x, y;

    /* Arc length in pixels, as 2*pi*r*da/TURN with pi*2 as 6284/1000 --
     * integer, because a single double would link the soft-float
     * runtime on an rv32i board. */
    {
        int arc = (int)(((int64_t)w->ball_r * 6284 * da) /
            (1000 * Z_TRIG_TURN));
        steps = arc / 2;
        if (steps < 1) steps = 1;
        if (steps > 28) steps = 28;
    }

    for (i = 0; i <= steps; i++) {
        /* Interpolated in BOTH angle and radius, so the streak follows
         * the ball through the drop rather than cutting the corner. */
        int32_t a = w->prev_ball_a + (int32_t)(((int64_t)da * i) / steps);
        int r = w->prev_ball_r +
            ((w->ball_r - w->prev_ball_r) * i) / steps;
        int sz = 1 + ((rb - 1) * i) / steps;

        z_polar(w->cx, w->cy, r, a, &x, &y);
        if (i == steps) z_fb_fill_circle(x, y, sz + 2, 0, clip);
        z_fb_fill_circle(x, y, sz, 1, clip);
    }
}

void rl_wheel_draw(const rl_wheel_t *w, const z_clip_t *clip)
{
    rl_wheel_patch(w, clip);
    rl_wheel_ball(w, clip);
}
