#ifndef ZGAME_H
#define ZGAME_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Full-screen game runtime: page layout, double buffering, and a
 * scrolling camera over the framebuffer.
 *
 * This is a thin layer over sw/common/zsoc.h's z_game_* helpers, and
 * it exists because those helpers are deliberately primitive -- they
 * are the register interface and nothing more. Every full-screen game
 * then has to invent the same three things on top: a decision about
 * how to carve the framebuffer into buffers, a flip that is actually
 * tear-free, and a world-to-framebuffer coordinate mapping. Getting
 * any of those subtly wrong produces symptoms (a tear, a one-frame
 * flicker, a sprite drawn into the buffer nobody is looking at) that
 * are much easier to avoid than to debug.
 *
 * -- what the hardware gives us, restated --
 *
 * There is ONE 640x480x1bpp framebuffer. Game mode points a 320x240
 * camera at part of it and doubles what it sees. The camera origin is
 * an arbitrary (x,y) adopted at a frame boundary. That is all.
 *
 * There is no second buffer, no page register and no pixel format
 * change. "Double buffering" here means nothing more than pointing
 * the camera at a different part of the one surface -- which is why a
 * flip costs one register write and no copying whatsoever.
 *
 * -- the page layout, and why it has two orientations --
 *
 * The framebuffer is cut into TWO half-pages, and which way it is cut
 * decides which axis can scroll. Both work identically; pick one at
 * z_game_begin().
 *
 *   Z_GAME_SCROLL_H -- two 640x240 pages, STACKED:
 *
 *       rows   0..239   page 0
 *       rows 240..479   page 1
 *
 *   Z_GAME_SCROLL_V -- two 320x480 pages, SIDE BY SIDE:
 *
 *       cols   0..319   page 0
 *       cols 320..639   page 1
 *
 * NOT the four 320x240 quadrants that also tile the framebuffer.
 * Quadrants would give four buffers of exactly viewport size; halves
 * give two buffers each with a viewport's worth of room to spare
 * along one axis. For a scrolling game that spare room is the entire
 * point, and here is why it is free.
 *
 * With wrap enabled the camera wraps at column 639 AND at row 479 --
 * the hardware wraps both axes together, there is no per-axis wrap
 * bit. What makes each layout work is that only ONE of those wraps
 * can ever fire:
 *
 *   Horizontal pages: origin y is 0 or 240, and 240 + 239 = 479, so
 *   the camera's bottom edge lands exactly on the last row and never
 *   crosses the vertical wrap. The x wrap is live, giving a
 *   horizontal torus 640 columns around.
 *
 *   Vertical pages: origin x is 0 or 320, and 320 + 319 = 639, so the
 *   camera's right edge lands exactly on the last column and never
 *   crosses the horizontal wrap. The y wrap is live, giving a
 *   vertical torus 480 rows around.
 *
 * So one mode bit gives a torus along the scrolling axis and a fixed
 * position along the other, with no special casing and no per-axis
 * hardware. The symmetry is not a coincidence -- 640/320 and 480/240
 * are both exactly 2 -- but it is worth stating, because it is the
 * reason a vertical scroller needs no RTL change at all.
 *
 * A world longer than the page is then drawn as a sliding window: as
 * the camera advances, draw only the newly exposed strip at
 * (camera + viewport) mod page_length. At walking pace that is a few
 * columns (or rows) per frame, instead of redrawing 320x240 of tiles.
 *
 * NOTE the vertical torus is 480 long and the horizontal one is 640,
 * so a vertical scroller redraws its leading edge slightly more
 * often for the same world size. It also has 240 rows of slack rather
 * than 320 columns. Neither difference matters at any plausible
 * scroll speed; it is mentioned because the two are otherwise so
 * symmetric that the asymmetry is surprising when you hit it.
 *
 * A game that wants four buffers of exactly viewport size (deep
 * frame-ahead rendering, say) should use the quadrants and ignore
 * this file's page helpers; nothing here is mandatory and the
 * hardware has no opinion.
 *
 * -- what this does NOT do --
 *
 * No sprites, no tiles, no collision, no entities. Those belong to a
 * game, not to a runtime, and every game wants them differently. What
 * is here is only the part that is genuinely about THIS hardware and
 * that is easy to get wrong.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zsoc.h"

