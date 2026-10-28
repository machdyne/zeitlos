#ifndef DRAWSTAMP_H
#define DRAWSTAMP_H

/*
 * draw -- what the tools leave on the page, a word at a time
 * (docs/draw_app.md).
 *
 * The page is DRAW_PAGE_H rows of DRAW_PAGE_WPL 32-bit words, pixel x
 * at bit (x & 31) of word (x >> 5) -- least significant bit leftmost,
 * the framebuffer's order (see CANVAS_W in draw.c). Everything here
 * clips to the page: a pixel off it is dropped, exactly as a
 * per-pixel set that checks its bounds would drop it.
 *
 * These were per pixel, inside draw.c: 64 bit tests a brush point and
 * 144 sets an eraser point, and a line of them a point per pixel along
 * it, so a quick eraser drag across the page took tens of ms in one
 * mouse sample (docs/responsive.md, "Still to do"). A row of a stamp
 * covers at most two words, so a brush point is now at most 16 word
 * writes and an eraser point 24. The pixels are the same --
 * tests/test_draw.c compares both ways on random strokes.
 *
 * Kept apart from draw.c, which needs the window system, so the build
 * machine can test it.
 */

#include <stdint.h>

#define DRAW_PAGE_W		640
#define DRAW_PAGE_H		480
#define DRAW_PAGE_WPL	(DRAW_PAGE_W / 32)
#define DRAW_PAGE_WORDS	(DRAW_PAGE_WPL * DRAW_PAGE_H)

#define DRAW_BRUSH_SIZE		8		// a rounded 8x8 blob, centred on (x, y)
#define DRAW_ERASER_SIZE	12		// a 12x12 square, likewise

// MSB-first rows: bit 7 is the leftmost pixel
extern const uint8_t draw_brush_mask[DRAW_BRUSH_SIZE];

// The brush at (x, y) in `pat` (an 8x8 pattern, MSB-first rows,
// anchored to the page's 8x8 grid -- see pat_bit() in draw.c).
void draw_stamp_brush(uint32_t *page, int x, int y, const uint8_t *pat);

// The eraser at (x, y): white.
void draw_stamp_eraser(uint32_t *page, int x, int y);

// The rect at (x, y), w x h, in `pat`.
void draw_fill_rect(uint32_t *page, int x, int y, int w, int h,
	const uint8_t *pat);

// A one-pixel outline, corners in either order; color 1 black, 0 white.
void draw_rect(uint32_t *page, int x0, int y0, int x1, int y1, int color);

#endif
