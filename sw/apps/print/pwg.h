#ifndef PRINT_PWG_H
#define PRINT_PWG_H

/*
 * print -- PWG Raster (PWG 5102.4), the page format every IPP
 * Everywhere printer takes: driverless printing, which is how a Canon
 * PIXMA prints from Linux. docs/print.md, "PWG raster".
 *
 * A file is "RaS2" and then, per page, a 1796-byte header and the
 * page's lines, each a repeat count (how many identical lines) and the
 * line in a PackBits variant that counts pixels: a byte n < 128 is the
 * next pixel n+1 times, n > 128 is 257-n literal pixels. White margins
 * and blank lines therefore cost almost nothing, and a slide page goes
 * out as tens of KB rather than eight megabytes.
 *
 * Grey only (sgray_8), or the same grey as RGB (srgb_8) for a printer
 * that takes nothing else -- three equal bytes a pixel, encoded from
 * the grey line without making an RGB copy.
 */

#include <stdint.h>
#include <stdbool.h>
#include "out.h"

#define PWG_HEADER_BYTES 1796

enum { PWG_SGRAY8 = 0, PWG_SRGB8 = 1 };

typedef struct {
	out_t		*o;
	int			w, h, dpi, space;	// pixels, dots per inch, PWG_*
	int			bpp;				// bytes a pixel: 1 or 3
	uint8_t		*prev;				// a copy of the held line, when it must be kept
	const uint8_t *held;			// the line being repeated: the caller's,
									// or prev after pwg_keep()
	int			reps;				// how many times so far (0: none held)
	int			lines;				// lines written this page
	uint8_t		*enc;				// pwg_enc_bytes(w, bpp)
} pwg_t;

// Encode buffer needed for a line of w pixels.
static inline uint32_t pwg_enc_bytes(int w, int bpp) {
	return (uint32_t)w * (uint32_t)bpp + (uint32_t)w / 2 + 16;
}

bool pwg_file(out_t *o);

typedef struct {
	int			w, h, dpi, space;
	int			page, pages;		// 1-based; pages 0 if not known
	const char	*media;				// PWG name: "na_letter_8.5x11in"
	const char	*media_type;		// "stationery", "photographic" ... or NULL
	int			quality;			// IPP print-quality (3 draft .. 5 high), 0 none
} pwg_page_t;

// Starts a page: writes its header. `prev` (w bytes) and `enc`
// (pwg_enc_bytes()) are the caller's.
bool pwg_page(pwg_t *p, out_t *o, const pwg_page_t *pg, uint8_t *prev, uint8_t *enc);
// One line of grey, w bytes, 255 white. The line is held, not copied:
// it must stay as it is until the next pwg_line(), or pwg_keep().
bool pwg_line(pwg_t *p, const uint8_t *gray);
// Copies the held line, before the buffer it is in is drawn over (the
// end of a band).
void pwg_keep(pwg_t *p);
// Ends the page; lines not given are white.
bool pwg_end(pwg_t *p);

// The PackBits line encoding alone, for the tests: returns bytes.
uint32_t pwg_encode_line(const uint8_t *gray, int w, int bpp, uint8_t *dst);

#endif
