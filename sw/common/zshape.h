#ifndef ZSHAPE_H
#define ZSHAPE_H

/*
 * Zeitlos -- circles, arcs, and the trig they need.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zgfx.h has lines, boxes, rectangle fills and shaded spans. It has no
 * circle, and rtl/gpu has no circle either -- so these are built on
 * top of what is there rather than being new hardware.
 *
 * -- why this is its own object and not part of zgfx.c --
 *
 * There is no MMIO in this file. Every function here is arithmetic
 * that ends in z_fb_set_pixel() or z_fb_hw_fill_rect(), both of which
 * sw/common/tests/zrender.h already supplies in software.
 *
 * That means zshape.c LINKS INTO A HOST TEST, and zgfx.c cannot -- it
 * is register writes from top to bottom. A circle whose only proof is
 * "it looked round on the device" is a circle nobody will dare change,
 * so the one that can be tested off the device is worth a separate
 * object file. --gc-sections drops it from anything that never draws
 * one.
 *
 * -- a filled circle is spans, not pixels --
 *
 * z_fb_fill_circle() computes each scanline's extent and issues ONE
 * hardware rectangle fill per row: 2r+1 blitter operations instead of
 * pi*r^2 CPU writes. At r = 36 that is 73 fills instead of about four
 * thousand pixel writes, and the difference is what makes an animated
 * wheel possible at all.
 *
 * The outline is genuinely per-pixel -- there is nothing to span --
 * but it is only about 8r points.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zgfx.h"

/* -- trig ------------------------------------------------------------
 *
 * Angles are in TURNS, not degrees or radians: Z_TRIG_TURN units make
 * one full revolution. A power of two, so that wrapping an angle is a
 * mask and stepping by a fraction of a turn is exact -- no accumulated
 * error in an animation that runs for thousands of frames, and no
 * division on a core that may not have one.
 *
 * Results are scaled by Z_TRIG_ONE. Integer throughout: some boards
 * here are rv32i, where a single double links the soft-float runtime.
 */
#define Z_TRIG_TURN 1024        /* units in a full revolution */
#define Z_TRIG_ONE  4096        /* what 1.0 is scaled to      */

/* sin and cos of `a` turns-units, in [-Z_TRIG_ONE, Z_TRIG_ONE].
 * `a` may be any int32_t; it is wrapped. */
int32_t z_sin(int32_t a);
int32_t z_cos(int32_t a);

/* A point on a circle: centre plus radius at angle `a`. Rounded, not
 * truncated, so a shape drawn this way is symmetric about its centre
 * instead of leaning one pixel toward the origin. */
void z_polar(int cx, int cy, int radius, int32_t a, int *x, int *y);

/* -- circles ---------------------------------------------------------- */

/* A one-pixel outline. Midpoint algorithm, eight-way symmetric. */
void z_fb_circle(int cx, int cy, int r, int color, const z_clip_t *clip);

/* Filled, as one hardware rectangle fill per scanline. */
void z_fb_fill_circle(int cx, int cy, int r, int color, const z_clip_t *clip);

/* A ring: every pixel between `r_inner` and `r_outer` inclusive, filled
 * as two spans per scanline. For a wheel rim, a gauge bezel, a dial.
 *
 * Drawn as spans rather than as two outlines because an outline pair
 * leaves the pixels between them untouched, which is a hole rather
 * than a ring whenever the two radii differ by more than one. */
void z_fb_fill_ring(int cx, int cy, int r_inner, int r_outer, int color,
    const z_clip_t *clip);

/* A convex quadrilateral, filled with one hardware rectangle fill per
 * scanline. `xs` and `ys` are four corners in order round the shape.
 *
 * Here because a wedge of an annulus -- one pocket of a roulette wheel,
 * one segment of a pie chart -- is a quad to within a fraction of a
 * pixel, and filling one by drawing closely spaced radial lines does
 * NOT work: adjacent lines diverge as the radius grows and leave the
 * far end speckled. That was the first attempt and the speckles were
 * plainly visible in a render at r = 90.
 *
 * Convex only, and unchecked. A concave quad fills its convex hull
 * rather than failing, which is the wrong picture but not a crash. */
void z_fb_fill_quad(const int *xs, const int *ys, int color,
    const z_clip_t *clip);

/* A radial line from `r0` to `r1` at angle `a` -- a spoke. */
void z_fb_spoke(int cx, int cy, int r0, int r1, int32_t a, int color,
    const z_clip_t *clip);

#endif