#define Z_GAME_PAGES 2

/* Which axis scrolls. See the header comment for the two layouts and
 * why each one keeps the other axis away from the wrap boundary. */
typedef enum {
	Z_GAME_SCROLL_H = 0,   /* two 640x240 pages, stacked */
	Z_GAME_SCROLL_V = 1    /* two 320x480 pages, side by side */
} z_game_scroll_t;

/* Page dimensions per orientation. The scrolling axis is the long one
 * in both cases; that is what "has room to spare" means. */
#define Z_GAME_PAGE_W(o) ((o) == Z_GAME_SCROLL_H ? 640 : 320)
#define Z_GAME_PAGE_H(o) ((o) == Z_GAME_SCROLL_H ? 240 : 480)

/* Length of the torus along the scrolling axis -- 640 columns
 * horizontally, 480 rows vertically. This is the modulus the world
 * folds into. */
#define Z_GAME_PAGE_SPAN(o) ((o) == Z_GAME_SCROLL_H ? 640 : 480)

/* Viewport extent along the scrolling axis. */
#define Z_GAME_VIEW_SPAN(o) \
	((o) == Z_GAME_SCROLL_H ? Z_GAME_VIEW_W : Z_GAME_VIEW_H)

/* Framebuffer origin of page `p`. Only one of these varies per page;
 * the other is always 0, which is exactly what keeps the non-scrolling
 * axis clear of the wrap boundary. */
#define Z_GAME_PAGE_X(o, p) ((o) == Z_GAME_SCROLL_H ? 0 : (p) * 320)
#define Z_GAME_PAGE_Y(o, p) ((o) == Z_GAME_SCROLL_H ? (p) * 240 : 0)

typedef struct {

	z_game_scroll_t orient;

	/* which page is being DRAWN into. The other one is on screen. */
	int back;

	/* Camera position along the SCROLLING axis, in world pixels. May
	 * be far larger than the page and grows without bound as the
	 * player advances. This is the number game logic thinks in.
	 *
	 * One field, not an x and a y, because only one axis can scroll
	 * -- see the header. A game that thinks in (x,y) should keep its
	 * own pair and hand the scrolling one to z_game_camera_to(); the
	 * other is fixed by the page layout and there is nothing for this
	 * struct to remember about it. */
	int32_t cam;

	/* World position of the low edge of the strip currently drawn
	 * into each page, and one past its high edge. Per page, because
	 * the two are drawn on alternate frames and therefore lag each
	 * other by one frame of scroll. z_game_scroll_span() works out
	 * what is new from these. */
	int32_t drawn_from[Z_GAME_PAGES];
	int32_t drawn_to[Z_GAME_PAGES];

	/* frame counter at the last flip -- see z_game_flip() */
	uint32_t last_frame;

	bool wrap;

} z_game_t;

/* Fold a world position on the scrolling axis into its page position.
 *
 * This is the toroidal fold, and it is the single most important
 * function here: the world is unbounded, the page is 640 (or 480)
 * long, and this is what makes the second fit the first. World column
 * 640 is page column 0, world column 1281 is page column 1, and so
 * on.
 *
 * Handles negative world coordinates, which happen the moment a game
 * lets the player move back past the origin -- C's % truncates toward
 * zero, so -1 % 640 is -1 rather than 639, and a raw modulo here would
 * put the camera off the end of the framebuffer. */
static inline int z_game_fold(z_game_scroll_t o, int32_t world) {
	int32_t span = Z_GAME_PAGE_SPAN(o);
	int32_t m = world % span;
	if (m < 0) m += span;
	return (int)m;
}

