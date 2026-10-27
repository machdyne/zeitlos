/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * learn -- drawing, through a device.
 *
 * Everything learn shows is drawn with the few things the graphics
 * hardware does (docs/gpu_blitter.md): rectangles filled solid, with a
 * shade or an 8x8 pattern; one-pixel lines; a one-bit bitmap from
 * memory copied onto the screen; a rectangle scrolled. A page is where
 * they go:
 *
 *   - on the board, the window itself: each call is a blitter or
 *     rasterizer operation (learn.c), nothing is drawn by the CPU but
 *     the spans of round shapes and the lines of text, and nothing is
 *     copied afterwards;
 *   - a one-bit bitmap in memory (ld_page()): the same operations in
 *     software, as the hardware does them -- the tests draw every
 *     screen this way and write it out as a picture, and a board
 *     without the blitter draws here and copies the bitmap.
 *
 * Text: each glyph is drawn once (zcfont, any face and size) into a
 * glyph cache; a line of text is then the cached glyphs put side by
 * side in a strip and one copy of the strip. Ink is a lit pixel (the
 * phosphor), the ground is dark.
 *
 * Coordinates are the page's pixels. A page has a clip (half-open);
 * nothing is drawn outside it.
 */

#ifndef LDRAW_H
#define LDRAW_H

#include <stdint.h>
#include <stdbool.h>

#include "zcanvas.h"
#include "zcfont.h"

// the fonts learn uses (docs/fonts.md); any may be missing and falls back
typedef struct {
	zc_font_t	f5, f6;					// built in: 5x8, 6x12
	zc_font_t	body, body_b;			// m9x15, m9x15b: reading
	zc_font_t	small, small_b;			// m7x13, m7x13b: secondary
	zc_font_t	big, big_b;				// m10x20 (no bold: smeared)
	zc_font_t	mono;					// m6x13: code
	zc_font_t	jp;						// jp12: Japanese
	zc_font_t	serif, serif_b, serif_i, sans_b;	// Hershey: headings, formulas
	bool		have_jp;
} ld_fonts_t;

extern ld_fonts_t LDF;

// a font's file image, from wherever the platform keeps fonts; NULL if none
typedef const void *(*ld_font_loader)(const char *file, uint32_t *len);
// loads every face learn uses (built-ins always work); keeps the images
void ld_fonts_load(ld_font_loader load);

enum { LD_DARK = 0, LD_INK = 1, LD_XOR = 2 };

// how a rectangle is filled
enum { LD_K_DARK = 0, LD_K_INK, LD_K_XOR, LD_K_SHADE, LD_K_PAT };
typedef struct {
	uint8_t		kind;
	uint8_t		level;		// LD_K_SHADE: 0 dark .. 16 ink, a 4x4 ordered dither
	uint8_t		pat[8];		// LD_K_PAT: rows, most significant bit leftmost, 1 lit
} ld_ink_t;

// how a bitmap goes onto the page (the blitter's raster operations)
enum { LD_ROP_COPY = 0, LD_ROP_OR = 1, LD_ROP_XOR = 2, LD_ROP_ANDN = 3 };

typedef struct { int x0, y0, x1, y1; } ld_clip_t;	// half-open

typedef struct ld_page ld_page_t;

// What a device does. Rectangles and copies arrive inside the page and
// inside the clip; a line comes with the clip. Bitmaps (blit, save) are
// rows of `wpl` 32-bit words, pixel x at bit x & 31 of word x >> 5 --
// the framebuffer's order.
typedef struct {
	void	(*fill)(ld_page_t *p, int x, int y, int w, int h, const ld_ink_t *k);
	void	(*line)(ld_page_t *p, int x0, int y0, int x1, int y1, int c, const ld_clip_t *clip);
	void	(*blit)(ld_page_t *p, const uint32_t *bits, int wpl, int sx, int sy,
				int x, int y, int w, int h, int rop);
	// optional (NULL: cannot): moves (x, y, w, h) by dy, the strip left is not drawn
	bool	(*scroll)(ld_page_t *p, int x, int y, int w, int h, int dy);
	// optional: the pixels of (x, y, w, h) into bits at (0, 0); false if they cannot be read
	bool	(*save)(ld_page_t *p, int x, int y, int w, int h, uint32_t *bits, int wpl);
} ld_dev_t;

struct ld_page {
	const ld_dev_t	*dev;
	void			*ctx;		// the device's
	int				w, h;		// pixels
	int				cx0, cy0, cx1, cy1;	// the clip
	uint32_t		*bits;		// a bitmap page's
	int				wpl;
};

// a page over a 1bpp buffer of (w+31)/32 words a row, h rows (drawn in software)
void ld_page(ld_page_t *p, uint32_t *bits, int w, int h);
// a page drawn by a device
void ld_page_dev(ld_page_t *p, const ld_dev_t *dev, void *ctx, int w, int h);
// the software device (what ld_page() uses), for a device that draws
// into memory some of the time
extern const ld_dev_t ld_soft;
// working memory: the glyph cache, and room for drawing (once)
void ld_init(void *scratch, uint32_t scratch_bytes, void *cache, uint32_t cache_bytes);

// the clip: narrowed (only ever), put back, the whole page
ld_clip_t ld_clip_push(ld_page_t *p, int x0, int y0, int x1, int y1);
void ld_clip_pop(ld_page_t *p, ld_clip_t was);
void ld_clip_reset(ld_page_t *p);
static inline bool ld_clip_hits(const ld_page_t *p, int x, int y, int w, int h) {
	return x < p->cx1 && y < p->cy1 && x + w > p->cx0 && y + h > p->cy0;
}

