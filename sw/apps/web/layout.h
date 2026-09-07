// Forces the software image blit. See layout.c's blit_rows().
#ifndef WEB_IMG_SW
#define WEB_IMG_SW 0
#endif

#ifndef LAYOUT_H
#define LAYOUT_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Turning html_line_t blocks into pixels: wrapping, indentation,
 * inline styles, and where the links ended up.
 *
 * -- host-buildable, and that is the point --
 *
 * This file includes zgfx.h and zfont.h for the drawing primitives
 * and zwin.h for the content rect, and nothing else from Zeitlos. It
 * does not open files, send messages, or touch the network. So
 * tests/render.c can link it against sw/common/tests/zrender.h,
 * draw a real page on the build machine, and write out a PBM to
 * LOOK AT.
 *
 * zrender.h exists because sw/apps/logic shipped its panel wrong
 * three times with an arithmetic test passing each time, and the
 * third bug -- absolute versus content-relative coordinates -- was
 * found in one look. A browser has more layout surface than any panel
 * in this tree, so that step is not optional here.
 *
 * -- the model --
 *
 * A block is drawn as one or more DISPLAY LINES. Display lines are
 * derived from the block text plus the window width, and nothing
 * else, so they are recomputed from scratch every draw rather than
 * cached. That sounds wasteful and is not: the input is one block,
 * the work is a scan of at most HTML_LINE_MAX bytes, and the
 * alternative -- a cache keyed on width -- is a cache to invalidate
 * every time the window is resized.
 *
 * A POSITION in a document is therefore (block, display line within
 * block), the same two-part shape sw/apps/read uses and for the same
 * reason: scrolling by whole blocks jumps a paragraph at a time,
 * which is unusable, while the sub-line offset makes scrolling smooth
 * without making the index depend on the window width.
 *
 * -- what a 1bpp display can carry --
 *
 * Two inline styles, because that is how many are distinguishable
 * without a second font weight: inverse for code, underline for
 * links. Same conclusion sw/apps/read reached (md.h), reached
 * independently here and worth stating rather than inheriting.
 *
 * Headings get a larger font (6x12 rather than 5x8) and h1/h2 get a
 * rule underneath. Everything below h2 is the same size as body text
 * with the rule omitted -- there is no third size available, and
 * pretending otherwise by adding blank lines just wastes a screen
 * that has 50 of them.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zgfx.h"
#include "../../common/zfont.h"

#include "html.h"

// Most display lines one block can occupy. A block is at most
// HTML_LINE_MAX characters and the narrowest useful window is about
// 30 columns, so 96 covers the worst case with room; a block that
// somehow needs more is clipped rather than overflowing the array.
#define LAYOUT_MAX_LINES  96

// Link rectangles reported per drawn screen. One per display line of
// a link, not one per link -- a link that wraps is two rectangles and
// both must be clickable.
#define LAYOUT_MAX_HITS   64

// A decoded image to draw in place of a placeholder box, or a caption
// to show inside it instead of the default one.
//
// The browser fills this in per block at draw time. layout.c does no
// decoding and holds no image state: it is handed either a bitmap or
// nothing, which keeps every question about WHICH image is loaded,
// and how much memory that costs, in the app where it belongs.
typedef struct {
	const uint32_t	*bits;		// framebuffer packing, or NULL
	int				wpl;
	int				w, h;
	const char		*caption;	// overrides the block's own, or NULL
} layout_img_t;

// Asked for each HTML_IMAGE block as it is drawn. May be NULL, in
// which case every image draws as an empty box.
typedef void (*layout_img_fn)(uint32_t block, layout_img_t *out);

typedef struct {

	// Pixels available for content, i.e. the window's content rect
	// less the margins the caller wants. Wrapping is computed against
	// this and nothing else.
	int		width;

	// Left origin in SCREEN coordinates.
	//
	// Screen, not content-relative, because that is what z_fb_hw_box()
	// and z_fb_hw_line() take while everything else an app draws with
	// is content-relative -- the exact mismatch zrender.h's header
	// records as the bug behind sw/apps/logic's first two layout
	// failures. Taking screen coordinates here, once, at the top,
	// means the mismatch is resolved in one place instead of at every
	// call.
	int		x;

	const z_font_t	*body;
	const z_font_t	*head;

	// Draw link underlines. Off while a page is still loading, so the
	// half-parsed document does not look interactive before it is.
	bool	links_live;

	// The image that is currently loaded, and the size it actually
	// DRAWS at.
	//
	// A placeholder is sized from the markup's width/height, but the
	// decoders scale by powers of two only (zimg.h), so a 250x224
	// picture in a 250-wide box comes back 125x112 and leaves half
	// the box empty -- a hundred-odd pixels of nothing between the
	// image and the next paragraph.
	//
	// Matched on the src rather than a block number so that
	// layout_count() and layout_draw() agree without either of them
	// needing to know which block they are looking at. Only one image
	// is loaded at a time, so one src is enough.
	const char		*img_src;
	int				img_dw, img_dh;

	// Supplies decoded images and per-block captions. See above.
	layout_img_fn	img;

} layout_cfg_t;

typedef struct {
	int16_t		x, y, w, h;		// screen coordinates
	uint8_t		link;			// index into the block's links[]
	uint16_t	block;			// caller's own block number
} layout_hit_t;

// Vertical space to leave ABOVE this block, in pixels.
//
// Zero for a block flagged `tight` (html.h) -- that is the <br> case,
// where a gap would turn a line break into a paragraph break.
int layout_gap_before(const html_line_t *l, const layout_cfg_t *cfg);

// Height of one display line of this block, in pixels. Constant
// within a block; headings are taller than body text.
int layout_line_height(const html_line_t *l, const layout_cfg_t *cfg);

// How many display lines this block occupies at cfg->width.
//
// Always at least 1, including for HTML_BLANK and HTML_RULE, so that
// a position is never inside a block with no lines to be at.
// The pixel size of an HTML_IMAGE placeholder box.
//
// Exposed so the browser can decode an image to exactly the box it
// will occupy: the decoders scale at decode time and there is no
// intermediate greyscale to re-scale from (see zimg.h), so the size
// has to be known before decoding rather than after.
void layout_image_box(const html_line_t *l, const layout_cfg_t *cfg,
	int *w, int *h);

int layout_count(const html_line_t *l, const layout_cfg_t *cfg);

// Draws display lines [from, from+max) of `l`, with the first of them
// at screen row `y`.
//
// Returns the number of lines actually drawn, which is less than
// `max` when the block runs out first. `clip` is the window's content
// rect, passed through to every primitive.
//
// Link rectangles are appended to `hits` (up to `maxhits`), tagged
// with `block` so the caller can turn a click back into a URL.
// Pass hits == NULL when hit testing is not wanted.
int layout_draw(const html_line_t *l, const layout_cfg_t *cfg,
	int y, int from, int max, const z_clip_t *clip,
	uint16_t block, layout_hit_t *hits, int *nhits, int maxhits);

// The byte range of display line `n` within the block text. Used by
// find-in-page and by the caller's own "which word is under the
// cursor" logic; drawing does not need it.
//
// Returns false if `n` is past the end of the block.
bool layout_line_range(const html_line_t *l, const layout_cfg_t *cfg,
	int n, uint16_t *start, uint16_t *end);

#endif
