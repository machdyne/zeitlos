#ifndef ZWIDGET_H
#define ZWIDGET_H

/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * A small immediate-hit-test / retained-state widget toolkit for apps
 * that have furniture in their windows: push buttons, toggles, radio
 * groups, pattern swatches, sliders.
 *
 * -- what this is and isn't --
 *
 * It is NOT a general UI framework. There is no layout engine, no
 * event bubbling, no widget tree, no focus chain. A widget set is a
 * flat array the app declares itself, positioned in content-relative
 * coordinates the app computed itself. That is deliberate: the
 * first two consumers (draw's tool column and pattern palette) both
 * want fixed grids they lay out arithmetically, and a layout engine
 * would be more code than the thing it replaces.
 *
 * What it does provide is the part that is genuinely annoying to
 * write twice: hit testing, pressed/selected state, radio-group
 * exclusivity, dirty tracking so only changed widgets repaint, and
 * drawing that goes through the GPU rather than per-pixel loops.
 *
 * -- hardware use --
 *
 * Widget bodies, frames, fills and slider tracks all go through the
 * blitter and line rasterizer (z_fb_hw_fill_rect(),
 * z_fb_hw_fill_pattern(), z_win_hw_box(), zgfx.h). The one thing that
 * does NOT is icon bitmaps, which are drawn per-pixel in software.
 *
 * That is not an oversight, so it's worth stating why: the hardware
 * glyph blitter can only draw what is currently in glyph memory
 * (rtl/mem/glyph.v), that memory holds exactly one font's data at a
 * time, and by board-wide convention wm is the ONLY process that ever
 * writes to it (see docs/window_manager.md, "Hardware glyph
 * blitting"). An app blitting its own icons would have to load them
 * there, which breaks that convention and reintroduces exactly the
 * cross-process race it exists to prevent. Icons are 16x16 and
 * redrawn only when a widget's state actually changes, so software is
 * genuinely fine here -- this is the same tradeoff wm itself already
 * makes for the dock's own 32x32 icons.
 *
 * Text labels DO go through the hardware path, via z_win_draw_text()
 * -- they're ordinary glyphs from the font wm already loaded.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zwin.h"
#include "zgfx.h"
#include "zfont.h"

typedef enum {
	// momentary push button -- reports an activation on release,
	// inside its own bounds, the way a button should (so dragging
	// off it cancels).
	Z_WIDGET_BUTTON = 0,
	// on/off. With group != 0 it's a radio member instead: selecting
	// it clears every other widget sharing that group.
	Z_WIDGET_TOGGLE,
	// an 8x8 fill pattern sample. Always a radio member -- a palette
	// with nothing selected has no meaning.
	Z_WIDGET_SWATCH,
	// horizontal slider over [vmin, vmax]. Reports activation
	// continuously while dragged, not just on release, since the
	// point of a slider is live feedback.
	Z_WIDGET_SLIDER,
} z_widget_type_t;

typedef struct {

	z_widget_type_t	type;

	// content-relative, matching z_win_draw_text()/z_win_fill_rect()
	// -- (0,0) is the top-left of the window's content area, not the
	// top-left of the window.
	int16_t		x, y, w, h;

	// radio group id. 0 means "not a radio member" (a plain toggle
	// that flips independently). Any other value groups this widget
	// with every other widget in the same set sharing that value, of
	// which exactly one is selected at a time.
	uint8_t		group;

	// selected/on. For a radio member this is the group's selection.
	bool		on;

	bool		enabled;

	// -- per-type payload. Kept as plain sibling fields rather than a
	// union: the space saved would be a handful of bytes per widget,
	// and a union here means every access needs to be sure which arm
	// is live, which is exactly the kind of footgun this toolkit is
	// too small to be worth.

	// BUTTON/TOGGLE: text drawn centered, or NULL. Always drawn with
	// z_font_5x8 -- see the header comment on why an app doesn't get
	// to choose a font.
	const char	*label;

	// BUTTON/TOGGLE: 16x16 1bpp icon, 2 bytes per row, MSB-first --
	// the same convention as zicon.h and z_font_t glyph data. NULL
	// for a text-only widget. If both label and icon are set, the
	// icon wins (there is no room for both in a 16x16-plus-frame
	// button).
	const uint8_t	*icon;

	// SWATCH: 8 bytes, one per pattern row, MSB-first -- the format
	// z_fb_hw_fill_pattern() (zgfx.h) takes. Z_PATTERN_* below are
	// ready to use here.
	const uint8_t	*pattern;

	// SLIDER: current value and its inclusive bounds.
	int16_t		value, vmin, vmax;

	// set by any state change, cleared by z_widget_draw(). Lets the
	// app repaint only what actually changed instead of the whole
	// panel -- which matters more than it sounds like, since a full
	// panel repaint is visible as a flash.
	bool		dirty;

} z_widget_t;

typedef struct {

	z_widget_t	*items;
	int		count;
	const z_win_t	*win;

	// index of the widget the pointer went down on, or -1. Held
	// across calls so a press that drags off its widget and back
	// still works, and so a release outside cancels rather than
	// activating something the user deliberately slid away from.
	int		pressed;

	// previous button mask, for edge detection inside
	// z_widget_mouse().
	uint8_t		last_buttons;

	// index of the keyboard-focused widget, or -1 for none.
	//
	// Keyboard-only operation is a first-class case in this system
	// (see docs/window_manager.md), and a set of buttons you can only
	// reach with a pointer is a dead end for anyone without one. The
	// focused widget draws a ring and is what z_widget_key_activate()
	// acts on.
	//
	// -1 by default, so an app that never calls the focus functions
	// below behaves exactly as it did before they existed -- nothing
	// draws a ring and nothing changes.
	int			focused;

	// Set once this set has ever had keyboard focus. Until then the
	// focus ring is not drawn OR erased, so a set that only uses the
	// pointer is left exactly as it was before focus existed --
	// which matters for a grid whose widgets abut, where erasing a
	// ring would rub out the neighbour's frame. A set that uses
	// focus needs at least 2px between widgets.
	bool		focus_used;

} z_widget_set_t;

// -- ready-made 8x8 patterns, MSB-first rows --
//
// A deliberately small, early-MacPaint-flavored set: solids, greys at
// a few densities, lines and a couple of textures. Extern rather than
// macros so they can be put straight into a z_widget_t.pattern or
// handed to z_fb_hw_fill_pattern() without the app copying anything.
#define Z_PATTERN_COUNT 16
extern const uint8_t z_pattern_table[Z_PATTERN_COUNT][8];

// convenience names for the two everything else is defined against
#define Z_PATTERN_WHITE  (z_pattern_table[0])
#define Z_PATTERN_BLACK  (z_pattern_table[1])

// -- setup --

// binds `items` (an array the app owns and keeps alive) to `win`.
// Does not copy or allocate.
void z_widget_set_init(z_widget_set_t *set, z_widget_t *items, int count,
	const z_win_t *win);

// marks every widget dirty -- call after a resize or any time the
// window's content was cleared underneath the widgets, so the next
// z_widget_draw_all() actually repaints rather than skipping
// everything as unchanged.
void z_widget_invalidate(z_widget_set_t *set);

// -- drawing --

// draws one widget and clears its dirty flag.
void z_widget_draw(z_widget_set_t *set, int idx);

// draws every widget whose dirty flag is set. `force` draws all of
// them regardless -- equivalent to z_widget_invalidate() followed by
// this, kept as a parameter because a full redraw is the common case
// right after a Z_WM_REDRAW.
void z_widget_draw_all(z_widget_set_t *set, bool force);

// -- input --

// feeds one pointer sample in (cx,cy content-relative, `buttons` from
// Z_WM_UNPACK_MOUSE_BUTTONS) and returns the index of the widget
// ACTIVATED by it, or -1 for none.
//
// "Activated" means: a button released inside the bounds it was
// pressed in, a toggle or swatch selected, or a slider moved. The app
// is expected to call this once per pointer sample and then act on a
// non-negative result -- see sw/apps/draw for the pattern.
//
// Redraws whatever changed as a side effect, so the caller doesn't
// have to chase pressed/selected repaints itself.
int z_widget_mouse(z_widget_set_t *set, int cx, int cy, uint8_t buttons);

// returns the index of the selected member of `group`, or -1. For
// reading which tool/pattern is current without the app tracking it
// separately (and thereby getting to disagree with what's on screen).
int z_widget_group_selection(const z_widget_set_t *set, uint8_t group);

// selects `idx`, clearing any other member of its group. Marks
// everything it changed dirty but does not draw -- pair it with
// z_widget_draw_all(set, false) to get the repaint.
void z_widget_select(z_widget_set_t *set, int idx);

// -- keyboard focus --
//
// An app drives these from its own key handler; the toolkit has no
// idea which key means "next". See sw/apps/files for the pattern,
// including how a set of buttons shares one tab order with a widget
// that isn't part of the set at all.

// Moves focus to the next (or previous) enabled widget, wrapping.
// From -1, forward lands on the first and backward on the last.
//
// Returns the new focus index. Marks what changed dirty; pair with
// z_widget_draw_all(set, false).
int z_widget_focus_next(z_widget_set_t *set, bool backward);

// Sets focus directly, or clears it with -1.
void z_widget_focus_set(z_widget_set_t *set, int idx);

// Activates the focused widget, exactly as a click on it would --
// toggles a toggle, selects a radio member, reports a button.
// Returns the activated index, or -1 if nothing has focus.
//
// The caller decides which keys reach this. Enter and Space are the
// conventional pair.
int z_widget_key_activate(z_widget_set_t *set);

// ---------------------------------------------------------------
// scrollbars
// ---------------------------------------------------------------

/*
 * A scrollbar is its own type rather than another z_widget_type_t,
 * for two concrete reasons rather than tidiness:
 *
 *   - RANGE. z_widget_t's value/vmin/vmax are int16_t, which is
 *     plenty for a slider over a handful of settings and not enough
 *     here. A 32KB text buffer of short lines wraps to more lines
 *     than an int16_t can index, and a scrollbar that silently wraps
 *     around at 32767 is a genuinely nasty bug to find. These are
 *     int32_t.
 *
 *   - INTERACTION. z_widget_mouse() answers "which widget did this
 *     activate", which is the right question for buttons and
 *     swatches and the wrong one for a scrollbar: what a caller needs
 *     to know is whether the VALUE changed, and a scrollbar's hit
 *     regions (the thumb, and the trough either side of it) do
 *     different things to it. Squeezing that into the activation-
 *     index model would mean the caller re-deriving from coordinates
 *     what this already knows.
 *
 * A scrollbar owns no memory and allocates nothing. It draws through
 * the blitter like everything else here.
 *
 * Visually it is deliberately minimal: a thumb, and nothing else. No
 * step arrows (there is no scroll wheel in this system either, so the
 * mouse-only granularity is a page -- but arrows are three extra hit
 * regions and a pair of drawn triangles for something the keyboard
 * already does better), and no trough frame. When there is nothing to
 * scroll, it draws NOTHING rather than a full-length thumb, which is
 * both quieter and unambiguous.
 *
 * The horizontal orientation is fully implemented even though the
 * first caller (sw/apps/text) only uses the vertical one -- see
 * sw/apps/draw's own "no scrolling yet" note for the app that wants
 * both, and docs/widgets.md.
 */

typedef enum {
	Z_SB_VERT = 0,
	Z_SB_HORZ,
} z_sb_orient_t;

// Thickness across the short axis -- the width of a vertical
// scrollbar, the height of a horizontal one. A click target rather
// than a visual weight: the thumb itself is drawn narrower than this
// (see z_scrollbar_draw()), and the rest is slack so the bar is
// grabbable without careful aim.
#define Z_SB_THICK   12

typedef struct {

	const z_win_t	*win;
	z_sb_orient_t	orient;

	// content-relative position, and length along the LONG axis --
	// the short axis is always Z_SB_THICK, so there's no way to
	// express a scrollbar of the wrong thickness.
	//
	// On a RESIZABLE window, a scrollbar down the right edge must
	// stop short of the resize grip or it swallows it and the window
	// can't be resized any more:
	//
	//     z_scrollbar_set_geom(&sb, cw - Z_SB_THICK, 0,
	//         ch - Z_WIN_GRIP_INSET);
	//
	// See Z_WIN_GRIP_INSET in zwm.h. A fixed-size window has no grip
	// and can use the full height.
	int16_t		x, y, len;

	// the scrolled document. `total` is its full extent, `page` how
	// much of it is visible at once, both in whatever unit the
	// caller likes (text uses wrapped lines; a picture would use
	// pixels). value is the first visible unit, clamped to
	// [0, total - page].
	int32_t		total, page, value;

	// -- interaction state, owned by z_scrollbar_mouse() --
	bool		dragging;
	// where in the thumb the pointer grabbed it, in pixels along the
	// long axis. Held so the thumb doesn't jump to center itself
	// under the cursor on the first pixel of movement -- the same
	// reasoning wm.c's own drag_off_x/resize_off_x have.
	int32_t		drag_grab;
	uint8_t		last_buttons;

	bool		dirty;

} z_scrollbar_t;

// binds a scrollbar to a window. Geometry and range are set
// separately (below) since both typically change on a resize while
// this doesn't.
void z_scrollbar_init(z_scrollbar_t *sb, const z_win_t *win, z_sb_orient_t o);

// content-relative position and length along the long axis. Call from
// the app's own layout() on every resize.
void z_scrollbar_set_geom(z_scrollbar_t *sb, int x, int y, int len);

// sets the document extent and visible amount, re-clamping value to
// suit. Returns true if value had to change as a result -- which the
// caller needs to know, since it means the view has to be redrawn as
// well as the scrollbar (e.g. deleting the last page of a document
// scrolls the view up whether the user asked or not).
bool z_scrollbar_set_range(z_scrollbar_t *sb, int32_t total, int32_t page);

// sets the scroll position, clamped. Returns true if it changed.
bool z_scrollbar_set_value(z_scrollbar_t *sb, int32_t v);

// draws the scrollbar if it's dirty, or unconditionally with force --
// same contract, and the same reasoning, as z_widget_draw_all().
void z_scrollbar_draw(z_scrollbar_t *sb, bool force);

// feeds one pointer sample in (content-relative, `buttons` from
// Z_WM_UNPACK_MOUSE_BUTTONS). Returns true if the value changed, so
// the caller can redraw its view. Repaints the scrollbar itself as
// needed.
//
// Note this returns false for a click that HIT the scrollbar but
// didn't move it (pressing the thumb, say). Use
// z_scrollbar_has_pointer() below to tell "not mine" from "mine, no
// change" -- a caller that treats a returned false as "not mine"
// would start selecting text the moment the user grabbed the thumb.
bool z_scrollbar_mouse(z_scrollbar_t *sb, int cx, int cy, uint8_t buttons);

// true while this scrollbar owns the pointer (a drag it started is
// still in progress), or if the given point is inside its rect.
bool z_scrollbar_has_pointer(const z_scrollbar_t *sb, int cx, int cy);

#endif
