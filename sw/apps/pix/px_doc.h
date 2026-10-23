#ifndef PX_DOC_H
#define PX_DOC_H

/*
 * pix -- the document: a set of cells (sprites, icons, glyphs, one
 * picture), each a grid of colour indices. See docs/pix_app.md.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Pure C, no hardware: the editor, the file formats (px_io.c) and the
 * host tests (tests/) all work on this. The accessors are the only way
 * to the pixels, which are kept one of two ways:
 *
 *   depth 1    icons, fonts and two-colour pictures: a bit a pixel,
 *              rows of (w + 7) / 8 bytes, MSB leftmost
 *   depth 8    colour pictures: a colour index 0..255, a byte a pixel
 *              (no transparency: a picture is all there)
 *   depth 16   sprite sheets: a colour index 0..255 or ZSP_T
 *              (transparent), a 16-bit word a pixel
 *
 * and a palette of 2..256 colours with what stands in for each with
 * fewer (zsprite.h): a sprite sheet's or colour picture's own, black
 * and white for the others.
 *
 * -- kinds --
 *
 * What a document is decides what its cells may be, and how it is
 * saved:
 *
 *   PX_ZIC   dock icons: 32x32, a name, a spoken name, what hardware
 *            the app needs (kept as read, never edited here)
 *   PX_ZFN   a bitmap font: one cell size for every glyph, each glyph
 *            a codepoint, in ascending order; and an ascent
 *   PX_ZBM   one picture, any size up to PX_MAX_DIM: two colours
 *            (depth 1), or 4 to 256 (depth 8, a colour ZBM, up to
 *            PX_MAX_COLOUR_PX pixels)
 *   PX_ZSP   a sprite sheet: named cells of any size. The pointer's
 *            /data/wm/cursor.zsp is one (two 16x16 sprites, normal and
 *            busy, every set pixel inverting)
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "../../common/zsprite.h"

#define PX_ZIC 1
#define PX_ZFN 2
#define PX_ZBM 3
#define PX_ZSP 4

#define PX_NAME_MAX   24     /* a cell's name, NUL included */
#define PX_MAX_DIM    1024   /* the widest or tallest cell */
#define PX_MAX_CELLS  8192   /* jp12 has about 7,000 glyphs */
#define PX_MAX_COLOUR_PX (1024u * 512u)  /* a colour picture: 512 KB, and as much again for undo */

/* cell flags */
#define PX_CELL_CHANGED  0x0001u   /* edited since the file was read */
#define PX_CELL_HIDDEN   0x0002u   /* .zic: not on the dock */
#define PX_CELL_NOHW     0x0004u   /* .zic: needs hardware this board lacks */

typedef struct {
	char     name[PX_NAME_MAX];    /* sprite or icon name; a glyph's is U+XXXX */
	char     spoken[PX_NAME_MAX];  /* .zic: the spoken name */
	uint32_t code;                 /* .zfn: the codepoint */
	uint32_t feat1, feat2;         /* .zic: hardware needs, as read */
	uint16_t zic_flags;            /* .zic: the entry's flags word, as read */
	uint16_t flags;                /* PX_CELL_* */
	uint16_t w, h;
	uint32_t off;                  /* into px_doc_t.bits */
} px_cell_t;

typedef struct {
	int        kind;
	int        depth;              /* 1, 8 or 16: how the pixels are kept */
	zsp_palette_t pal;             /* pal.colours: 2, 4, 16, 64, 256 */
	int        ncells, cap;
	px_cell_t *cell;
	uint8_t   *bits;               /* every cell's pixels, packed */
	uint32_t   bits_len, bits_cap;
	int        ascent;             /* .zfn */
	bool       cursor;             /* .zsp: the pointer's file */
	bool       dirty;              /* unsaved changes */
	char      *extra;              /* .zsp: lines that are not sprites */
	uint32_t   extra_len;
} px_doc_t;

