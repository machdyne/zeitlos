#ifndef ZPAGE_H
#define ZPAGE_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zpage -- what shape a card is, and where it goes on paper.
 * docs/zcanvas.md, "Pages".
 *
 * A card, a slide, a canvas and a page are one thing in slide: a
 * rectangle with a shape. The shape is a paper size (Letter, A4, an
 * index card...) or a screen ratio (4:3, 16:9), and an orientation. The
 * card's coordinates -- its design grid -- follow from the shape: 512
 * across for a landscape card, 440 down for a portrait one, so a card
 * at 100% fits a window with room beside it. Nothing is ever stored in
 * pixels of a particular screen or printer; a grid unit is whatever the
 * drawing scale makes it.
 *
 * Printing maps the grid onto a sheet: fitted into its printable area,
 * or at its actual paper size, turned a quarter if that fits better,
 * one or several to a sheet with cut marks between them.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zcanvas.h"

enum {
	ZPAGE_PAPER = 0,	// w, h in tenths of a millimetre, portrait (w <= h)
	ZPAGE_RATIO = 1,	// w, h a ratio, landscape (w >= h); no paper size
};

typedef struct {
	const char	*name;		// "letter", "a4", "4x6", "16:9" ...
	const char	*label;		// "US Letter", "A4", "4 x 6 in" ...
	uint16_t	w, h;
	uint8_t		kind;
} zpage_size_t;

typedef struct {
	zpage_size_t	size;
	bool			landscape;
	uint16_t		gw, gh;		// design grid
} zpage_t;

#define ZPAGE_GRID_LONG     512	// a landscape card's width
#define ZPAGE_GRID_PORTRAIT 440	// a portrait card's height

// The known sizes, in menu order.
int zpage_count(void);
const zpage_size_t *zpage_at(int i);
const zpage_size_t *zpage_find(const char *name);

// A page of a known size; false for an unknown name. `classic` is
// HyperCard's 512x342, exactly.
bool zpage_set(zpage_t *pg, const char *name, bool landscape);
// Paper of any size, tenths of a millimetre (a custom size or a ratio
// with ZPAGE_RATIO).
bool zpage_custom(zpage_t *pg, int kind, int w, int h, bool landscape);

// Physical size in tenths of a millimetre as oriented, or false for a
// ratio page.
bool zpage_physical(const zpage_t *pg, int *w_mm10, int *h_mm10);

// -- printing ------------------------------------------------------------

typedef struct {
	int		w, h;		// the sheet, device pixels (portrait as fed)
	int		dpi;
	// the printable area, device pixels from each edge
	int		left, top, right, bottom;
} zpage_sheet_t;

void zpage_sheet(zpage_sheet_t *sh, const zpage_size_t *paper, int dpi,
	int margin_mm10);

enum {
	ZPAGE_FIT = 0,		// as large as fits the printable area (or the cell)
	ZPAGE_ACTUAL = 1,	// the page's own paper size (paper pages only)
};

typedef struct {
	zc_xf_t		xf;			// grid units -> sheet device pixels
	int			x, y, w, h;	// the card's device rectangle on the sheet
	bool		turned;		// laid a quarter turn clockwise
} zpage_cell_t;

// Where card `index` (0..nup-1) of a sheet of `nup` goes. Several to a
// sheet are laid in the grid of columns and rows that makes them
// largest, a `gap_mm10` gutter between them for cutting. Returns false
// if nothing fits.
bool zpage_place(const zpage_t *pg, const zpage_sheet_t *sh, int nup, int index,
	int mode, int gap_mm10, zpage_cell_t *out);

// Cut marks for a sheet of cells: short lines outside each corner, in
// the gutter, as device segments (x0 y0 x1 y1 each). Returns how many
// it wrote (at most max).
int zpage_cut_marks(const zpage_sheet_t *sh, const zpage_cell_t *cells, int n,
	int len_px, int (*seg)[4], int max);

static inline int zpage_mm10_to_px(int mm10, int dpi) {
	return (int)(((int64_t)mm10 * dpi + 127) / 254);
}

#endif
