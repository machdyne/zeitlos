#ifndef RL_WHEEL_H
#define RL_WHEEL_H

/*
 * Zeitlos roulette -- the wheel, drawn and spun.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- the pocket order is not 0, 1, 2, 3 --
 *
 * A roulette wheel's pockets run in a fixed, deliberately scrambled
 * sequence so that neighbouring numbers on the rim are far apart on the
 * betting layout. The European order begins 0, 32, 15, 19, 4, 21 and
 * the American one is different again.
 *
 * Carrying the real sequence rather than counting 0 to 36 is most of
 * what makes the wheel look like a wheel, and it buys a free check on
 * something else: ON A EUROPEAN WHEEL THE COLOURS ALTERNATE ALL THE WAY
 * ROUND apart from the zero. Red, black, red, black, thirty-six times.
 *
 * That is a second, independent test of the red set in rl_table.c --
 * one written-out list validating another. Get a single number's colour
 * wrong and the alternation breaks somewhere.
 *
 * -- the head turns, and the ball goes the other way --
 *
 * A real wheel spins its head one way while the ball orbits the other,
 * which is most of what makes one look alive. The first version here
 * rotated only the three hub spokes and left the pockets fixed, so the
 * wheel read as a static ring with a dot going round it.
 *
 * The pockets sweep now, which means the rim is redrawn every frame
 * rather than once -- about nineteen quad fills, thirty-eight
 * separators and two circle outlines. That is affordable because the
 * clear is a RING, not the whole disc: the wall, the hub and everything
 * outside the band stay put.
 *
 * Because the head moves, the pocket the ball lands in depends on where
 * the head has got to. The head turns at a fixed rate and the spin has
 * a known length, so where it will BE is known when the spin starts --
 * rl_wheel_spin() aims the ball at that future position. Nothing is
 * decided by timing.
 *
 * -- how the spin is animated --
 *
 * The result is decided FIRST, by the generator, and the animation is
 * then made to land on it. The alternative -- spinning freely and
 * reading off wherever it stops -- sounds more honest and is worse: the
 * stopping pocket would be a function of frame timing, so the odds
 * would silently depend on how fast the board is.
 *
 * The ball's angle is an EASE, not an integration. Each frame computes
 * the angle directly from the frame number:
 *
 *     angle(f) = start + travel * ease(f / frames)
 *
 * with a cubic ease-out, so it arrives exactly on the target at the
 * last frame with no accumulated error and no final snap. Integrating a
 * decelerating velocity instead would drift by a pocket or two over a
 * few hundred frames, and the fix for that is always an ugly correction
 * at the end that looks like what it is.
 *
 * -- why it is fast --
 *
 * The rim is drawn ONCE. Each frame touches only what moved: the ball
 * and the hub spokes. rl_wheel_dirty() reports the small rectangles
 * that changed, and the app redraws the wheel clipped to those --
 * restoring the background exactly, because the same drawing code
 * produced it.
 *
 * That is a few hundred pixels a frame instead of a few thousand, which
 * is what makes this smooth on a 48MHz core without needing a page flip
 * or an off-screen buffer.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zshape.h"
#include "rl_table.h"

/* The pocket sequence around the rim, starting at zero and running
 * clockwise. Returns the number of pockets, and writes them to `out`,
 * which must hold RL_MAX_POCKETS. */
int rl_wheel_order(int wheel, int *out);

/* Where pocket `p` sits on the rim, in zshape turns-units. */
int32_t rl_wheel_angle(int wheel, int p);

/* Which pocket the rim position `a` falls in. RIM-RELATIVE: `a` is
 * measured against the head, not the screen, so this does not change as
 * the wheel turns. */
int rl_wheel_at(int wheel, int32_t a);