/* Bytes a w x h cell takes in this document. */
static inline uint32_t px_cell_bytes(const px_doc_t *d, int w, int h) {
	if (d->depth == 16) return (uint32_t)w * (uint32_t)h * 2u;
	if (d->depth == 8) return (uint32_t)w * (uint32_t)h;
	return (uint32_t)h * (uint32_t)((w + 7) / 8);
}

/* Whether the document has colours of its own (a palette): a sprite
 * sheet or a colour picture. */
static inline bool px_colour(const px_doc_t *d) { return d->depth > 1; }

/* What a cleared pixel is: transparent in a sprite sheet, 0 elsewhere. */
static inline int px_clear(const px_doc_t *d) {
	return d->depth == 16 ? ZSP_T : 0;
}

/* A new document of `kind` (0: none yet): its depth from the kind, a
 * 2-colour black and white palette. */
void px_doc_init(px_doc_t *d, int kind);
void px_doc_free(px_doc_t *d);

/* A new cell of w x h, cleared, at index `at` (ncells appends).
 * Returns its index, or -1 if out of memory or too big. */
int px_cell_insert(px_doc_t *d, int at, int w, int h, const char *name);

/* A copy of cell i right after it, named `name`. -1 on failure. */
int px_cell_dup(px_doc_t *d, int i, const char *name);

void px_cell_delete(px_doc_t *d, int i);

/* A cell's new size, keeping what fits of its pixels (top left): for a
 * quarter turn of a whole cell. False if out of memory or too big. */
bool px_cell_resize(px_doc_t *d, int i, int w, int h);

/* .zfn: the index a glyph for `code` goes in to keep the order, and
 * whether one is already there (*exists). */
int px_glyph_slot(const px_doc_t *d, uint32_t code, bool *exists);

/* Name a glyph cell "U+0041" (or "A" style for printable ASCII). */
void px_glyph_name(char *out, uint32_t code);

static inline uint8_t *px_cell_rows(const px_doc_t *d, int i) {
	return d->bits + d->cell[i].off;
}

/* The pixel: 0/1 at depth 1; 0..255 at depth 8; 0..255 or ZSP_T at
 * depth 16. Outside the cell, px_clear(). */
static inline int px_get(const px_doc_t *d, int i, int x, int y) {
	const px_cell_t *c = &d->cell[i];
	if (x < 0 || y < 0 || x >= c->w || y >= c->h) return px_clear(d);
	if (d->depth == 16)
		return ((const uint16_t *)(const void *)(d->bits + c->off))[(uint32_t)y * c->w + (uint32_t)x];
	if (d->depth == 8) return d->bits[c->off + (uint32_t)y * c->w + (uint32_t)x];
	return (d->bits[c->off + (uint32_t)y * ((c->w + 7) / 8) + (x >> 3)]
		>> (7 - (x & 7))) & 1;
}

/* At depth 1 only 1 is ink: anything else (0, ZSP_T) clears. At
 * depth 8 ZSP_T is 0: a picture has no see-through. */
static inline void px_set(px_doc_t *d, int i, int x, int y, int v) {
	px_cell_t *c = &d->cell[i];
	uint8_t *p, m;
	if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
	if (d->depth == 16) {
		if (v != ZSP_T) v &= 255;
		((uint16_t *)(void *)(d->bits + c->off))[(uint32_t)y * c->w + (uint32_t)x] = (uint16_t)v;
		return;
	}
	if (d->depth == 8) {
		d->bits[c->off + (uint32_t)y * c->w + (uint32_t)x] = (uint8_t)(v == ZSP_T ? 0 : v);
		return;
	}
	p = &d->bits[c->off + (uint32_t)y * ((c->w + 7) / 8) + (x >> 3)];
	m = (uint8_t)(0x80u >> (x & 7));
	if (v == 1) *p |= m; else *p &= (uint8_t)~m;
}

/* A cell has been edited: flag it, and the document. */
static inline void px_touch(px_doc_t *d, int i) {
	d->cell[i].flags |= PX_CELL_CHANGED;
	d->dirty = true;
}

/* Find a cell by name; -1 if none. */
int px_find(const px_doc_t *d, const char *name);

#endif