/* Initialise, and enter game mode.
 *
 * `wrap` should be true for a scrolling game (see the header comment)
 * and false for anything that fits in one screen, where wrapping would
 * only turn an off-by-one into a sprite appearing on the wrong edge
 * rather than being harmlessly clipped.
 *
 * Returns false, having changed nothing, on a bitstream without game
 * mode. A caller should check and either fall back to a windowed
 * 640x480 presentation or say plainly that this needs a reflash --
 * failing silently and then rendering into a camera that does not
 * exist is much harder to diagnose. */
bool z_game_begin(z_game_t *g, z_game_scroll_t orient, bool wrap);

/* Leave game mode, returning the display to the 640x480 desktop.
 *
 * Does NOT clear the framebuffer: the window manager repaints on its
 * own, and clearing here would race with that repaint to produce a
 * flash of black. */
void z_game_end(z_game_t *g);

/* Frame the camera on the back page and wait for it to be shown.
 *
 * This is the flip, and the ORDER inside it is the whole point:
 *
 *   1. point the camera at the back page (one register write; the
 *      hardware adopts it at the next frame boundary)
 *   2. wait for that boundary
 *
 * After this returns, the page just drawn is on screen and the other
 * one is free. There is no copy and no tear -- the camera cannot move
 * mid-frame, so a frame is always shown whole.
 *
 * Returns the number of display frames that actually elapsed, which
 * is 1 when the game is keeping up and more when it is not. Use it to
 * scale movement so a game that drops to 30fps moves at the same
 * speed rather than in slow motion. */
uint32_t z_game_flip(z_game_t *g);

/* Framebuffer origin of the back page -- add these to any page-relative
 * coordinate before drawing. Every draw call in this system takes
 * ABSOLUTE framebuffer coordinates (see zgfx.h), so this is how a game
 * addresses the buffer nobody is currently looking at.
 *
 * One of the two is always 0, depending on orientation. Both are
 * provided anyway so a game can add both unconditionally and not care
 * which layout it is using. */
static inline int z_game_back_x(const z_game_t *g) {
	return Z_GAME_PAGE_X(g->orient, g->back);
}

static inline int z_game_back_y(const z_game_t *g) {
	return Z_GAME_PAGE_Y(g->orient, g->back);
}

static inline int z_game_front_x(const z_game_t *g) {
	return Z_GAME_PAGE_X(g->orient, g->back ^ 1);
}

static inline int z_game_front_y(const z_game_t *g) {
	return Z_GAME_PAGE_Y(g->orient, g->back ^ 1);
}

/* Move the camera along the scrolling axis, clamped to `world_len` so
 * it cannot scroll past the end of the level.
 *
 * This clamps in WORLD space, which is what a level designer means,
 * and is entirely separate from the hardware's own clamp in
 * framebuffer space (which is disabled whenever wrap is on). */
void z_game_camera_to(z_game_t *g, int32_t world_pos, int32_t world_len);

/* What needs drawing into the back page this frame.
 *
 * Returns the half-open world-x span [*from, *to) of columns that are
 * about to be visible but are not currently drawn in the back page.
 * Returns false if nothing needs drawing, which happens whenever the
 * camera has not moved since this page was last drawn.
 *
 * The span is in WORLD coordinates and is at most one viewport wide,
 * so a caller can always just draw all of it.
 *
 * The caller is expected to call z_game_mark_drawn() afterwards. They
 * are separate so a caller that draws in tile-sized chunks can widen
 * the span to tile boundaries first and record what it really drew. */
bool z_game_scroll_span(z_game_t *g, int32_t *from, int32_t *to);

/* Record that world span [from, to) is now drawn in the back page. */
void z_game_mark_drawn(z_game_t *g, int32_t from, int32_t to);

/* Forget what is drawn in both pages, so the next two frames redraw
 * everything. Call after anything that invalidates the background --
 * a level change, a palette-equivalent change, or returning from the
 * desktop. */
void z_game_invalidate(z_game_t *g);

#endif
