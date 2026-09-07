#ifndef TOOLBAR_H
#define TOOLBAR_H

#include <stdbool.h>

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Where the browser toolbar's pieces sit, and what a click at a given
 * point hits.
 *
 * -- why this is its own file --
 *
 * It is arithmetic with no dependencies -- no window, no framebuffer,
 * no fonts -- which means it can be TESTED, and toolbar geometry is
 * exactly the kind of code that is wrong in ways nobody notices: a
 * button whose drawn rectangle and whose clickable rectangle differ
 * by two pixels works perfectly except at the edges.
 *
 * Both the drawing and the hit testing in web.c call this, so a
 * button that LOOKS pressed and a button that IS pressed are the same
 * button by construction rather than by both being updated together.
 *
 * Everything is content-relative, matching z_win_draw_text() and the
 * coordinates a mouse event arrives in.
 */

#define TB_BTN_W    18		// square-ish at the bar height below
#define TB_BTN_GAP  2
#define TB_MARGIN   2

// Bar height: room for a framed field. zedit.c draws a one-pixel
// border and pads by three, so a bare 8+4 left the glyphs touching
// the frame.
#define TB_H        (8 + 8)

// The narrowest field worth drawing. Below this the buttons are
// dropped instead, because a two-character URL box is not a thing
// anyone can use and the buttons still are.
#define TB_MIN_FIELD 40

typedef struct {
	int		field_x, field_w;
	int		back_x;
	int		fwd_x;
	bool	buttons;		// false when the window is too narrow
} toolbar_t;

void toolbar_geom(int content_w, toolbar_t *tb);

typedef enum {
	TB_HIT_NONE = 0,
	TB_HIT_FIELD,
	TB_HIT_BACK,
	TB_HIT_FORWARD,
} toolbar_hit_t;

// `x`/`y` are content-relative. A click below the bar is TB_HIT_NONE,
// so the caller can pass every click through without pre-filtering.
toolbar_hit_t toolbar_hit(const toolbar_t *tb, int x, int y);

// -- the button glyphs --
//
// Is pixel (dx, dy) inside the arrow, for a button of the given size?
//
// A pure function, and separate from the drawing, because the first
// version of this was a loop that computed columns and half-heights
// inline and drew NOTHING -- and nothing about reading it said so.
// The shape is testable this way; the drawing is then three lines
// that cannot be wrong on their own.
//
// `dx`/`dy` are relative to the button's top-left corner.
bool toolbar_arrow_px(int dx, int dy, int w, int h, bool left);

// Whether a pixel of a DISABLED arrow should be drawn.
//
// There is no grey on one bit, so a disabled arrow is stippled: every
// other pixel, which reads as faint next to a solid one. It is still
// an arrow.
//
// It used to be drawn as nothing at all, leaving an empty frame --
// which does not say "you cannot go back", it says "this button is
// broken".
bool toolbar_stipple(int dx, int dy);

#endif
