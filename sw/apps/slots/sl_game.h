#ifndef SL_GAME_H
#define SL_GAME_H

/*
 * Zeitlos slots -- the spin.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * No I/O. Where each reel IS at a given frame, and nothing about how it
 * gets drawn.
 *
 * -- the result is decided first --
 *
 * The stops come from the generator before a single frame is drawn, and
 * the animation is made to land on them. Spinning freely and reading
 * off wherever the reels happened to stop would make the odds a
 * function of frame timing, so a faster board would pay differently --
 * and the whole point of sl_reels.c is that the edge is an exact
 * number.
 *
 * -- position comes from the frame number --
 *
 * Each reel's offset is computed directly from how many frames have
 * elapsed, not by adding a velocity each frame. An integrator drifts
 * over a few hundred frames and the usual fix is a correction on the
 * last one, which looks exactly like what it is -- a reel snapping into
 * place. sw/apps/roulette has the same arrangement for the same reason.
 *
 * -- reels stop left to right --
 *
 * Reel 1 stops, then reel 2, then reel 3. That is where the tension in
 * a slot machine lives: two sevens and one reel still turning. It also
 * explains sl_reels.c's strip counts, which thin out to the right --
 * the reel still spinning is the one least likely to oblige.
 */

#include <stdint.h>
#include <stdbool.h>
#include "sl_reels.h"

/* One symbol's cell, in pixels -- EXACTLY the tile height, with no gap.
 *
 * That is what makes the renderer simple and flicker-free: the tiles
 * abut, so five opaque blits cover a three-row reel completely and
 * every pixel is written exactly once. Nothing is ever cleared, which
 * is the whole problem sw/apps/roulette has.
 *
 * A gap between cells would need filling, and a fill is a clear.
 *
 * The strip is SL_STOPS cells tall and a reel's offset runs 0 to
 * SL_STRIP_PX -- which at 32 is a power of two, so the divides below
 * are shifts. */
#define SL_CELL      32
#define SL_STRIP_PX  (SL_STOPS * SL_CELL)

/* How long the first reel turns, and how much longer each one after it.
 * At 30 frames a second that is two seconds to the first stop and just
 * over three to the last. */
#define SL_BASE_FRAMES   60
#define SL_STAGGER       18

/* Full revolutions of the strip before a reel settles. */
#define SL_SPIN_TURNS    3

typedef struct {
    int      stop[SL_REELS];     /* where each reel will land */
    int32_t  pos[SL_REELS];      /* current offset, 0 .. SL_STRIP_PX-1 */
    int32_t  prev[SL_REELS];     /* the offset last frame, for scrolling */
    int32_t  start[SL_REELS];
    int32_t  travel[SL_REELS];
    int      frames[SL_REELS];   /* how long this reel turns for */
    int      frame;              /* frames elapsed since the spin began */
    bool     spinning;
} sl_spin_t;

void sl_spin_init(sl_spin_t *s);

/* Starts a spin that lands on `stops`. */
void sl_spin_begin(sl_spin_t *s, const int *stops);

/* Advances one frame. Returns true while any reel is still turning. */
bool sl_spin_step(sl_spin_t *s);

/* True once reel `r` has settled. */
bool sl_reel_stopped(const sl_spin_t *s, int r);

/* How far reel `r` moved this frame, in pixels -- what a scrolling
 * renderer needs. Always >= 0: reels only ever turn one way. */
int sl_reel_dy(const sl_spin_t *s, int r);

/* The symbol showing in `row` of reel `r` at the current offset, and
 * how far that row's cell is scrolled out of alignment. A stopped reel
 * has an offset of 0. */
uint8_t sl_reel_symbol(const sl_spin_t *s, int r, int row);
int sl_reel_subpixel(const sl_spin_t *s, int r);

#endif
