#ifndef SLIDE_EDIT_H
#define SLIDE_EDIT_H

/*
 * slide -- editing a deck, without a window: tools, selection, drags,
 * text, commands, undo, the clipboard. slide.c turns window events into
 * these calls and draws what they leave behind; the tests drive them
 * directly. Coordinates are design units (deck.h).
 */

#include <stdint.h>
#include <stdbool.h>

#include "deck.h"
#include "deckio.h"
#include "render.h"

enum {
	TOOL_BROWSE = 0,	// clicks go to the card, as in a show
	TOOL_SELECT,
	TOOL_TEXT,
	TOOL_RECT,
	TOOL_RRECT,
	TOOL_OVAL,
	TOOL_LINE,
	TOOL_ARROW,
	TOOL_POLY,
	TOOL_PEN,		// freehand: a smooth polygon, drawn as the pointer goes
	TOOL_IMAGE,
	TOOL_COUNT
};

enum {
	DRAG_NONE = 0,
	DRAG_MOVE,			// the selection
	DRAG_HANDLE,		// one handle of the one selected object
	DRAG_CREATE,		// a new shape, corner to corner
	DRAG_BAND,			// rubber band selection
	DRAG_PEN,			// a freehand line being drawn (into poly[])
};

#define SEL_MAX 256
#define UNDO_MAX 64

typedef struct {
	char		*text;		// a card or the deck, in .zsl
	int			card;		// which card, -1 the whole deck
	bool		bg;			// a background, not a card
} undo_t;

typedef struct {

	deck_t		*d;
	int			cur;			// current card
	bool		on_bg;			// editing the current card's background
	int			tool;

	obj_t		*sel[SEL_MAX];
	int			nsel;

	// text being typed into (OB_TEXT), and where
	obj_t		*text_obj;
	uint32_t	caret;
	int			caret_goal;		// x (units) up/down keep, -1 none

	// a drag in progress
	int			drag;
	int			handle;
	int			ax, ay, bx, by;	// where it began, where it is
	int			ox0, oy0, ox1, oy1;	// the handle-dragged object's box when it began
	bool		constrain;		// shift: squares, 45 degree lines

	// a polygon being drawn
	int16_t		poly[2 * 128];
	int			npoly;

	int			snap;			// units; 0 off

	// damage, units: what needs drawing again (x1 < x0: nothing)
	int			dx0, dy0, dx1, dy1;
	bool		all_dirty;		// the whole card (or a different one)
	bool		modified;

	undo_t		undo[UNDO_MAX], redo[UNDO_MAX];
	int			nundo, nredo;

	char		msg[96];		// the last thing worth saying in the status line
} editor_t;

void edit_init(editor_t *e, deck_t *d);
void edit_reset(editor_t *e, deck_t *d);	// a different deck: no history
card_t *edit_card(const editor_t *e);		// what is being edited: card or background

// -- selection --------------------------------------------------------------

void edit_select_none(editor_t *e);
void edit_select(editor_t *e, obj_t *o, bool add);
bool edit_is_selected(const editor_t *e, const obj_t *o);
void edit_select_all(editor_t *e);
// topmost object at (x, y), NULL for none; `tol` units around lines
obj_t *edit_hit(const editor_t *e, int x, int y, int tol);
// Lays the card's connectors again (deck.h, card_route()) and marks what
// moved for redrawing; the window calls it before drawing.
void edit_route(editor_t *e);
// handle of `o` at (x, y): 0..7 clockwise from top-left, -1 none (lines: 0, 1)
// Handles: a box's eight; a line's two ends; a polygon's eight and then
// one at each corner (handle 8 + i is corner i), up to EDIT_POINTS_MAX
// corners -- dragged to move it, double-clicked to take it away.
#define EDIT_POINTS_MAX  32
#define EDIT_HANDLES_MAX (8 + EDIT_POINTS_MAX)
int edit_handle_at(const obj_t *o, int x, int y, int tol);
int edit_handles(const obj_t *o, int hx[EDIT_HANDLES_MAX], int hy[EDIT_HANDLES_MAX]);

// -- the mouse ----------------------------------------------------------------

void edit_press(editor_t *e, int x, int y, bool shift, bool dbl);
void edit_motion(editor_t *e, int x, int y, bool shift);
void edit_release(editor_t *e, int x, int y);
// the box a drag shows (units), for the outline: false when none
bool edit_drag_box(const editor_t *e, int *x0, int *y0, int *x1, int *y1, int *kind);
// the object a drag will produce or the selection moved, as it would
// look now (a moved/resized copy), for outlines
void edit_finish_poly(editor_t *e);
void edit_cancel(editor_t *e);

// -- the keyboard ---------------------------------------------------------------

// Text editing keys, when text_obj is set. True if the key was used.
bool edit_text_key(editor_t *e, uint32_t keysym, uint8_t mods);
void edit_text_insert(editor_t *e, const char *s, uint32_t n);
void edit_text_begin(editor_t *e, obj_t *o, int x, int y);	// caret at (x, y), or the end
void edit_text_end(editor_t *e);
// caret position for drawing: x, top, height (units, 24.8)
void edit_caret(const editor_t *e, zc_fix *x, zc_fix *y, zc_fix *h);

void edit_nudge(editor_t *e, int dx, int dy);
void edit_delete(editor_t *e);
void edit_duplicate(editor_t *e);
void edit_order(editor_t *e, int how);	// 0 back, 1 backward, 2 forward, 3 front

// -- cards ---------------------------------------------------------------------

void edit_goto(editor_t *e, int card);
void edit_new_card(editor_t *e, bool duplicate);
void edit_delete_card(editor_t *e);
void edit_move_card(editor_t *e, int from, int to);

// -- commands (the "/" line) ------------------------------------------------------

// Runs one command line. The message (an answer, or what was wrong) is
// in e->msg. Some commands are the app's (open, save, image, show...);
// those come back as false with *app set to the command word.
bool edit_command(editor_t *e, const char *line, const char **app);

// An SVG drawing's outlines as polygons on the card (/import): fitted
// into its middle, grouped and selected, one undo step. Returns how many
// it made (0: none, said in e->msg); -1 for a file that is not SVG.
int edit_import_svg(editor_t *e, const char *doc, uint32_t len);
// Every command with a line of help, for the "/" list and docs.
int edit_command_count(void);
const char *edit_command_help(int i);

// -- undo ----------------------------------------------------------------------

// Before a change: keeps the card (or the whole deck) as it is.
void edit_checkpoint(editor_t *e, bool whole_deck);
bool edit_undo(editor_t *e);
bool edit_redo(editor_t *e);

// -- the clipboard ---------------------------------------------------------------

bool edit_copy(editor_t *e, sbuf_t *out);
int edit_paste(editor_t *e, const char *text, uint32_t len);

// -- damage ----------------------------------------------------------------------

void edit_damage(editor_t *e, int x0, int y0, int x1, int y1);
void edit_damage_obj(editor_t *e, const obj_t *o);
void edit_damage_all(editor_t *e);
void edit_damage_clear(editor_t *e);

int edit_snap(const editor_t *e, int v);

#endif
