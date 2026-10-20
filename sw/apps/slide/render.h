#ifndef SLIDE_RENDER_H
#define SLIDE_RENDER_H

/*
 * slide -- drawing a card through zcanvas, at any scale: the editor's
 * view, a show's full screen, a printed page (docs/slide_app.md,
 * "Drawing a card"). Fonts and images are loaded here, on demand.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zcanvas.h"
#include "../../common/zcfont.h"
#include "deck.h"

enum { RENDER_EDIT = 0, RENDER_SHOW, RENDER_PRINT };

// Reads a whole file into the pool (pool_free() it), NULL if it cannot.
// The app's and the tests' own.
typedef uint8_t *(*render_load_fn)(const char *path, uint32_t *len);

// `scratch` holds zcanvas's edge table and the working paths.
void render_init(void *scratch, uint32_t bytes, render_load_fn load);

// Where relative image paths start: the deck's folder ("" for none).
void render_set_dir(const char *dir);

// The card, its background first, into t (its clip says which part).
// The page is painted white first.
void render_card(zc_target_t *t, const zc_xf_t *xf, const deck_t *d,
	const card_t *c, int mode);

// Called between objects while a card is drawn (NULL: nothing): the
// show draws the next card in a coroutine (zco.h) that hands back
// there, so a key never waits for a whole card.
void render_set_between(void (*fn)(void));
void render_obj(zc_target_t *t, const zc_xf_t *xf, const deck_t *d,
	const obj_t *o, int mode);

// -- text layout, for the editor's caret ---------------------------------

#define RENDER_MAX_LINES 128

typedef struct {
	zc_style_t	st;
	zc_line_t	line[RENDER_MAX_LINES];
	int			n;
	zc_fix		line_h, asc;
	zc_fix		top;			// first baseline's top, units
	zc_fix		x0, w;			// the content box across, units
} text_layout_t;

void render_text_layout(const deck_t *d, const obj_t *o, text_layout_t *L);
// x (units) of the start of line i
zc_fix render_line_x(const obj_t *o, const text_layout_t *L, int i);

// What an object can ink, in units: its box and what can run past it
// (strokes, arrowheads, text). Objects outside the target's clip are
// not drawn at all.
void render_ink_bounds(const deck_t *d, const obj_t *o, int *x0, int *y0, int *x1, int *y1);

// The style text in this object draws with (the face chosen for its
// bold/italic, the size).
void render_style(const deck_t *d, const obj_t *o, zc_style_t *st);

// -- fonts ---------------------------------------------------------------

// The font for a name: loaded from /sys/fonts the first time, or one of
// the built-in "mono5x8", "mono6x12"; a missing font falls back to sans,
// and without sans to mono6x12.
const zc_font_t *render_font_named(const char *name);
// Whether a face of this name can be had (for the font command).
bool render_font_exists(const char *name);

// -- images ----------------------------------------------------------------

// Drops an object's image cache (it was deleted, or its source changed);
// a load of it stops.
void render_forget(obj_t *o);
// Natural size of an object's image, false until it is loaded.
bool render_image_size(obj_t *o, int *w, int *h);

// Pictures load a slice at a time, never inside a drawing call
// (docs/responsive.md). Drawing one not loaded yet queues it and draws
// its frame with a progress bar; the app's main loop runs
// render_load_step() when it has no message waiting.
bool render_loading(void);
// Works for about `ticks`; returns the picture worked on (to redraw:
// its progress bar, or itself when *finished), or NULL with nothing to do.
obj_t *render_load_step(uint32_t ticks, bool *finished);
// How far the picture being loaded has got, 0..1000, and its file name
// (good until the next render_load_step()).
uint16_t render_load_progress(const char **name);
// Stops loading now: the picture being loaded and those waiting. Returns
// how many were stopped.
int render_load_stop(void);
// Loads a stopped or failed picture again.
bool render_load_retry(obj_t *o);

enum { RENDER_IMG_NONE = 0, RENDER_IMG_WAITING, RENDER_IMG_READY, RENDER_IMG_FAILED,
	RENDER_IMG_STOPPED };
int render_image_state(const obj_t *o);
// -- controls --
// The control drawn pressed (a button held down) and the field being
// typed into (a caret after its text); NULL for none.
void render_ui_state(const obj_t *pressed, const obj_t *focus);
// A list's items (its text, a line each) and row height in units.
int render_list_items(const obj_t *o);
bool render_list_item(const obj_t *o, int i, const char **s, uint32_t *len);
int render_list_row_h(const deck_t *d, const obj_t *o);
// A slider: its knob's centre across (units), and the value at x.
int render_slider_x(const obj_t *o);
int render_slider_value(const obj_t *o, int x);

// Asks for a card's pictures (its background's too) without drawing it:
// the show asks for the next card's while this one is up. `first` puts
// them at the front of the queue (the card the person is looking at).
void render_card_want(const deck_t *d, const card_t *c, int mode, bool first);
// Whether o is on card c (or its background): is a finished picture
// worth redrawing for.
bool render_card_has(const deck_t *d, const card_t *c, const obj_t *o);
// Lets go of the pictures kept for objects re-made by undo, paste and
// the like (another deck is opened, or memory is short).
void render_drop_recent(void);

// Arrowhead length for a line of width w (units).
int render_arrow_len(int w);

#endif
