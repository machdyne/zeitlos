#ifndef ZFONT_H
#define ZFONT_H

#include <stdint.h>

/*
 * Bitmap fonts, generated from sw/data/font/ .mem sources by
 * sw/data/font/gen_font_data.py (see that script, and
 * sw/data/font/bdf_to_mem.py for importing a new font from BDF).
 *
 * A font is indexed by BYTE value. Every font covers ASCII 0x20-0x7F.
 * The two hardware fonts, z_font_5x8 and z_font_6x12, also cover the
 * upper half of ISO 8859-15 (Latin-9), 0xA0-0xFF -- so byte 0xE4 is
 * a-umlaut and 0xA4 is the euro sign. Bytes 0x80-0x9F (C1 controls)
 * have no glyphs and are not stored: that is the gap_lo/gap_hi range,
 * and it is what lets both fonts fit glyph memory (docs/
 * window_manager.md, "Fonts in glyph memory"). Use z_font_index()
 * rather than `c - first`, which is only right below the gap.
 *
 * 0x7F (DEL) is not a character; in the Latin-9 fonts it holds the
 * missing-glyph box (Z_GLYPH_MISSING), which is what z_fb_draw_cp()
 * shows for a character the font cannot draw. See
 * docs/text_encoding.md.
 *
 * Glyph data is `h` bytes per glyph, one per row, MSB-first, using
 * the top `w` bits of each byte (w <= 8).
 */

typedef struct {
	uint8_t		w, h;			// glyph dimensions in pixels
	uint8_t		first, last;	// inclusive byte range covered
	const uint8_t	*glyphs;	// z_font_glyph_count() * h bytes, row-major
	// Bytes in [gap_lo, gap_hi] have no glyph and take no space in
	// `glyphs`. gap_lo == 0 means no gap. Last in the struct so an
	// initialiser written before the gap existed still means "no gap".
	uint8_t		gap_lo, gap_hi;
} z_font_t;

// The glyph a character with no glyph of its own is drawn as.
#define Z_GLYPH_MISSING  0x7F

// Glyph number of byte c in font f, or -1 if the font has none.
static inline int z_font_index(const z_font_t *f, uint32_t c) {
	if (c < f->first || c > f->last) return -1;
	if (f->gap_lo && c >= f->gap_lo) {
		if (c <= f->gap_hi) return -1;
		return (int)(c - f->first) - (int)(f->gap_hi - f->gap_lo + 1);
	}
	return (int)(c - f->first);
}

// Number of glyphs stored.
static inline uint32_t z_font_glyph_count(const z_font_t *f) {
	uint32_t n = (uint32_t)(f->last - f->first + 1);
	if (f->gap_lo) n -= (uint32_t)(f->gap_hi - f->gap_lo + 1);
	return n;
}

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
									//
									// LOCALLY MODIFIED, not stock misc-fixed:
									// '.' was a three-pixel diamond straddling
									// the baseline, and a column of them (hex's
									// non-printable placeholder) read as content
									// rather than padding. It is now the same
									// 2x2 baseline dot ':' already used. See
									// sw/data/font/5x8.bdf's own COMMENT block,
									// which is where the change lives -- the
									// .mem and zfont_data.c are both generated.

#endif
