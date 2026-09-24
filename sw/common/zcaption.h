#ifndef ZCAPTION_H
#define ZCAPTION_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Captions: large text over the whole desktop, with no window.
 * See docs/captions.md.
 *
 *     z_caption_show("Hello, world", Z_CAPTION_SCALE(3));
 *     z_caption_show("Volume 40", Z_CAPTION_COMPACT | Z_CAPTION_CENTER
 *                                 | Z_CAPTION_TIMEOUT(1500));
 *     z_caption_hide();
 *
 * wm draws it (Z_WM_CAPTION, zwm.h). There is one caption for the
 * whole system; showing one replaces whatever was there. Text is
 * UTF-8, drawn from z_font_6x12 scaled up, so everything in ISO 8859-15
 * shows -- German, French, the euro sign -- and anything else is the
 * missing-glyph box. '\n' breaks a line; long lines are word-wrapped,
 * up to Z_CAPTION_LINES_MAX lines.
 *
 * The second half of this header is the renderer wm uses. It is pure
 * (no I/O, no globals) so the host test in sw/common/tests draws the
 * same pixels the board does.
 */

#include <stdint.h>
#include <stdbool.h>

// -- options (the Z_WM_CAPTION tag) --

// 1, 2 or 3 times the 6x12 font. 0 means the default, 2.
//   2x: 12x24 cells, ~50 characters a line -- legible in a video
//       watched full-screen.
//   3x: 18x36 cells, ~33 characters a line -- legible on a phone.
#define Z_CAPTION_SCALE(n)      ((uint32_t)(n) & 3u)
#define Z_CAPTION_GET_SCALE(o)  ((int)((o) & 3u))

#define Z_CAPTION_BOTTOM        (0u << 2)	// above the dock (default)
#define Z_CAPTION_TOP           (1u << 2)
#define Z_CAPTION_CENTER        (2u << 2)
#define Z_CAPTION_GET_POS(o)    (((o) >> 2) & 3u)

// Box sized to the text rather than a full-width band. The band is
// the default because consecutive captions of the same line count then
// occupy exactly the same pixels, and nothing underneath has to be
// repainted between them.
#define Z_CAPTION_COMPACT       (1u << 4)

// Lit box, dark text. The default is dark with a lit border.
#define Z_CAPTION_INVERSE       (1u << 5)

// Hide by itself after `ms`, rounded down to 100ms; 0 = stay until
// replaced or hidden. Up to about 109 minutes.
#define Z_CAPTION_TIMEOUT(ms)   (((((uint32_t)(ms)) / 100u) & 0xFFFFu) << 16)
#define Z_CAPTION_GET_TIMEOUT_MS(o) ((((o) >> 16) & 0xFFFFu) * 100u)

// -- showing one --

// Returns false if wm could not be reached. The text is copied, so a
// stack buffer is fine.
bool z_caption_show(const char *utf8, uint32_t opts);
bool z_caption_hide(void);

// -- the renderer (wm, and the host test) --

#define Z_CAPTION_TEXT_MAX      256	// Latin-9 bytes, after conversion
#define Z_CAPTION_LINES_MAX     3

// UTF-8 to the Latin-9 bytes the font is indexed by. '\n' is kept,
// other control characters become spaces, a codepoint Latin-9 lacks
// becomes the missing-glyph box. Always NUL-terminates. Returns the
// length.
int z_caption_to_l9(const char *utf8, char *out, int cap);

// Lays out and draws `l9` into a 1bpp bitmap in framebuffer bit order
// (pixel x at bit x&31 of word x>>5, LSB leftmost), top-left at 0,0.
//
// `band_w` is the width of a full-width band, and the most a compact
// box may use; the caller subtracts its own margins first. `bits` has
// `stride_words` words per row and `max_h` rows, and is cleared by
// this call only as far as the box reaches.
//
// Returns false (and draws nothing) for text with nothing to show.
bool z_caption_render(const char *l9, uint32_t opts, int band_w,
	uint32_t *bits, int stride_words, int max_h, int *out_w, int *out_h);

#endif
