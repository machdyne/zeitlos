#ifndef DRAWUNDO_H
#define DRAWUNDO_H

/*
 * draw -- undo and redo (docs/draw_app.md, "Undo").
 *
 *   draw_undo_init(&u, canvas, spare, mem, bytes);
 *   press:    draw_undo_begin(&u);
 *   ...the stroke or shape changes the page...
 *   release:  draw_undo_end(&u, x0, y0, x1, y1);   what it may have touched
 *   Ctrl+Z:   draw_undo_undo(&u, box)      then refresh `box`
 *   Ctrl+Y:   draw_undo_redo(&u, box)
 *
 * A level is a rectangle of the page as it was before an edit -- whole
 * words across, rows down -- cut to what the edit actually changed:
 * draw_undo_end() compares the page with `spare`, which holds the page
 * as it was before. A pencil dot costs a word; a stroke across the
 * page up to 37.5KB. Undoing a level SWAPS its words with the page's,
 * so the level then holds the page as it was after the edit, which is
 * what redo puts back: undo and redo are one operation in two
 * directions.
 *
 * Two forms, chosen by draw_undo_init():
 *
 *   - With `mem` (draw.c takes it from the arena): up to
 *     DRAW_UNDO_LEVELS levels, as a ring in mem; when a new level does
 *     not fit, the oldest go. `spare` is kept equal to the page --
 *     every change is copied across, box by box -- so a press costs
 *     nothing: the page before the stroke is already there.
 *
 *   - With no mem (no arena to give: the 1MB boards): ONE level, and it
 *     costs nothing more. `spare` holds the page before the last edit,
 *     the two differ only inside that edit's box, and undo and redo
 *     swap that box between them. The next press copies the box
 *     across, so the level goes then -- even if the new stroke turns
 *     out to change nothing.
 *
 * `spare` is also the page a file loads into (draw.c, drawio.h). A
 * load clobbers it: draw_undo_spare_lost() before one. A whole new
 * page -- New, or a load swapped in -- is draw_undo_clear().
 *
 * A new edit forgets every level that could have been redone. An edit
 * too large for mem forgets every level: what came before it can no
 * longer be reached.
 *
 * Kept apart from draw.c, which needs the window system, so the build
 * machine can test it (tests/test_draw.c).
 */

#include <stdint.h>
#include <stdbool.h>

#include "drawstamp.h"		// DRAW_PAGE_*

#define DRAW_UNDO_LEVELS	64

typedef struct {
	uint32_t	off;			// in words, into mem
	uint16_t	wa, wb;			// words across, inclusive
	uint16_t	y0, y1;			// rows, inclusive
	uint32_t	serial;			// the page's state after this edit
} draw_undo_rec_t;

typedef struct {
	uint32_t	*page;			// on screen
	uint32_t	*spare;			// see above

	uint32_t	*mem;			// NULL: the one-level form
	uint32_t	cap;			// mem's size in words

	// a ring of `count`, oldest at `first`; 0..cursor-1 can be undone,
	// cursor..count-1 redone
	draw_undo_rec_t rec[DRAW_UNDO_LEVELS];
	int			first, count, cursor;

	bool		synced;			// spare holds what it should (above)

	// Every state of the page has a number: the serial of the level
	// that made it, or `base` before the oldest level kept. `saved` is
	// the one on disk, so undoing back to it is "unmodified" again.
	uint32_t	serial, base, saved, next;
} draw_undo_t;

// `mem` (`bytes` of it, 4-aligned) for the levels, or NULL for one level
// in spare. Starts empty, at the saved state.
void draw_undo_init(draw_undo_t *u, uint32_t *page, uint32_t *spare,
	void *mem, uint32_t bytes);

// A whole new page: forgets every level, and the page as it now is is
// the saved one. `page` and `spare` as they are now (a load swaps
// them).
void draw_undo_clear(draw_undo_t *u, uint32_t *page, uint32_t *spare);

// Before a stroke or shape changes the page.
void draw_undo_begin(draw_undo_t *u);

// After it, with a rectangle (corners in any order; clipped to the
// page) that holds every pixel it may have changed. True if that made
// a level: false if nothing changed, or the change is larger than mem.
bool draw_undo_end(draw_undo_t *u, int x0, int y0, int x1, int y1);

// Undo or redo one level; false if there is none. box[] is then the
// rectangle the page changed in: x0, y0, x1, y1, inclusive.
bool draw_undo_undo(draw_undo_t *u, int box[4]);
bool draw_undo_redo(draw_undo_t *u, int box[4]);

int draw_undo_levels(const draw_undo_t *u);			// undo can go back
int draw_undo_redo_levels(const draw_undo_t *u);	// and redo forward

// The page as it is is the one on disk. / Is it?
void draw_undo_mark_saved(draw_undo_t *u);
bool draw_undo_at_saved(const draw_undo_t *u);

// Something else is about to write spare (a load).
void draw_undo_spare_lost(draw_undo_t *u);

#endif
