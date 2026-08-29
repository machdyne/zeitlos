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
 * -- the page layout this uses, and why --
 *
 * TWO 640x240 half-pages, stacked:
 *
 *     rows   0..239   page 0
 *     rows 240..479   page 1
 *
 * NOT the four 320x240 quadrants that also tile the framebuffer.
 * Quadrants would give four buffers of exactly viewport size; halves
 * give two buffers each with 320 pixels of horizontal room to spare.
 * For a side-scroller that spare room is the entire point, and here
 * is why it is free:
 *
 * With wrap enabled the camera wraps at column 639, so a half-page is
 * a horizontal TORUS -- scroll right past the end and you arrive back
 * at the start. A world longer than 640 pixels is then drawn as a
 * sliding window: as the camera advances, draw only the newly exposed
 * columns at (camera + 320) mod 640. At walking pace that is two to
 * four columns per frame, instead of redrawing 320x240 of tiles.
 *
 * And crucially the wrap costs nothing vertically. A half-page has
 * its origin at y=0 or y=240, and 240 + 239 = 479 -- so the camera's
 * bottom edge lands exactly on the last row and never crosses the
 * wrap boundary. Toroidal horizontal scrolling and a fixed vertical
 * position, from one mode bit, with no special casing.
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

/* One page is the full framebuffer width by half its height. See the
 * header comment for why halves rather than quadrants. */
#define Z_GAME_PAGE_W 640
#define Z_GAME_PAGE_H 240
#define Z_GAME_PAGES  2

/* Framebuffer row at which page `p` starts. */
#define Z_GAME_PAGE_Y(p) ((p) * Z_GAME_PAGE_H)

typedef struct {

	/* which page is being DRAWN into. The other one is on screen. */
	int back;

	/* camera position in WORLD pixels -- may be far larger than 640,
	 * and grows without bound as the player walks right. This is the
	 * number game logic thinks in. */
	int32_t cam_x;

	/* vertical camera offset WITHIN the page, 0..0 in practice: a
	 * page is exactly viewport height, so there is nowhere to scroll
	 * to. Kept as a field rather than assumed zero so a game using a
	 * different page layout can still use the flip machinery. */
	int32_t cam_y;

	/* world x of the leftmost column currently drawn into each page.
	 * The sliding window's left edge, per page, because the two pages
	 * are drawn on alternate frames and therefore lag each other.
	 * z_game_scroll_span() uses this to work out what is new. */
	int32_t drawn_from[Z_GAME_PAGES];

	/* world x one past the rightmost column drawn into each page. */
	int32_t drawn_to[Z_GAME_PAGES];

	/* frame counter at the last flip, for z_game_frames_elapsed() */
	uint32_t last_frame;

	bool wrap;

} z_game_t;

/* Map a world x to its column within a page.
 *
 * This is the toroidal fold, and it is the single most important
 * function here: the world is unbounded, the page is 640 wide, and
 * this is what makes the second fit the first. World column 640 is
 * page column 0, world column 1281 is page column 1, and so on.
 *
 * Uses a masking modulo rather than `%` because 640 is not a power of
 * two and picorv32 has no divider unless `CPU_DIV is built -- and
 * even then a division in a per-column inner loop is worth avoiding.
 * A conditional subtract loop would be worse for large values, so
 * this reduces by 640 arithmetically. Handles negative world
 * coordinates, which happen the moment a game lets the player walk
 * left of the origin. */
static inline int z_game_page_col(int32_t world_x) {
	int32_t m = world_x % Z_GAME_PAGE_W;
	if (m < 0) m += Z_GAME_PAGE_W;
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
bool z_game_begin(z_game_t *g, bool wrap);

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

/* Framebuffer y of the top of the back page -- add this to any
 * page-relative y before drawing. Every draw call in this system takes
 * absolute framebuffer coordinates (see zgfx.h), so this is how a
 * game addresses the buffer nobody is looking at. */
static inline int z_game_back_y(const z_game_t *g) {
	return Z_GAME_PAGE_Y(g->back);
}

static inline int z_game_front_y(const z_game_t *g) {
	return Z_GAME_PAGE_Y(g->back ^ 1);
}

/* Move the camera to a world position, clamped to `world_w` so it
 * cannot scroll past the end of the level.
 *
 * Note this clamps in WORLD space, which is what a level designer
 * means, and is entirely separate from the hardware's own clamp in
 * framebuffer space (which is disabled when wrap is on). */
void z_game_camera_to(z_game_t *g, int32_t world_x, int32_t world_w);

/* What needs drawing into the back page this frame.
 *
 * Returns the half-open world-x span [*from, *to) of columns that are
 * about to be visible but are not currently drawn in the back page.
 * Returns false if nothing needs drawing, which happens whenever the
 * camera has not moved since this page was last drawn.
 *
 * The span is in WORLD coordinates and may be wider than the viewport
 * after a jump (a level restart, a teleport), in which case the caller
 * should just draw all of it -- it is at most 640 columns.
 *
 * The caller is expected to call z_game_mark_drawn() afterwards. They
 * are separate so a caller that draws in tile-sized chunks can widen
 * the span to tile boundaries first and record what it really drew. */
bool z_game_scroll_span(z_game_t *g, int32_t *from, int32_t *to);

/* Record that world columns [from, to) are now drawn in the back
 * page. */
void z_game_mark_drawn(z_game_t *g, int32_t from, int32_t to);

/* Forget what is drawn in both pages, so the next two frames redraw
 * everything. Call after anything that invalidates the background --
 * a level change, a palette-equivalent change, or returning from the
 * desktop. */
void z_game_invalidate(z_game_t *g);

#endif
