#ifndef ZFONT_H
#define ZFONT_H

#include <stdint.h>

/*
 * Bitmap fonts, generated from sw/data/font/ .mem sources by
 * sw/data/font/gen_font_data.py (see that script if you need to add
 * or regenerate one -- e.g. a future 6x6). Each font covers ASCII
 * 0x20-0x7F; codepoints missing from a given source (like 0x7F in
 * font6x12.mem) are just blank.
 *
 * Glyph data is `h` bytes per glyph, one per row, MSB-first, using
 * the top `w` bits of each byte (w <= 8).
 */

typedef struct {
	uint8_t		w, h;			// glyph dimensions in pixels
	uint8_t		first, last;	// inclusive codepoint range covered
	const uint8_t	*glyphs;	// (last-first+1) * h bytes, row-major per glyph
} z_font_t;

extern const z_font_t z_font_8x16;	// original font, sw/data/font/font16.mem
extern const z_font_t z_font_6x12;	// compact font, for dense text (e.g. a terminal)

#endif
