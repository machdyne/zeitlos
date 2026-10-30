#ifndef DRAWFILL_H
#define DRAWFILL_H

/*
 * draw -- the paint bucket, a little at a time (docs/draw_app.md,
 * "Fill").
 *
 *   draw_fill_start(&f, page, orig, x, y, pat);
 *   while (draw_fill_step(&f, 64) == DRAW_FILL_MORE)
 *       ... read messages; Esc: draw_fill_stop(&f) ...
 *   the page is filled inside f.x0..f.x1, f.y0..f.y1
 *
 * The region is every pixel joined to (x, y) -- up, down, left, right --
 * that has its colour. Found first in one colour, then given the
 * pattern:
 *
 *   1. Runs of the region's colour are found a row at a time and set to
 *      the other colour. A pixel set is a pixel visited: what is left
 *      of the region's colour is what the fill has not reached. The
 *      runs still to look above and below are on a stack of spans,
 *      not of pixels, so a stack of DRAW_FILL_STACK covers any
 *      ordinary shape.
 *   2. If the stack ever runs out, nothing is lost: a span not pushed
 *      is found again by a sweep of the rows touched, which looks for a
 *      pixel of the region's colour beside one the fill has changed,
 *      and starts there. Sweeps repeat until one finds nothing.
 *   3. The pattern goes in where the page differs from `orig` -- the
 *      page as it was, which draw's undo keeps in `spare` anyway
 *      (drawundo.h) -- so a 50% pattern, half of whose pixels are the
 *      region's colour, is not mistaken for unfilled region.
 *
 * Nothing is allocated and no step runs long: draw_fill_step() does at
 * most `work` units (a span, or a row of a sweep or of the pattern),
 * and the caller reads its messages between steps. The fill this
 * replaces sat on a stack of pixels, ran out on anything not simple,
 * and swept the whole page in one go: wm timed out waiting for draw.
 *
 * draw_fill_stop() puts the rows it touched back from `orig`.
 *
 * Kept apart from draw.c so the build machine can test it
 * (tests/test_draw.c).
 */

#include <stdint.h>
#include <stdbool.h>

#include "drawstamp.h"		// DRAW_PAGE_*

#define DRAW_FILL_STACK		256

#define DRAW_FILL_MORE		0
#define DRAW_FILL_DONE		1

typedef struct {
	int16_t	x0, x1, y;
} draw_span_t;

typedef struct {
	uint32_t		*page;
	const uint32_t	*orig;
	const uint8_t	*pat;
	int				target;			// the region's colour
	int				phase;
	draw_span_t		st[DRAW_FILL_STACK];
	int				sp;
	bool			overflow;		// a span was dropped: sweep
	bool			found;			// this sweep started something
	int				row;			// the sweep's or the pattern's next row
	int				x0, y0, x1, y1;	// what has been touched
	uint32_t		sweeps;			// how many, for the tests
} draw_fill_t;

// False if (x, y) is off the page.
bool draw_fill_start(draw_fill_t *f, uint32_t *page, const uint32_t *orig,
	int x, int y, const uint8_t *pat);

int draw_fill_step(draw_fill_t *f, int work);

void draw_fill_stop(draw_fill_t *f);

#endif
