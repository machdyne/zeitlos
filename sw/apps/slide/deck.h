#ifndef SLIDE_DECK_H
#define SLIDE_DECK_H

/*
 * slide -- the deck: backgrounds, cards and the objects on them.
 * docs/slide_app.md, "The deck".
 *
 * Everything lives in the pool (sw/common/zpool.h). Coordinates are design-grid
 * units (zpage.h): 512 across a landscape card. Nothing here knows
 * about windows or pixels.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zpage.h"

// -- objects -------------------------------------------------------------

enum {
	OB_TEXT = 0,	// a text box (it can have a fill and a frame too)
	OB_RECT,
	OB_RRECT,
	OB_OVAL,
	OB_LINE,		// from (x, y) to (x + w, y + h): w and h may be negative
	OB_POLY,		// points, open or closed
	OB_IMAGE,
	// controls (Phase 3): they work in browse mode, a show and a deck
	// run as an app (docs/slide_app.md, "Controls")
	OB_BUTTON,		// text: the label; action on a click
	OB_CHECK,		// text: the label; value 0/1
	OB_RADIO,		// text: the label; value 0/1, one of its group on
	OB_FIELD,		// text: what is typed into it
	OB_SLIDER,		// value vmin..vmax
	OB_LIST,		// text: the items, a line each; value the chosen, -1 none
	// diagram shapes (Phase 5): flowchart symbols, with text inside
	OB_DIAMOND,		// a decision
	OB_SLANT,		// data, input/output: a parallelogram
	OB_PILL,		// start, end: a rectangle with round ends
	OB_DOC,			// a document: the bottom edge a wave
	OB_DRUM,		// stored data: a cylinder
	OB_KINDS
};

#define OB_IS_FLOW(k)		((k) >= OB_DIAMOND && (k) <= OB_DRUM)
// A closed shape that shows its text inside (text centred by default)
#define OB_IS_SHAPE(k)		((k) == OB_RECT || (k) == OB_RRECT || (k) == OB_OVAL || OB_IS_FLOW(k))

#define OB_IS_CONTROL(k)	((k) >= OB_BUTTON && (k) <= OB_LIST)
// Text edited in place, in the box as it is drawn (the others' text is
// the "text" command's: a check's label, a list's items)
#define OB_EDITS_TEXT(k)	((k) == OB_TEXT || (k) == OB_BUTTON || (k) == OB_FIELD || OB_IS_SHAPE(k))
// Text with a font: the text box and the controls with words
#define OB_HAS_FONT(k)		((k) == OB_TEXT || (OB_IS_CONTROL(k) && (k) != OB_SLIDER) || OB_IS_SHAPE(k))

enum { FILL_NONE = 0, FILL_SHADE, FILL_PATTERN };

#define OBF_HIDDEN  1
#define OBF_LOCKED  2
#define OBF_FLIPX   4		// mirrored across (a slant, a document)
#define OBF_FLIPY   8		// mirrored up and down

#define ST_BOLD      1
#define ST_ITALIC    2
#define ST_UNDERLINE 4

enum { AL_LEFT = 0, AL_CENTER, AL_RIGHT };
enum { VA_TOP = 0, VA_MIDDLE, VA_BOTTOM };

#define AR_START 1
#define AR_END   2

#define OB_NAME_MAX 24

typedef struct img_cache img_cache_t;		// render.c's

typedef struct obj {
	uint8_t		kind, flags;
	int16_t		x, y, w, h;

	uint8_t		fill;			// FILL_*
	uint8_t		fill_v;			// tone 0..16, or pattern 0..15
	uint8_t		line_w;			// frame/stroke width, units; 0 = none
	uint8_t		line_tone;		// 0..16 (16 black)
	uint8_t		radius;			// OB_RRECT
	uint8_t		arrows;			// OB_LINE / open OB_POLY: AR_*
	uint8_t		closed;			// OB_POLY
	uint8_t		smooth;			// OB_POLY: a curve through its points' middles

	// text (OB_TEXT; other kinds keep their text but do not show it yet)
	uint8_t		font;			// index into the deck's font names
	uint8_t		size;			// units
	uint8_t		style;			// ST_*
	uint8_t		align, valign;
	uint8_t		text_tone;		// 16 black, 0 white
	uint8_t		pad;			// inner margin, units
	char		*text;

	int16_t		*pts;			// OB_POLY: x, y pairs, absolute
	uint16_t	npts;

	char		*src;			// OB_IMAGE: path, relative to the deck
	int8_t		contrast, bright;
	img_cache_t	*img;

	// a connector (Phase 5): a line or an elbow (open poly) whose ends
	// follow the objects called `from` and `to` (card_route())
	char		from[OB_NAME_MAX], to[OB_NAME_MAX];
	uint16_t	gid;			// a group: selected and moved together; 0 none

	// controls
	int16_t		value;			// check/radio 0 1; slider; list index (-1 none)
	int16_t		vmin, vmax;		// slider
	char		group[12];		// radio: the others it excludes ("" all on the card)

	char		name[OB_NAME_MAX];
	char		*action;		// what a click does: forms in Scheme notation,
								// "(go 'next)" (act.h)
	char		*script;
} obj_t;

// -- cards ---------------------------------------------------------------

enum { TR_CUT = 0, TR_WIPE, TR_PUSH, TR_IRIS, TR_DISSOLVE, TR_KINDS };

#define CARD_NAME_MAX 32

typedef struct card {
	char		name[CARD_NAME_MAX];
	int			bg;				// background index, -1 none
	obj_t		**obj;			// bottom to top
	int			nobj, cap;
	char		*notes;
	uint8_t		trans;			// TR_*
	uint16_t	trans_ms;
	uint32_t	advance_ms;		// 0 = never on its own
	char		*action;		// run on arriving at the card (browse, show, app)
	char		*script;
} card_t;

#define DECK_FONTS_MAX 32
#define DECK_TITLE_MAX 64
#define DECK_PRINT_MAX 128

typedef struct deck {
	zpage_t		page;
	char		title[DECK_TITLE_MAX];
	char		print[DECK_PRINT_MAX];	// how it prints: "4up cut paper a4" (printjob.h)
	card_t		**card;
	int			ncard, capcard;
	card_t		**bg;			// backgrounds: cards that are shared
	int			nbg, capbg;
	char		*font[DECK_FONTS_MAX];	// font names objects refer to
	int			nfont;
	char		*script;
	bool		app;			// opens to run, not to edit: a deck as an app
	bool		loop;			// a show goes round: after the last, the first
} deck_t;

// The deck in use. One per process; the editor and the show share it.
extern deck_t *g_deck;

deck_t *deck_new(void);			// one empty card, classic page
void deck_free(deck_t *d);

card_t *card_new(const char *name);
void card_free(card_t *c);
card_t *card_dup(const card_t *c);
// Inserts at index (0..ncard); takes ownership. false when out of memory.
bool deck_insert_card(deck_t *d, int at, card_t *c);
card_t *deck_remove_card(deck_t *d, int at);	// caller frees
bool deck_move_card(deck_t *d, int from, int to);
int deck_find_card(const deck_t *d, const char *name);
bool deck_insert_bg(deck_t *d, int at, card_t *c);
int deck_find_bg(const deck_t *d, const char *name);
// A name no card in the deck has yet: "card 3".
void deck_unique_card_name(const deck_t *d, char *out, int cap);

// The font index for a name, adding it if new; 0 if the table is full.
int deck_font_index(deck_t *d, const char *name);

obj_t *obj_new(int kind);		// defaults for the kind
// Called for every object freed (render.c drops its image cache).
extern void (*obj_free_hook)(obj_t *o);
void obj_free(obj_t *o);
obj_t *obj_dup(const obj_t *o);
bool obj_set_text(obj_t *o, const char *s, uint32_t len);
bool obj_set_str(char **field, const char *s);	// src, action, script, notes
bool card_add(card_t *c, int at, obj_t *o);		// at = -1: on top
obj_t *card_take(card_t *c, int i);				// removes, does not free
int card_index_of(const card_t *c, const obj_t *o);
// A name no object on the card has: "rect 2".
void card_unique_obj_name(const card_t *c, int kind, char *out, int cap);
obj_t *card_find_obj(const card_t *c, const char *name);

const char *obj_kind_name(int kind);		// "text", "rect", ...
int obj_kind_of(const char *name);			// -1 if unknown

// Connectors: every line or elbow on c whose `from` and `to` name
// objects (on c, or on bg, the card's background, which may be NULL)
// is laid between them again -- after a move, a resize, a load. A line
// goes straight between their edges; an elbow (an open polygon) goes
// across, then along, then across. Returns how many changed, and their
// boxes before and after in *chg (up to `max` of each), to redraw.
int card_route(card_t *c, const card_t *bg, int (*chg)[4], int max);

// An object made a connector: line (straight) or elbow (`elbow`).
// False if it is not a line or a polygon, or out of memory.
bool obj_make_connector(obj_t *o, const char *from, const char *to, bool elbow);
bool obj_is_connector(const obj_t *o);
// A polygon's box made its points' again (after a point moved).
void obj_fit_box(obj_t *o);

// Bounding box, units (a line's ends ordered).
void obj_bounds(const obj_t *o, int *x0, int *y0, int *x1, int *y1);
void obj_move(obj_t *o, int dx, int dy);
// New box; polygons scale their points into it.
void obj_set_box(obj_t *o, int x, int y, int w, int h);

#endif
