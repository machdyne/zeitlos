/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Full-screen game runtime -- see sw/common/zgame.h for the design
 * and, in particular, for why the framebuffer is carved into two
 * 640x240 half-pages rather than four 320x240 quadrants.
 */

#include "zgame.h"

bool z_game_begin(z_game_t *g, z_game_scroll_t orient, bool wrap) {

	g->orient = orient;
	g->back = 1;
	g->cam = 0;
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
	z_game_set_view((uint32_t)Z_GAME_PAGE_X(orient, 0),
		(uint32_t)Z_GAME_PAGE_Y(orient, 0));

	g->last_frame = z_game_frame();

	return true;

}

void z_game_end(z_game_t *g) {
	(void)g;
	z_game_set_enabled(false, false);
}

void z_game_camera_to(z_game_t *g, int32_t world_pos, int32_t world_len) {

	/* Clamp so the FAR edge of the viewport stops at the end of the
	 * world, not the near edge -- otherwise the last viewport's worth
	 * of a level could never be looked at. */
	int32_t max_pos = world_len - Z_GAME_VIEW_SPAN(g->orient);
	if (max_pos < 0) max_pos = 0;

	if (world_pos < 0) world_pos = 0;
	if (world_pos > max_pos) world_pos = max_pos;

	g->cam = world_pos;

}

uint32_t z_game_flip(z_game_t *g) {

	uint32_t now, elapsed;

	/* Along the scrolling axis the camera is the world position folded
	 * into the page's torus; along the other it is simply the page
	 * origin, which is what keeps that axis clear of the wrap
	 * boundary. */
	{
		int fold = z_game_fold(g->orient, g->cam);
		if (g->orient == Z_GAME_SCROLL_H)
			z_game_set_view((uint32_t)fold,
				(uint32_t)Z_GAME_PAGE_Y(g->orient, g->back));
		else
			z_game_set_view((uint32_t)Z_GAME_PAGE_X(g->orient, g->back),
				(uint32_t)fold);
	}

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
	int32_t want_from = g->cam;
	int32_t want_to = g->cam + Z_GAME_VIEW_SPAN(g->orient);

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

	/* The ordinary case: advancing, so the new strip is at the high
	 * end. */
	if (want_to > g->drawn_to[b]) {
		*from = g->drawn_to[b];
		*to = want_to;
		return true;
	}

	/* Retreating. */
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

	/* A page is a torus: it cannot hold more distinct world positions
	 * than its own span, and claiming otherwise would mean skipping a
	 * redraw of a strip the wrap has genuinely overwritten. Trim from
	 * the low end, since the high end is the leading edge and is what
	 * was just drawn. */
	{
		int32_t span = Z_GAME_PAGE_SPAN(g->orient);
		if (g->drawn_to[b] - g->drawn_from[b] > span)
			g->drawn_from[b] = g->drawn_to[b] - span;
	}

}

void z_game_invalidate(z_game_t *g) {
	for (int i = 0; i < Z_GAME_PAGES; i++) {
		g->drawn_from[i] = 0;
		g->drawn_to[i] = 0;
	}
}
