#ifndef ZBIGLINE_H
#define ZBIGLINE_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Big text: a line of characters wider than the glyph blitter's 8
 * pixels, drawn into a buffer in memory and copied to the screen in one
 * blit (docs/fonts.md, "Big text").
 *
 * The hardware glyph path (zgfx.h) draws at most 8 pixels wide, from
 * glyph memory. A font like misc-fixed 10x20 cannot go there. So a line
 * is put together in software -- a shift and an OR a glyph row -- in a
 * buffer laid out like the framebuffer, and z_fb_hw_blit_mem() copies
 * it out, background and all. A line is a few hundred microseconds to
 * a millisecond of CPU, against ~0.1 ms in glyph hardware; for big
 * print, where a screen holds a few hundred characters, that is fine.
 *
 * The copy writes every pixel of the line, so the row needs no clearing
 * first and does not flash. It is clipped to the window's visible
 * region by the blitter's scissor (z_win_select() puts the region in
 * force) and to `clip` by z_bigline_put() itself.
 *
 *   z_bigline_begin(w, h);                  // the line, all background
 *   x = z_bigline_text(x, s, n, &z_bigfont_10x20);
 *   z_bigline_invert(x0, x1);               // a selection
 *   z_win_select(&win);
 *   z_bigline_put(screen_x, screen_y, &clip);
 *
 * One buffer, in this module: a process draws one line at a time.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zgfx.h"

// A font compiled from a .zfn by tools/gen_bigfont.py. Glyph rows in
// framebuffer order: bit k of a row is pixel column k.
typedef struct {
	uint8_t			w, h;
	uint16_t		count;
	const uint16_t	*cps;		// codepoints, ascending
	const uint16_t	*rows;		// count * h
} z_bigfont_t;

extern const z_bigfont_t z_bigfont_10x20;

// The tallest line and the widest the buffer holds.
#define Z_BIGLINE_MAX_H		24
#define Z_BIGLINE_MAX_W		640

// Can a line be put on the screen here? False on a bitstream whose
// blitter has no memory-copy mode (zgfx.h); an app then offers no big
// size. Asked once, cached.
bool z_bigline_available(void);

// A new line, `w` x `h` pixels of background (clamped to the maximum).
void z_bigline_begin(int w, int h);

// Draws s[0..n), UTF-8, from pixel column `x` of the line; returns the
// column after it. A character the font has not got is drawn as its
// look-alike (zutf8.h), else as a box; a wide one (z_cp_width() 2) takes
// two cells; a control character, a tab included, is a space.
int z_bigline_text(int x, const char *s, int n, const z_bigfont_t *f);

// Swaps ink and background in columns [x0, x1), every row: a selection.
void z_bigline_invert(int x0, int x1);

// Copies the line to the screen with its top-left at (sx, sy), only
// where it falls inside `clip` (screen coordinates). False if the
// blitter cannot copy from memory.
bool z_bigline_put(int sx, int sy, const z_clip_t *clip);

#endif
