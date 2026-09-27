#ifndef BODYVIEW_H
#define BODYVIEW_H

/*
 * Zeitlos -- sw/apps/web
 *
 * The page body: drawing it, and scrolling it forward by moving the
 * pixels already on the glass instead of drawing everything again.
 * docs/web_app.md, "Scrolling".
 *
 * -- why a model --
 *
 * Lines here are not all one height (a heading is taller than text, an
 * image is several lines tall) and blocks have gaps between them, so
 * "scrolled by three lines" is not a fixed number of pixels. bv_draw()
 * therefore records, for every display line it draws, which line it is
 * (block, sub-line) and where it went, and where drawing stopped. To
 * scroll forward, bv_scroll_to() finds the new top line in that record:
 * its old y IS the distance to move. One blit moves everything from it
 * down to the top of the view, the strip that opens at the bottom is
 * cleared, and drawing continues from the first line that was not on
 * the glass. What is left is exactly what bv_draw() at the new top would
 * have drawn -- tests/test_bodyview.c compares the two pixel for pixel.
 *
 * Only FORWARD. A line's position after a backward scroll depends on
 * lines that were never drawn, so there is nothing recorded to move by;
 * scrolling back is a full draw, as it was.
 *
 * Kept apart from web.c, like toolbar.c, so that it can be tested
 * against a real layout and the render harness's emulated blitter.
 */

#include <stdint.h>
#include <stdbool.h>

#include "page.h"
#include "layout.h"
#include "../../common/zwin.h"

// Display lines the model holds. A 480-line body of 5x8 text is about
// 53; beyond this the model is incomplete and scrolling falls back to a
// full draw (bv_scroll_to() needs the new top line to be in it).
#define BV_VIS_MAX  160

typedef struct {

	// -- set by the caller (web.c's relayout()) --
	page_t				*pg;
	const layout_cfg_t	*cfg;
	z_win_t				*win;
	const z_clip_t		*crect;		// content rect, screen coordinates
	int					view_y;		// body top, content-relative
	int					view_h;		// body height
	int					body_w;		// width moved and cleared: the
									// content width less the scrollbar
	layout_hit_t		*hits;		// link rectangles, as layout_draw()
	int					*nhits;
	int					max_hits;

	// -- what is on the glass (bv_draw(), kept in step by bv_scroll_to()) --
	uint32_t	vblock[BV_VIS_MAX];		// each drawn display line: block,
	uint16_t	vsub[BV_VIS_MAX];		// its line within the block,
	int16_t		vy[BV_VIS_MAX];			// and y, content-relative
	int			nvis;

	// Where drawing stopped: the first line NOT on the glass, and the y
	// it would have started at (before any gap above it).
	bool		more;			// is there such a line
	uint32_t	nblock;
	uint16_t	nsub;
	int16_t		ny;
	int8_t		nkind;			// the kind of the last block drawn

	bool		valid;			// the glass matches this model
	int			drawn_view_h;	// geometry at the time, to refuse a
	int16_t		drawn_x0, drawn_y0;	// scroll after a move or resize

} bodyview_t;

// Draws the body with `top` at its top, recording the model.
void bv_draw(bodyview_t *v, page_pos_t top);

// The view's top has just moved FORWARD to `top` (page_advance()).
// Moves what is already on the glass and draws only what is new.
// Returns false, touching nothing, when that cannot be done exactly --
// no model, the window frozen or not wholly visible, the new top not
// on the glass, a move of a whole screen or more -- and the caller then
// draws the body with bv_draw().
bool bv_scroll_to(bodyview_t *v, page_pos_t top);

// Anything drawn over the body other than by the two calls above (a
// selection box, say) makes the model wrong; say so.
static inline void bv_invalidate(bodyview_t *v) { v->valid = false; }

#endif
