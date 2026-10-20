#ifndef ZPSVG_H
#define ZPSVG_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zpsvg -- a page of a zpaper job (zpaper.h) as an SVG document: what an
 * app draws for printing, for a drawing program or a web page instead.
 * docs/zpaper.md, "As SVG".
 *
 * Fills and strokes become paths, shades greys, the 16 patterns SVG
 * patterns; text stays text (<text>, in the nearest generic family: sans
 * is sans-serif, serif serif, the misc-fixed faces monospace -- a
 * reader's own fonts, so it can be edited, and looks a little
 * different, but as wide when `font` is given); a picture by its path is embedded (JPEG, PNG, GIF, BMP:
 * what a browser reads) when `load` gives its bytes, else linked; pixel
 * images are embedded as BMP. Units: the page's own, a slide card is
 * 512 wide (the job's device units divided by ZP_SUB).
 */

#include <stdint.h>
#include <stdbool.h>
#include "zcfont.h"

// Writes text out; false stops the conversion.
typedef bool (*zp_svg_put_fn)(void *ctx, const char *s, uint32_t len);
// A file's bytes (a picture to embed), or NULL; freed with `release`.
typedef uint8_t *(*zp_svg_load_fn)(void *ctx, const char *path, uint32_t *len);

typedef struct {
	zp_svg_put_fn	put;
	zp_svg_load_fn	load;		// may be NULL: pictures are linked
	void			(*release)(void *ctx, uint8_t *p);
	// The font a run was drawn in (as zp_play_t has it), or NULL: with
	// it, each run keeps its width (textLength), so a reader's font,
	// wider or narrower, still fits where the words were.
	bool			(*font)(void *ctx, const char *name, const zc_font_t **out);
	void			*ctx;
	uint32_t		skipped;	// things it could not say (a mask image)
} zp_svg_t;

// Page `index` (0-based) of the job as one SVG document. False if there
// is no such page or a write failed.
bool zp_svg_page(zp_svg_t *s, const void *job, uint32_t len, int index);

#endif