void ld_clear(ld_page_t *p);		// the clip, dark
void ld_paint(ld_page_t *p, int x, int y, int w, int h, const ld_ink_t *k);
void ld_fill(ld_page_t *p, int x, int y, int w, int h, int c);		// c: LD_DARK, LD_INK, LD_XOR
void ld_shade(ld_page_t *p, int x, int y, int w, int h, int tone);	// tone 0..16 of ink
void ld_frame(ld_page_t *p, int x, int y, int w, int h, int lw, int c);
void ld_rrect(ld_page_t *p, int x, int y, int w, int h, int r, int c);
void ld_rframe(ld_page_t *p, int x, int y, int w, int h, int r, int lw, int c);
void ld_hline(ld_page_t *p, int x0, int x1, int y, int c);
void ld_vline(ld_page_t *p, int x, int y0, int y1, int c);
void ld_line(ld_page_t *p, double x0, double y0, double x1, double y1, double w, int c);
void ld_disc(ld_page_t *p, double cx, double cy, double r, int c);
void ld_ring(ld_page_t *p, double cx, double cy, double r, double w, int c);
void ld_poly(ld_page_t *p, const double *xy, int n, int c);
void ld_arc(ld_page_t *p, double cx, double cy, double r, int a0, int a1, double w, int c);	// 1024 a turn

// The same shapes in any ink (figures: a pattern for a colour), in
// floats (a figure's units scaled to the page). A line of width <= 1.25
// is one pixel wide.
void ld_k_line(ld_page_t *p, float x0, float y0, float x1, float y1, float w, const ld_ink_t *k);
void ld_k_disc(ld_page_t *p, float cx, float cy, float r, const ld_ink_t *k);
void ld_k_ring(ld_page_t *p, float cx, float cy, float r, float w, const ld_ink_t *k);
void ld_k_poly(ld_page_t *p, const float *xy, int n, const ld_ink_t *k);	// nonzero winding
// joined segments through n points (closed: back to the first)
void ld_k_polyline(ld_page_t *p, const float *xy, int n, float w, bool closed, const ld_ink_t *k);
ld_ink_t ld_ink(int c);		// LD_DARK, LD_INK, LD_XOR as an ink

// a bitmap onto the page (bits at (sx, sy), w x h, to (x, y))
void ld_blit(ld_page_t *p, const uint32_t *bits, int wpl, int sx, int sy, int x, int y, int w, int h, int rop);
// moves (x, y, w, h) by dy; false if the device cannot (draw it all instead)
bool ld_scroll(ld_page_t *p, int x, int y, int w, int h, int dy);
// the pixels of (x, y, w, h) into bits; false if they cannot be read
bool ld_save(ld_page_t *p, int x, int y, int w, int h, uint32_t *bits, int wpl);

// -- text --

typedef struct {
	const zc_font_t	*font;
	int				size;		// px; 0 = a bitmap font's own height
	bool			bold, italic, underline;
} ld_style_t;

ld_style_t ld_st(const zc_font_t *f, int size);
// returns the x after the text
int ld_text(ld_page_t *p, const ld_style_t *st, int x, int base, const char *s, int len, int c);
int ld_textz(ld_page_t *p, const ld_style_t *st, int x, int base, const char *s, int c);
int ld_width(const ld_style_t *st, const char *s, int len);
int ld_widthz(const ld_style_t *st, const char *s);
// ascent, descent and line height in pixels
void ld_metrics(const ld_style_t *st, int *asc, int *desc, int *line);
// at most w pixels wide: cut with "..." if it does not fit
int ld_fitz(ld_page_t *p, const ld_style_t *st, int x, int base, int w, const char *s, int c);
int ld_text_c(ld_page_t *p, const ld_style_t *st, int cx, int base, const char *s, int c);
int ld_text_r(ld_page_t *p, const ld_style_t *st, int rx, int base, const char *s, int c);
// wrapped into w; returns the baseline after the last line (the first is `base`)
int ld_wrap(ld_page_t *p, const ld_style_t *st, int x, int base, int w, int lead,
	const char *s, int c);
// how many lines ld_wrap would take
int ld_wrap_lines(const ld_style_t *st, int w, const char *s);
// glyphs drawn into the cache, and found there (tests, tuning)
void ld_glyph_stats(uint32_t *drawn, uint32_t *found);

// -- pieces of learn's look (docs/learn_ui.md) --

// a key's name in a small box: returns the x after it
int ld_keycap(ld_page_t *p, int x, int base, const char *k, int c);
// "Enter|continue;R|review": keycaps and words, from x; returns the x after
int ld_keys(ld_page_t *p, int x, int base, const char *spec, int c);
void ld_button(ld_page_t *p, int x, int y, int w, int h, const char *label, bool filled, bool focus);
void ld_bar(ld_page_t *p, int x, int y, int w, int h, int pct);
void ld_mastery(ld_page_t *p, int x, int y, int level);		// 0..4, an 11 px disc in quarters
void ld_speaker(ld_page_t *p, int x, int y, int c);
void ld_tick(ld_page_t *p, int x, int y, int c);
void ld_cross(ld_page_t *p, int x, int y, int c);

#endif
