#ifndef ZFONT_H
#define ZFONT_H

#include <stdint.h>

/*
 * Bitmap fonts, generated from sw/data/font/ .mem sources by
 * sw/data/font/gen_font_data.py (see that script, and
 * sw/data/font/bdf_to_mem.py for importing a new font from BDF).
 * Each font covers ASCII 0x20-0x7F; codepoints missing from a given
 * source (like 0x7F in most of these) are just blank.
 *
 * Glyph data is `h` bytes per glyph, one per row, MSB-first, using
 * the top `w` bits of each byte (w <= 8).
 */

typedef struct {
	uint8_t		w, h;			// glyph dimensions in pixels
	uint8_t		first, last;	// inclusive codepoint range covered
	const uint8_t	*glyphs;	// (last-first+1) * h bytes, row-major per glyph
} z_font_t;

extern const z_font_t z_font_8x16;	// original font, sw/data/font/font8x16.mem
extern const z_font_t z_font_6x12;	// compact font, for dense text (e.g. a terminal)
extern const z_font_t z_font_5x7;	// smaller still -- see sw/apps/term's TERM_FONT_NAME
									// for how to pick this at build time
extern const z_font_t z_font_5x8;	// same width as z_font_5x7, one extra
									// row of height -- sw/data/font/font5x8.mem.
									// Adopted as the default for hardware-blitted
									// text (wm/term/hello_win) after real-hardware
									// testing showed the bottom pixel row of
									// z_font_5x7 glyphs getting cut off on screen;
									// the extra row works around that rather than
									// being a confirmed fix for its root cause,
									// which hasn't been separately diagnosed.

#endif
