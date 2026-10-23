#ifndef PX_EDIT_H
#define PX_EDIT_H

/*
 * pix -- what the tools do to a cell, and undo. Pure C, like px_doc.c:
 * the editor calls these, the host tests check them.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */

#include <stdint.h>
#include <stdbool.h>

#include "px_doc.h"

/* -- shapes, in colour v, clipped to the cell -- */

void px_line(px_doc_t *d, int i, int x0, int y0, int x1, int y1, int v);
void px_rect(px_doc_t *d, int i, int x0, int y0, int x1, int y1, int v, bool filled);
/* Flood fill from (x, y), 4-connected. False if nothing changed. */
bool px_flood(px_doc_t *d, int i, int x, int y, int v);

/* Whether (x, y) is on the line or rectangle from (x0, y0) to (x1, y1):
 * the preview while one is being dragged. */
bool px_on_line(int x0, int y0, int x1, int y1, int x, int y);
bool px_on_rect(int x0, int y0, int x1, int y1, int x, int y, bool filled);

/* -- pieces: a rectangle of pixels lifted out of a cell --
 *
 * One word a pixel, a colour or ZSP_T, so a piece can be flipped,
 * turned and moved without caring about how the document keeps it. The
 * clipboard is one; so is a selection being moved. */
typedef struct {
	int w, h;
	uint16_t *px;
} px_piece_t;

bool px_piece_take(px_piece_t *p, const px_doc_t *d, int i, int x, int y, int w, int h);
/* Into cell i at (x, y), clipped. `transparent`: px_clear() pixels of
 * the piece leave the cell's as they are. */
void px_piece_put(const px_piece_t *p, px_doc_t *d, int i, int x, int y, bool transparent);
void px_piece_free(px_piece_t *p);
/* A copy of q into p (p's old pixels freed). False if out of memory. */
bool px_piece_copy(px_piece_t *p, const px_piece_t *q);
static inline int px_piece_get(const px_piece_t *p, int x, int y) {
	return (x < 0 || y < 0 || x >= p->w || y >= p->h) ? ZSP_T : p->px[y * p->w + x];
}
void px_piece_flip_h(px_piece_t *p);
void px_piece_flip_v(px_piece_t *p);
/* A quarter turn clockwise: w and h swap. */
bool px_piece_rotate(px_piece_t *p);
/* Shift by (dx, dy) with wrap: what leaves one edge comes in the other. */
void px_piece_shift(px_piece_t *p, int dx, int dy);

/* -- undo --
 *
 * Before each stroke the editor saves the cell it is about to change;
 * undo puts that back and keeps what it replaced for redo. A step is
 * kept as only the rows and bytes the edit changed (it is cut down when
 * it is settled: at the next step, at undo, or px_undo_settle()), so a
 * dot in a big picture costs bytes. Up to PX_UNDO_STEPS steps or
 * PX_UNDO_BYTES, whichever is less: the oldest go first. A cell's size
 * is part of what is saved, so a step taken before a rotate undoes it.
 * Steps name their cell by index, so the editor clears the history
 * when cells are added or removed. */
#define PX_UNDO_STEPS 64
#define PX_UNDO_BYTES (512u * 1024u)

void px_undo_clear(void);
bool px_undo_save(const px_doc_t *d, int i);
/* Cut the newest step down now (the editor calls it when a stroke ends). */
void px_undo_settle(const px_doc_t *d);
/* Undo (redo = false) or redo. The cell it changed, or -1. */
int px_undo(px_doc_t *d, bool redo);
int px_undo_depth(bool redo);
/* Bytes the steps hold. */
uint32_t px_undo_held(void);

#endif
