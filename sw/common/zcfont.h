#ifndef ZCFONT_H
#define ZCFONT_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zcfont -- text for zcanvas: public-domain Hershey stroke fonts at
 * any size, misc-fixed bitmap fonts, and the compiled-in 5x8/6x12;
 * measuring, wrapping and drawing UTF-8. docs/fonts.md.
 *
 * Stroke fonts are drawn as strokes: a pen of a chosen width swept
 * along each glyph's lines, through the same rasterizer and transform
 * as everything else on a page. So a glyph is as sharp at 600 dpi as at
 * 100%, and what is on paper is what was on the screen.
 *
 * Bitmap fonts keep their pixels: drawn 1:1 at their own size, and
 * enlarged pixel for pixel otherwise (crisp on screen, blocky on
 * paper; docs/fonts.md says which to use when).
 *
 * The font data is the file image (.zhf or .zfn) as loaded -- nothing
 * is copied; the caller keeps it alive while the font is in use.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zcanvas.h"
#include "zfont.h"

enum {
	ZC_FONT_NONE = 0,
	ZC_FONT_STROKE,		// ZHF1
	ZC_FONT_BITMAP,		// ZFN1
	ZC_FONT_BUILTIN,	// a z_font_t compiled into the app (Latin-9 bytes)
};

#define ZC_FONT_ITALIC 1
#define ZC_FONT_BOLD   2

typedef struct {

	uint8_t			kind;
	uint8_t			flags;		// ZC_FONT_ITALIC / ZC_FONT_BOLD, as designed

	// Metrics, in font units: stroke fonts in units of 1/upm em, bitmap
	// fonts in pixels (upm = the cell height).
	uint8_t			upm;
	uint8_t			cap, xh, asc, desc;
	uint8_t			space;		// advance of a space

	uint16_t		count;
	const uint8_t	*index;		// stroke: u16 cp, u16 offset; bitmap: u16 cp
	const uint8_t	*glyphs;
	uint32_t		len;

	uint8_t			w, h, stride;	// bitmap cell
	const z_font_t	*builtin;

	// ASCII looked up without a search: 1 + the glyph's offset (stroke)
	// or index (bitmap), 0 when the font has none
	uint16_t		ascii[95];

	char			name[17];

} zc_font_t;

// Recognises and checks a .zhf or .zfn file image. Returns false (and
// leaves kind NONE) for anything malformed: a font that would read out
// of its own data is refused here, not when drawn.
bool zc_font_open(zc_font_t *f, const void *file, uint32_t len,
	const char *name);

// One of the compiled-in fonts (z_font_5x8, z_font_6x12...).
// `ascent` is the baseline row from the top.
void zc_font_builtin(zc_font_t *f, const z_font_t *zf, int ascent,
	const char *name);

bool zc_font_has(const zc_font_t *f, uint32_t cp);

typedef struct {
	const zc_font_t	*font;
	zc_fix		size;		// em, user units (bitmap fonts: their cell
							// height draws 1:1)
	zc_fix		weight;		// pen, user units; 0 = by size (stroke fonts)
	zc_fix		track;		// added to each advance
	uint8_t		bold;		// heavier pen / 1-pixel smear, if the font is not bold
	uint8_t		italic;		// a fifth of a slant, if the font is not italic
	uint8_t		underline;
} zc_style_t;

// Vertical metrics in user units: above and below the baseline, and the
// distance from one baseline to the next.
void zc_text_metrics(const zc_style_t *st, zc_fix *ascent, zc_fix *descent,
	zc_fix *line);

zc_fix zc_text_advance(const zc_style_t *st, uint32_t cp);
zc_fix zc_text_width(const zc_style_t *st, const char *s, uint32_t len);

// Draws UTF-8 with its baseline's left end at user (x, y). Returns the
// x where the next character would go. Characters the font lacks use a
// look-alike if there is one (zutf8.h), else an empty box.
zc_fix zc_text_draw(zc_target_t *t, const zc_xf_t *xf, const zc_style_t *st,
	zc_fix x, zc_fix y, const char *s, uint32_t len, const zc_paint_t *p);

// -- wrapping ----------------------------------------------------------

typedef struct {
	uint32_t	start, len;		// bytes of the source; trailing spaces excluded
	zc_fix		width;
} zc_line_t;

// Greedy word wrap into lines no wider than maxw (maxw <= 0: only at
// newlines). A word wider than a line is broken between characters.
// Returns the number of lines, at most max_lines (the rest is dropped;
// *more, if given, says so).
int zc_text_wrap(const zc_style_t *st, const char *s, uint32_t len,
	zc_fix maxw, zc_line_t *lines, int max_lines, bool *more);

// -- glyph cache -------------------------------------------------------

// Stroke glyphs drawn onto a 1bpp target through an unrotated
// transform are rendered once and kept, at their device size, in this
// memory; afterwards a glyph is one blit. Without it every glyph is
// rasterized every time (correct, slower). The cache empties itself
// when full. Rendering is identical either way: the cache stores what
// the rasterizer drew.
void zc_text_cache(void *mem, uint32_t bytes);
void zc_text_cache_flush(void);
// hits, misses (for tuning and the tests)
void zc_text_cache_stats(uint32_t *hits, uint32_t *misses);

// -- for font writers --------------------------------------------------
//
// What a PDF or PostScript writer needs to make a font of its own from
// one of these (sw/apps/print): the glyphs as data, in font units.

// The codepoint drawn for cp: itself, a look-alike, or 0 (the box
// drawn for a character the font lacks).
uint32_t zc_font_resolve(const zc_font_t *f, uint32_t cp);

// A stroke glyph (resolved codepoint c): `npts` pairs of signed bytes,
// x then y, y down from the baseline; x == -128 lifts the pen. `adv`
// is its advance, `box` its ink box (x0 y0 x1 y1). NULL if none.
const int8_t *zc_font_strokes(const zc_font_t *f, uint32_t c, int *npts, int *adv,
	int8_t box[4]);

// A bitmap glyph's rows (f->h rows of f->stride bytes, most
// significant bit leftmost, 1 = ink), or NULL.
const uint8_t *zc_font_rows(const zc_font_t *f, uint32_t c);

// The pen a stroke font is drawn with in this style, user units (for a
// bitmap font, one font pixel).
zc_fix zc_text_pen(const zc_style_t *st);

#endif