typedef struct {
    int      wheel;
    int      cx, cy, r;      /* centre and outer radius */

    int32_t  ball_a;         /* current ball angle  */
    int32_t  rim_a;          /* the wheel head's rotation */
    int32_t  rim_rate;       /* turns-units per frame, opposite the ball */
    int32_t  rim_end;        /* where the head will be when the ball lands */
    int32_t  rim_drawn;      /* the head's angle when the rim was last painted */
    int      ball_r;         /* current ball radius -- it drops inward */
    int32_t  hub_a;          /* current hub angle   */

    /* Set by rl_wheel_spin() and read by rl_wheel_step(). */
    int32_t  start_a;
    int32_t  travel;         /* total angle the ball will cover */
    int      frame;
    int      frames;
    int      result;         /* the pocket it will land in, decided up front */

    bool     spinning;

    /* Where the ball and hub were last frame, so the app knows what to
     * repair. */
    int32_t  prev_ball_a;
    int      prev_ball_r;
    int32_t  prev_hub_a;
} rl_wheel_t;

void rl_wheel_init(rl_wheel_t *w, int wheel, int cx, int cy, int r);

/* Which pocket the ball is actually sitting in, accounting for where
 * the head has rotated to. This is the one an app asks -- rl_wheel_at()
 * is rim-relative and does not know the wheel has turned. */
int rl_wheel_landed(const rl_wheel_t *w);

/* Starts a spin that will land on `result` after `frames` frames. */
void rl_wheel_spin(rl_wheel_t *w, int result, int frames);

/* Advances one frame. Returns true while still spinning. */
bool rl_wheel_step(rl_wheel_t *w);

/* The whole wheel: a patch plus the ball. For a full repaint. */
void rl_wheel_draw(const rl_wheel_t *w, const z_clip_t *clip);

/* The turning parts, painted IN PLACE with no clear -- each pocket in
 * its own colour, so every pixel of the band is written once and there
 * is never a moment when the ring is blank. This is what a spin frame
 * calls; rl_wheel_draw() clears first and is for a full repaint. */
void rl_wheel_rim(const rl_wheel_t *w, const z_clip_t *clip);

/* Restores a patch from scratch -- for erasing where the ball was, the
 * only thing in a frame that needs clearing. */
void rl_wheel_patch(const rl_wheel_t *w, const z_clip_t *clip);

/* True when the head has turned far enough for a rim repaint to change
 * a pixel, and records that it has been repainted. Most frames it has
 * not -- see rl_wheel.c. */
bool rl_wheel_rim_due(rl_wheel_t *w);

/* What the ball and its streak cover this frame, for a frame that skips
 * the rim and still has to restore what the ball was on top of. */
void rl_wheel_ball_bbox(const rl_wheel_t *w, z_clip_t *out);

/* Clears the ball's track. Free visually -- that band holds nothing but
 * the ball, so it is black over black everywhere else. */
void rl_wheel_track_clear(const rl_wheel_t *w, const z_clip_t *clip);

/* The ball, drawn as the arc it travelled this frame rather than as a
 * point: continuous at any speed, and a dot again once it slows. */
void rl_wheel_ball(const rl_wheel_t *w, const z_clip_t *clip);

/* The rectangle covering the ball at (`a`, `r`), and the one covering
 * the hub. An app redraws the wheel clipped to these to repair the
 * previous frame -- passing prev_ball_a and prev_ball_r.
 *
 * THE RADIUS IS A PARAMETER, not read from the wheel. The ball moves
 * inward as well as round during the drop, so a rectangle sized to
 * cover the whole radial travel would be a quarter of the wheel wide
 * and every frame would repaint most of it -- which is the cost this
 * whole design exists to avoid. Given both coordinates of where the
 * ball actually was, the rectangle stays a dozen pixels across. */
void rl_wheel_ball_rect(const rl_wheel_t *w, int32_t a, int r,
    z_clip_t *out);
void rl_wheel_hub_rect(const rl_wheel_t *w, z_clip_t *out);

/* The smallest radius the wheel can be drawn at and still read. */
#define RL_WHEEL_MIN_R 24

#endif
