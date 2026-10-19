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
	OB_KINDS
};

enum { FILL_NONE = 0, FILL_SHADE, FILL_PATTERN };

#define OBF_HIDDEN  1
#define OBF_LOCKED  2

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

	char		name[OB_NAME_MAX];
	char		*action;		// a DSL form, kept for later phases
	char		*script;
} obj_t;

// -- cards ---------------------------------------------------------------

enum { TR_CUT = 0, TR_WIPE, TR_PUSH, TR_KINDS };

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
	char		*script;
} card_t;

#define DECK_FONTS_MAX 32
#define DECK_TITLE_MAX 64
#define DECK_PRINT_MAX 64

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
	bool		app;			// opens to run, not to edit (later phases)
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

// Bounding box, units (a line's ends ordered).
void obj_bounds(const obj_t *o, int *x0, int *y0, int *x1, int *y1);
void obj_move(obj_t *o, int dx, int dy);
// New box; polygons scale their points into it.
void obj_set_box(obj_t *o, int x, int y, int w, int h);

#endif
