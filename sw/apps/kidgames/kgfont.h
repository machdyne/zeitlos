#ifndef KGFONT_H
#define KGFONT_H

/*
 * kidgames -- big block letters.
 *
 * The original renders a 5x7 dot-matrix font as coloured ncurses cells
 * (src/bigfont.c). The bitmap data here is that same font, unchanged:
 * 5 columns is wide enough to give M, N, W, K, X, Y and R real
 * diagonal strokes, which a 3-column font collapses into look-alike
 * blocks. That reasoning is the original author's and it holds just as
 * well in pixels.
 *
 * -- WHY THIS IS NOT A z_font_t --
 *
 * It would be the obvious thing, and it cannot work. Hardware glyph
 * memory is written by ONE process board-wide -- `wm`, at its own
 * startup, and nobody else (docs/window_manager.md, "Fonts in glyph
 * memory"). An app that loads its own font corrupts every other
 * process's text. There is also no room: the table is full at
 * z_font_5x8 + z_font_6x12 + the window icons, and the blitter's glyph
 * mode tops out at 8 pixels wide regardless, which a 10x-scaled letter
 * is not.
 *
 * So big letters are drawn as RECTANGLES, which is what they are.
 *
 * -- HOW IT USES THE HARDWARE --
 *
 * Naively that is one fill per lit pixel: up to 35 blitter operations
 * per letter, for a word of seven, sixty times a second if anything
 * animates.
 *
 * kg_big_putc() instead emits one fill per RUN OF LIT CELLS, and
 * merges vertically adjacent rows whose bit patterns are identical
 * before it does. 'L' is two fills (the stem, the foot) rather than
 * thirteen; 'A' is seven rather than nineteen. Measured over the whole
 * font that is 6.4 fills per glyph against 19.4 lit cells -- a third
 * of the blitter traffic, from an transformation that is twenty lines
 * long and entirely in software.
 *
 * tests/test_layout.c asserts the run decomposition covers exactly the
 * lit cells and nothing else, because a run-splitter that is subtly
 * wrong draws letters that are subtly wrong, and nobody reads a 'B' as
 * a misplaced fill.
 */

#include "kg.h"

#define KG_BIG_ROWS     7
#define KG_BIG_COLS     5

/* How a glyph is painted. All four are distinguishable WITHOUT dither
 * support, per kg.h's rule -- SOLID and HINT differ in fill, but BLANK
 * and GHOST differ in shape as well. */
typedef enum {
	KG_BIG_SOLID = 0,	/* the word itself */
	KG_BIG_HINT,		/* dithered: the answer, revealed */
	KG_BIG_BLANK,		/* a solid filled cell block: "put a letter here" */
	KG_BIG_GHOST		/* an empty outlined box: "not guessed yet" */
} kg_big_style_t;

/* The 7 rows of `c`, low KG_BIG_COLS bits each, MSB = leftmost
 * column. NULL for space and anything unsupported. Pure; no drawing.
 * Case-insensitive; only A-Z and 0-9 exist. */
const uint8_t *kg_glyph(char c);

/* Advance between the left edges of two adjacent glyphs, and the total
 * width of `s`, at `scale` pixels per bitmap cell. The gap between
 * letters is one whole cell, which at every scale keeps 'II' from
 * reading as a single wide glyph. */
int kg_big_advance(int scale);
int kg_big_w(const char *s, int scale);
int kg_big_h(int scale);

/*
 * The largest scale at which `s` fits inside maxw x maxh, clamped to
 * [KG_BIG_SCALE_MIN, KG_BIG_SCALE_MAX].
 *
 * Both bounds matter and the height one is not decoration: a 3-letter
 * word constrained only by width comes out at scale 18, which is 126
 * pixels tall and taller than the play area once a prompt and a score
 * line are also on screen. The original had only the width bound
 * because a terminal row is a fixed height; here it is not.
 */
#define KG_BIG_SCALE_MIN 2
#define KG_BIG_SCALE_MAX 14

int kg_big_best_scale(const char *s, int maxw, int maxh);

/* Draw one glyph with its top-left at (x,y), content-relative.
 * Returns kg_big_advance(scale) so callers can walk a string. */
int kg_big_putc(int x, int y, char c, int scale, kg_big_style_t style);

/* Draw a string left to right. Returns the total width used. */
int kg_big_puts(int x, int y, const char *s, int scale, kg_big_style_t style);

/*
 * Like kg_big_puts(), but the character at `blank_idx` (0-based, -1
 * for none) is drawn in `blank_style` instead of its own letter.
 *
 * This is Missing Letter's whole prompt and Word Guess's whole board,
 * and it is why a blank is a drawn SHAPE rather than a gap: a gap
 * reads as wider letter spacing, and a kid counts the wrong number of
 * letters in the word.
 */
int kg_big_puts_blank(int x, int y, const char *s, int scale,
	kg_big_style_t style, int blank_idx, kg_big_style_t blank_style);

/* Centred in the full playfield width. Returns the x used, so a caller
 * can line something up underneath. */
int kg_big_puts_centered(int y, const char *s, int scale,
	kg_big_style_t style);

#endif
