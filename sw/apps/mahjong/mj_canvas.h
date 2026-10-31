#ifndef MJ_CANVAS_H
#define MJ_CANVAS_H

/*
 * Zeitlos mahjong -- the canvas: a picture composed in memory, then put
 * on the screen in one go.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * No I/O and no Zeitlos headers beyond the font's struct, so the host
 * tests draw with exactly the code the board runs.
 *
 * -- why compose in memory at all --
 *
 * A tile is a face, two sides and a shadow, overlapping its neighbours
 * on two edges, with a picture on top. Drawn straight to the screen
 * that is a dozen fills and a few masked blits a tile, back to front,
 * and the stack flickers while it is built: there is no back buffer in
 * a window ("Single-buffered, on purpose", docs/arcade.md). Composed
 * here first, it is plain word arithmetic in RAM -- every overlap exact,
 * no ROPs needed -- and reaches the screen as one copy per plane
 * (ar_plane() on the screen face, z_fb_hw_blit_mem() on the desktop
 * face), which is never half drawn.
 *
 * -- two kinds of canvas --
 *
 * COLOUR: `planes` bitplanes (3, 4 or 6: 8, 16 or 64 colours), pixel
 * index c having bit k in plane k. What the screen face shows in its
 * colour window or full screen in colour.
 *
 * MONO: one plane, and an index c is drawn through its INK -- an 8x8
 * pattern (ar_ink_t's layout: a byte a row, the MOST significant bit
 * leftmost) -- anchored to the canvas. What the desktop face shows,
 * and the screen face in black and white. The same indices, so the
 * board's drawing code does not know which it is drawing.
 *
 * Planes are LEAST significant bit leftmost, one row `stride` words:
 * the layout z_fb_hw_blit_mem() and ar_plane() read directly.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zfont.h"

#define MJ_CV_MAXP  6

typedef struct {
	int       w, h;
	int       stride;           /* words a row */
	int       planes;           /* 0 = mono */
	uint32_t *plane[MJ_CV_MAXP];/* plane[0] is the mono plane when mono */

	/* Mono: index -> ink (16 of them), `ink_map` folding larger indices
	 * onto those 16 (NULL: index & 15). */
	const uint8_t (*ink)[8];
	const uint8_t *ink_map;
	uint32_t  inkw[16][8];      /* each ink row as a 32-bit word */

	int       cx0, cy0, cx1, cy1;  /* the clip; x1 and y1 exclusive */
} mj_canvas_t;

/* Words the canvas needs for w x h with `planes` planes (0 = mono). */
int  mj_cv_words(int w, int h, int planes);

/* Lays the canvas out in `mem` (mem_words long). False when it does
 * not fit. Clip is the whole canvas. Contents are left as they were. */
bool mj_cv_setup(mj_canvas_t *cv, uint32_t *mem, int mem_words,
		 int w, int h, int planes,
		 const uint8_t (*ink)[8], const uint8_t *ink_map);

void mj_cv_clip(mj_canvas_t *cv, int x, int y, int w, int h);
void mj_cv_noclip(mj_canvas_t *cv);

void mj_cv_fill(mj_canvas_t *cv, int x, int y, int w, int h, int c);

/* One row of up to 32 pixels: bits LSB leftmost, bit i set -> pixel
 * (x + i, y) becomes c. */
void mj_cv_row(mj_canvas_t *cv, int x, int y, uint32_t bits, int n, int c);

/* A 1bpp mask (one uint32 a row, LSB leftmost, w <= 32) in colour c. */
void mj_cv_bits(mj_canvas_t *cv, const uint32_t *rows, int w, int h,
		int x, int y, int c);

/* Darkens a rectangle. In colour: sets bit `bit` of every pixel's
 * index -- one plane, one OR a word -- which the palette is laid out to
 * make the darker variant of each colour (mj_board.h); a bit the canvas
 * does not have (or -1) does nothing. In mono: half its lit pixels go
 * dark (a checkerboard anchored to the canvas, so it reads as a shade,
 * not as a hole, and two shades side by side join). */
void mj_cv_shade(mj_canvas_t *cv, int x, int y, int w, int h, int bit);

/* A STAMP: a small picture with a mask, in the same number of planes
 * as the canvas -- `planes` (plane 0 alone for mono) of `sh` rows of
 * `sstride` words each, plane after plane, then the mask's rows. Put
 * at (x, y): where the mask has a bit the stamp replaces the canvas,
 * elsewhere the canvas stays. Clipped. A tile's whole body (face,
 * sides, outline) is one of these, which is two words a row a plane
 * instead of a fill for every one-pixel line in it. */
void mj_cv_stamp(mj_canvas_t *cv, const uint32_t *src, int sstride, int sw, int sh,
		 int x, int y);

/* One row of up to 32 pixels in several colours at once: pixel
 * (x + i, y) becomes c[k] where bits[k] has bit i (the masks do not
 * overlap). One write a plane, however many colours. */
void mj_cv_row_multi(mj_canvas_t *cv, int x, int y, const uint32_t *bits,
		     const uint8_t *c, int nc, int n);

/* Moves rows [y0, y1) of every plane by (dx, dy) pixels -- content at
 * (x, y) goes to (x + dx, y + dy) -- the whole width, within the band.
 * What moves in at the edges is left as it was, for the caller to
 * draw: a scrolled board redraws a strip, not the table. */
void mj_cv_scroll(mj_canvas_t *cv, int y0, int y1, int dx, int dy);

/* Text in colour c, transparent behind. Returns the width drawn. */
int  mj_cv_text(mj_canvas_t *cv, int x, int y, const char *s, int c,
		const z_font_t *f);
int  mj_cv_text_width(const char *s, const z_font_t *f);

/* Reading back, for the tests: the index at (x, y) (colour), or
 * whether the pixel is lit (mono). */
int  mj_cv_get(const mj_canvas_t *cv, int x, int y);

#endif
