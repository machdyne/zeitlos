/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Full-screen game runtime -- see sw/common/zgame.h for the design
 * and, in particular, for why the framebuffer is carved into two
 * 640x240 half-pages rather than four 320x240 quadrants.
 */

#include "zgame.h"

bool z_game_begin(z_game_t *g, bool wrap) {

	g->back = 1;
	g->cam_x = 0;
	g->cam_y = 0;
	g->wrap = wrap;
	g->last_frame = 0;

	z_game_invalidate(g);

	if (!z_game_set_enabled(true, wrap)) return false;

	/* Point the camera at page 0 -- the FRONT page, since back is 1.
	 * Doing this before the caller has drawn anything means the first
	 * frame shows whatever was in rows 0..239 already, which is the
	 * desktop. That is deliberate and is the least-bad option: the
	 * alternative, clearing it here, produces a flash of black on
	 * every entry into game mode, and a caller that cares can clear
	 * both pages itself before its first flip. */
	z_game_set_view(0, Z_GAME_PAGE_Y(0));

	g->last_frame = z_game_frame();

	return true;

}

void z_game_end(z_game_t *g) {
	(void)g;
	z_game_set_enabled(false, false);
}

void z_game_camera_to(z_game_t *g, int32_t world_x, int32_t world_w) {

	/* Clamp so the RIGHT edge of the viewport stops at the end of the
	 * world, not the left edge -- otherwise the last 320 columns of a
	 * level can never be looked at. */
	int32_t max_x = world_w - Z_GAME_VIEW_W;
	if (max_x < 0) max_x = 0;

	if (world_x < 0) world_x = 0;
	if (world_x > max_x) world_x = max_x;

	g->cam_x = world_x;

}

uint32_t z_game_flip(z_game_t *g) {

	uint32_t now, elapsed;

	/* The camera's framebuffer x is the world position folded into the
	 * page's 640-column torus; its y selects which half-page. */
	z_game_set_view((uint32_t)z_game_page_col(g->cam_x),
		(uint32_t)(Z_GAME_PAGE_Y(g->back) + g->cam_y));

	z_game_wait_frame();

	now = z_game_frame();

	/* Unsigned subtraction, so the counter's 16-bit wrap (every ~18
	 * minutes at 60Hz) is not a special case -- it comes out right on
	 * its own, which a `now > last` comparison would not. */
	elapsed = (now - g->last_frame) & 0xffffu;
	if (elapsed == 0) elapsed = 1;
	g->last_frame = now;

	g->back ^= 1;

	return elapsed;

}

bool z_game_scroll_span(z_game_t *g, int32_t *from, int32_t *to) {

	int b = g->back;
	int32_t want_from = g->cam_x;
	int32_t want_to = g->cam_x + Z_GAME_VIEW_W;

	/* Nothing recorded for this page -- everything is new. */
	if (g->drawn_from[b] >= g->drawn_to[b]) {
		*from = want_from;
		*to = want_to;
		return true;
	}

	/* Already covers what we need. */
	if (g->drawn_from[b] <= want_from && g->drawn_to[b] >= want_to)
		return false;

	/* A jump larger than the viewport -- no overlap at all with what
	 * is drawn, so there is no "leading edge" to speak of and the
	 * whole viewport is new. This is the level-restart / teleport
	 * case; it costs one full redraw and then scrolling is cheap
	 * again. */
	if (want_from >= g->drawn_to[b] || want_to <= g->drawn_from[b]) {
		*from = want_from;
		*to = want_to;
		return true;
	}

	/* The ordinary case: scrolling right, so the new columns are on
	 * the right. */
	if (want_to > g->drawn_to[b]) {
		*from = g->drawn_to[b];
		*to = want_to;
		return true;
	}

	/* Scrolling left. */
	*from = want_from;
	*to = g->drawn_from[b];
	return true;

}

void z_game_mark_drawn(z_game_t *g, int32_t from, int32_t to) {

	int b = g->back;

	if (g->drawn_from[b] >= g->drawn_to[b]) {
		g->drawn_from[b] = from;
		g->drawn_to[b] = to;
		return;
	}

	/* Merge only if the new span actually touches what is recorded --
	 * a disjoint span means the old record describes columns that have
	 * since been scrolled over and overwritten by the torus, so
	 * keeping it would claim coverage that is not there. */
	if (from > g->drawn_to[b] || to < g->drawn_from[b]) {
		g->drawn_from[b] = from;
		g->drawn_to[b] = to;
		return;
	}

	if (from < g->drawn_from[b]) g->drawn_from[b] = from;
	if (to > g->drawn_to[b]) g->drawn_to[b] = to;

	/* A page is a 640-wide torus: it cannot hold more than 640
	 * distinct world columns, and claiming otherwise would mean
	 * skipping a redraw of columns that have genuinely been
	 * overwritten by the wrap. Trim from the left, since the right is
	 * the leading edge and is what was just drawn. */
	if (g->drawn_to[b] - g->drawn_from[b] > Z_GAME_PAGE_W)
		g->drawn_from[b] = g->drawn_to[b] - Z_GAME_PAGE_W;

}

void z_game_invalidate(z_game_t *g) {
	for (int i = 0; i < Z_GAME_PAGES; i++) {
		g->drawn_from[i] = 0;
		g->drawn_to[i] = 0;
	}
}
