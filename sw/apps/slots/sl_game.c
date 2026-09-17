/*
 * Zeitlos slots -- the spin.
 * See sl_game.h for why position comes from the frame number.
 */

#include "sl_game.h"

/* Fixed point for the easing. A power of two so the divides are
 * shifts -- some boards here are rv32i with no divider, and this runs
 * three times a frame. */
#define ONE 4096

/* The speed profile, as fractions of the way through a reel's spin:
 * full speed to HOLD_TO, decelerating to SLOW_TO, then a crawl into
 * the stop.
 *
 * Constant speed first is the part that matters. sw/apps/roulette eased
 * from the very first frame and the result was reported as "spins very
 * fast then stops suddenly" -- which is what a pure ease-out looks like
 * when you watch it rather than plot it. A reel that starts decelerating
 * immediately never looks like it is spinning at all. */
#define HOLD_TO  ((ONE * 55) / 100)
#define SLOW_TO  ((ONE * 92) / 100)
#define SPEED_MIN (ONE / 10)

static int32_t ease_raw(int32_t u)
{
    if (u <= HOLD_TO) return u;

    if (u <= SLOW_TO) {
        int64_t span = SLOW_TO - HOLD_TO;
        int64_t t = ((int64_t)(u - HOLD_TO) * ONE) / span;
        int64_t lin = ((int64_t)SPEED_MIN * t) / ONE;
        int64_t dec = ((int64_t)(ONE - SPEED_MIN) *
            (t - (t * t) / (2 * ONE))) / ONE;
        return (int32_t)(HOLD_TO + (span * (lin + dec)) / ONE);
    }

    {
        int64_t span = SLOW_TO - HOLD_TO;
        int64_t at_b = HOLD_TO + (span * ((ONE + SPEED_MIN) / 2)) / ONE;
        return (int32_t)(at_b + ((int64_t)SPEED_MIN * (u - SLOW_TO)) / ONE);
    }
}

static int32_t ease(int32_t u)
{
    int32_t full = ease_raw(ONE);
    if (full <= 0) return u;
    return (int32_t)(((int64_t)ease_raw(u) * ONE) / full);
}

static void clear(sl_spin_t *s)
{
    int i;
    for (i = 0; i < (int)sizeof(sl_spin_t); i++) ((char *)s)[i] = 0;
}

void sl_spin_init(sl_spin_t *s)
{
    int r;

    clear(s);

    for (r = 0; r < SL_REELS; r++) {
        s->stop[r] = 0;
        s->pos[r] = 0;
        s->prev[r] = 0;
    }
}

void sl_spin_begin(sl_spin_t *s, const int *stops)
{
    int r;

    s->frame = 0;
    s->spinning = true;

    for (r = 0; r < SL_REELS; r++) {
        int32_t target, delta;

        s->stop[r] = ((stops[r] % SL_STOPS) + SL_STOPS) % SL_STOPS;
        s->start[r] = s->pos[r];
        s->prev[r] = s->pos[r];

        /* LEFT TO RIGHT: each reel turns longer than the one before
         * it. Two sevens and a reel still moving is the whole
         * experience; three reels stopping together is a dice roll. */
        s->frames[r] = SL_BASE_FRAMES + r * SL_STAGGER;

        /* The offset where the target stop sits at the top of the
         * window. Forward only -- a reel that crept backwards to reach
         * a near target would be unmistakable. */
        target = (int32_t)s->stop[r] * SL_CELL;
        delta = target - s->start[r];
        while (delta < 0) delta += SL_STRIP_PX;

        s->travel[r] = (int32_t)SL_SPIN_TURNS * SL_STRIP_PX + delta;
    }
}

bool sl_spin_step(sl_spin_t *s)
{
    int r;
    bool any = false;

    if (!s->spinning) return false;

    s->frame++;

    for (r = 0; r < SL_REELS; r++) {
        int32_t u, e;

        s->prev[r] = s->pos[r];

        if (s->frame >= s->frames[r]) {
            /* Set from the target, not from the easing function's last
             * step, so a reel ends exactly on its stop however the
             * arithmetic rounded. */
            s->pos[r] = (int32_t)s->stop[r] * SL_CELL;
            continue;
        }

        any = true;

        u = ((int32_t)s->frame * ONE) / s->frames[r];
        e = ease(u);

        s->pos[r] = (s->start[r] +
            (int32_t)(((int64_t)s->travel[r] * e) / ONE)) % SL_STRIP_PX;
    }

    if (!any) s->spinning = false;

    return any;
}

bool sl_reel_stopped(const sl_spin_t *s, int r)
{
    if (r < 0 || r >= SL_REELS) return true;
    return !s->spinning || s->frame >= s->frames[r];
}

int sl_reel_dy(const sl_spin_t *s, int r)
{
    int32_t d;

    if (r < 0 || r >= SL_REELS) return 0;

    d = s->pos[r] - s->prev[r];
    while (d < 0) d += SL_STRIP_PX;

    return (int)d;
}

uint8_t sl_reel_symbol(const sl_spin_t *s, int r, int row)
{
    int32_t p;
    int idx;

    if (r < 0 || r >= SL_REELS) return SL_BLANK;

    p = s->pos[r] % SL_STRIP_PX;
    if (p < 0) p += SL_STRIP_PX;

    idx = (int)(p / SL_CELL) + row;

    return sl_strip[r][((idx % SL_STOPS) + SL_STOPS) % SL_STOPS];
}

int sl_reel_subpixel(const sl_spin_t *s, int r)
{
    int32_t p;

    if (r < 0 || r >= SL_REELS) return 0;

    p = s->pos[r] % SL_STRIP_PX;
    if (p < 0) p += SL_STRIP_PX;

    return (int)(p % SL_CELL);
}
