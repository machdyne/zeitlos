/*
 * Zeitlos
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Widget toolkit. See zwidget.h for what this is, what it isn't, and
 * why icon drawing is the one thing here that isn't hardware
 * accelerated.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "zeitlos.h"
#include "zwm.h"
#include "zwin.h"
#include "zgfx.h"
#include "zfont.h"
#include "zwidget.h"
#include "zkbd.h"		// Z_KEY_*, for the list box
#include "zsoc.h"		// Z_TICK_HZ, for its type-to-find timeout

// -- pattern table --
//
// Row-per-byte, MSB-first, so each literal below reads as a picture of
// its own 8x8 tile. Ordered roughly light-to-dark for the first half
// (which is what makes a palette laid out in order look like a ramp
// rather than a jumble), then lines and textures.
const uint8_t z_pattern_table[Z_PATTERN_COUNT][8] = {

	// 0: white / empty
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	// 1: black / solid
	{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
	// 2: 1/16 dots -- the lightest useful grey
	{ 0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00 },
	// 3: 1/8 dots
	{ 0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00 },
	// 4: 25% grey
	{ 0xAA, 0x00, 0x55, 0x00, 0xAA, 0x00, 0x55, 0x00 },
	// 5: 50% checker
	{ 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55 },
	// 6: 75% grey
	{ 0x55, 0xFF, 0xAA, 0xFF, 0x55, 0xFF, 0xAA, 0xFF },
	// 7: horizontal lines
	{ 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00 },
	// 8: vertical lines
	{ 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88 },
	// 9: diagonal, rising
	{ 0x11, 0x22, 0x44, 0x88, 0x11, 0x22, 0x44, 0x88 },
	// 10: diagonal, falling
	{ 0x88, 0x44, 0x22, 0x11, 0x88, 0x44, 0x22, 0x11 },
	// 11: cross-hatch
	{ 0xFF, 0x88, 0x88, 0x88, 0xFF, 0x88, 0x88, 0x88 },
	// 12: fine diagonal mesh
	{ 0x99, 0x66, 0x66, 0x99, 0x99, 0x66, 0x66, 0x99 },
	// 13: bricks
	{ 0xFF, 0x80, 0x80, 0x80, 0xFF, 0x08, 0x08, 0x08 },
	// 14: weave
	{ 0xE8, 0x8E, 0x83, 0x38, 0x8E, 0xE8, 0x38, 0x83 },
	// 15: scales
	{ 0x18, 0x24, 0x42, 0x81, 0x81, 0x42, 0x24, 0x18 },

};

// -- helpers --

// Widget bodies/frames are drawn in ABSOLUTE screen coordinates
// (z_win_hw_box()/z_fb_hw_fill_pattern() both take absolute), while
// widget rects are stored content-relative. This converts, once, in
// one place -- the alternative is every draw function doing its own
// addition and one of them eventually forgetting.
static void widget_abs(const z_widget_set_t *set, const z_widget_t *wd,
	int *x0, int *y0, int *x1, int *y1) {

	z_clip_t clip;
	z_win_content_rect(set->win, &clip);

	*x0 = clip.x0 + wd->x;
	*y0 = clip.y0 + wd->y;
	*x1 = *x0 + wd->w - 1;
	*y1 = *y0 + wd->h - 1;

}

// true if a content-relative point is inside this widget
static bool widget_contains(const z_widget_t *wd, int cx, int cy) {
	return cx >= wd->x && cx < wd->x + wd->w &&
		cy >= wd->y && cy < wd->y + wd->h;
}

// blits a 16x16 1bpp icon at absolute (x0,y0).
//
// `ink` is the pixel value written where an icon bit is SET; bits that
// are clear are left alone rather than written as background. That
// matters for the selected state: the body underneath has already been
// filled solid, and drawing the icon with ink=0 punches its shape back
// out of that fill, giving an inverted icon for free. Same technique
// wm's own draw_icon_bitmap_inverted() uses for the dock.
//
// Software, per pixel -- see zwidget.h's header comment for why this
// specifically doesn't go through the glyph blitter.
static void widget_draw_icon(int x0, int y0, const uint8_t *icon, int ink,
	const z_clip_t *clip) {

	for (int row = 0; row < 16; row++) {
		for (int col = 0; col < 16; col++) {
			uint8_t byte = icon[row * 2 + (col >> 3)];
			if (byte & (0x80u >> (col & 7)))
				z_fb_set_pixel(x0 + col, y0 + row, ink, clip);
		}
	}

}

// -- setup --

void z_widget_set_init(z_widget_set_t *set, z_widget_t *items, int count,
	const z_win_t *win) {

	set->items = items;
	set->count = count;
	set->win = win;
	set->pressed = -1;
	set->last_buttons = 0;
	set->focused = -1;
	set->focus_used = false;

	for (int i = 0; i < count; i++)
		items[i].dirty = true;

}

void z_widget_invalidate(z_widget_set_t *set) {
	for (int i = 0; i < set->count; i++)
		set->items[i].dirty = true;
}

// -- drawing --

void z_widget_draw(z_widget_set_t *set, int idx) {

	if (idx < 0 || idx >= set->count) return;

	z_widget_t *wd = &set->items[idx];

	int x0, y0, x1, y1;
	widget_abs(set, wd, &x0, &y0, &x1, &y1);

	z_clip_t clip;
	z_win_content_rect(set->win, &clip);

	// A widget can be entirely outside the content area after a
	// shrink -- draw nothing rather than relying on every individual
	// primitive below to clip it away. z_fb_hw_fill_rect() in
	// particular clamps to the SCREEN, not to the window, so an
	// out-of-window widget would otherwise paint over whatever else
	// is there.
	if (x1 < clip.x0 || x0 > clip.x1 || y1 < clip.y0 || y0 > clip.y1) {
		wd->dirty = false;
		return;
	}

	// clamp the body fill to the content rect ourselves, for the same
	// reason -- this is the one primitive here that doesn't take a
	// clip.
	int fx0 = x0 < clip.x0 ? clip.x0 : x0;
	int fy0 = y0 < clip.y0 ? clip.y0 : y0;
	int fx1 = x1 > clip.x1 ? clip.x1 : x1;
	int fy1 = y1 > clip.y1 ? clip.y1 : y1;

	switch (wd->type) {

		case Z_WIDGET_SWATCH: {

			// the pattern itself fills the body, so the swatch shows
			// what it will actually paint with.
			z_fb_hw_fill_pattern(fx0 + 1, fy0 + 1,
				fx1 - fx0 - 1, fy1 - fy0 - 1,
				wd->pattern ? wd->pattern : Z_PATTERN_WHITE);

			// selection is a frame, not an inversion -- inverting a
			// pattern swatch would show a DIFFERENT pattern, which is
			// actively misleading in a palette whose whole job is
			// showing you what you're about to get.
			z_win_hw_box(set->win, x0, y0, x1, y1, wd->on ? 1 : 0);
			if (wd->on)
				z_win_hw_box(set->win, x0 + 1, y0 + 1, x1 - 1, y1 - 1, 1);

			break;

		}

		case Z_WIDGET_SLIDER: {

			z_fb_hw_fill_rect(fx0, fy0, fx1 - fx0 + 1, fy1 - fy0 + 1, 0);

			// track
			int mid = (y0 + y1) / 2;
			z_win_hw_line(set->win, x0 + 2, mid, x1 - 2, mid, 1);

			// knob position. vmax == vmin would divide by zero, and a
			// degenerate slider is a plausible thing to construct by
			// accident (two constants that happen to be equal), so
			// pin it to the left rather than trapping.
			int span = wd->vmax - wd->vmin;
			int travel = (x1 - 2) - (x0 + 2) - 6;
			int kx = x0 + 2;
			if (span > 0 && travel > 0)
				kx += ((wd->value - wd->vmin) * travel) / span;

			z_fb_hw_fill_rect(kx, fy0 + 1, 7, fy1 - fy0 - 1, 1);

			break;

		}

		case Z_WIDGET_BUTTON:
		case Z_WIDGET_TOGGLE:
		default: {

			// A disabled widget is never lit, whatever its `on`
			// says. Everything else here already refuses to touch
			// one -- widget_hit() skips it, so the pointer cannot
			// press it; z_widget_focus_next() skips it, so Tab
			// cannot reach it; z_widget_key_activate() returns -1
			// for it. Only the DRAWING did not know, so a disabled
			// button was pixel-for-pixel identical to a live one
			// and simply did nothing when clicked.
			//
			// That was latent rather than broken -- nothing in the
			// tree set enabled = false until sw/apps/cal's month
			// arrows, which are disabled at the ends of the
			// representable date range and reach that state on the
			// very first launch on a machine with no clock (the
			// app opens on the epoch, where `<` has nowhere to
			// go). A dead-looking button is the point; a live one
			// that ignores you is a bug report.
			bool lit = wd->enabled && (wd->on || (set->pressed == idx));

			// body: solid when lit, empty when not. The pressed state
			// borrows the same visual as "on" deliberately -- with one
			// bit of colour there is no third appearance available,
			// and "held down looks like selected" is both the honest
			// reading and what the original Macintosh did.
			z_fb_hw_fill_rect(fx0 + 1, fy0 + 1,
				fx1 - fx0 - 1, fy1 - fy0 - 1, lit ? 1 : 0);

			z_win_hw_box(set->win, x0, y0, x1, y1, 1);

			// Keyboard focus ring: a second box drawn one pixel
			// OUTSIDE the widget's own frame, the same visual
			// language wm uses for the focused window (see
			// draw_window_box()). Outside rather than inside so it
			// never eats into the label's space, which at this size
			// would be the difference between a readable button and
			// a cramped one.
			//
			// Drawn on EVERY redraw, in ink when focused and in
			// background when not. Only drawing it when focused left
			// the old ring on screen when focus moved on, because a
			// redraw of the losing widget repaints its body and
			// frame but never touches the pixels outside them -- so
			// rings accumulated on every widget that had ever been
			// focused, and focus appeared never to leave anything.
			//
			// Gated on focus_used so a set that never uses keyboard
			// focus is untouched. That matters: the ring sits one
			// pixel outside the widget, and in a grid where widgets
			// ABUT (sw/apps/draw's tool column, 20px cells with no
			// gap) erasing it would rub out the neighbour's frame.
			// Any set that does use focus must leave at least 2px
			// between widgets.
			if (set->focus_used)
				z_win_hw_box(set->win, x0 - 1, y0 - 1, x1 + 1, y1 + 1,
					set->focused == idx ? 1 : 0);

			int ink = lit ? 0 : 1;

			// An empty frame is the disabled appearance.
			//
			// 1bpp leaves no room for the usual answer. Greying a
			// label needs a per-pixel AND against a 50% pattern,
			// and neither the blitter nor z_fb_hw_fill_pattern()
			// does a masked write -- the pattern fill OVERWRITES,
			// so it would erase the button rather than dim it.
			// Drawing the label anyway and hoping the user notices
			// it does nothing is the option this replaces.
			//
			// So: keep the frame, which holds the layout and shows
			// something is there, and drop the contents. It reads
			// as unavailable at a glance and costs no new drawing
			// primitive.
			if (!wd->enabled) break;

			if (wd->icon) {

				widget_draw_icon(x0 + (wd->w - 16) / 2,
					y0 + (wd->h - 16) / 2, wd->icon, ink, &clip);

			} else if (wd->label) {

				// z_fb_draw_text2(), not z_win_draw_text().
				//
				// z_fb_draw_char() hardcodes its background to 0
				// (see zgfx.c) and exposes only the foreground. So
				// drawing a label on a LIT body -- where the ink has
				// to be 0 to show against a filled background --
				// asked for fg=0 with bg=0, and every glyph cell
				// came out solid background. The label did not go
				// dim or misalign; it vanished, leaving a block
				// where the text should be.
				//
				// That only showed up once a widget stayed lit with
				// a label on it. A push button is lit just while
				// it is held, so the label blinking out for that
				// moment read as a press effect; a selected radio
				// member is lit permanently, and then it is simply
				// broken. z_fb_draw_text2() has both colours.
				int tw = (int)strlen(wd->label) * z_font_5x8.w;

				z_fb_draw_text2(
					clip.x0 + wd->x + (wd->w - tw) / 2,
					clip.y0 + wd->y + (wd->h - z_font_5x8.h) / 2,
					wd->label, ink, lit ? 1 : 0, &z_font_5x8, &clip);

			}

			break;

		}

	}

	wd->dirty = false;

}

void z_widget_draw_all(z_widget_set_t *set, bool force) {

	for (int i = 0; i < set->count; i++)
		if (force || set->items[i].dirty)
			z_widget_draw(set, i);

}

// -- input --

int z_widget_group_selection(const z_widget_set_t *set, uint8_t group) {

	if (!group) return -1;

	for (int i = 0; i < set->count; i++)
		if (set->items[i].group == group && set->items[i].on)
			return i;

	return -1;

}

void z_widget_select(z_widget_set_t *set, int idx) {

	if (idx < 0 || idx >= set->count) return;

	uint8_t group = set->items[idx].group;

	if (group) {
		for (int i = 0; i < set->count; i++) {
			if (i == idx) continue;
			if (set->items[i].group != group) continue;
			if (!set->items[i].on) continue;
			set->items[i].on = false;
			set->items[i].dirty = true;
		}
	}

	if (!set->items[idx].on) {
		set->items[idx].on = true;
		set->items[idx].dirty = true;
	}

}

void z_widget_focus_set(z_widget_set_t *set, int idx) {

	if (idx < -1 || idx >= set->count) return;

	// Sticky: once a set has had focus, its widgets keep managing
	// their rings even after focus is cleared to -1. Otherwise
	// clearing focus would leave the last ring drawn forever.
	set->focus_used = true;

	if (set->focused == idx) return;

	// Both the old and the new widget have to repaint: one loses a
	// ring, the other gains one.
	if (set->focused >= 0) set->items[set->focused].dirty = true;

	set->focused = idx;

	if (idx >= 0) set->items[idx].dirty = true;

}

int z_widget_focus_next(z_widget_set_t *set, bool backward) {

	if (set->count < 1) return -1;

	// Walk at most count steps, so a set with nothing enabled
	// terminates instead of spinning.
	int idx = set->focused;

	for (int i = 0; i < set->count; i++) {

		if (idx < 0)
			idx = backward ? set->count - 1 : 0;
		else
			idx = backward ? (idx + set->count - 1) % set->count
			               : (idx + 1) % set->count;

		if (set->items[idx].enabled) {
			z_widget_focus_set(set, idx);
			return idx;
		}

	}

	return set->focused;

}

int z_widget_key_activate(z_widget_set_t *set) {

	int idx = set->focused;

	if (idx < 0 || idx >= set->count) return -1;
	if (!set->items[idx].enabled) return -1;

	z_widget_t *wd = &set->items[idx];

	// Exactly what a release inside the widget does in
	// z_widget_mouse() -- deliberately the same behaviour, so a
	// keyboard user and a mouse user get the same result from the
	// same widget rather than two implementations that drift.
	switch (wd->type) {

		case Z_WIDGET_TOGGLE:
		case Z_WIDGET_SWATCH:
			if (wd->group) z_widget_select(set, idx);
			else { wd->on = !wd->on; wd->dirty = true; }
			break;

		case Z_WIDGET_BUTTON:
		default:
			wd->dirty = true;
			break;

	}

	z_widget_draw_all(set, false);

	return idx;

}

// finds the widget under a content-relative point, or -1
static int widget_hit(const z_widget_set_t *set, int cx, int cy) {

	for (int i = 0; i < set->count; i++) {
		if (!set->items[i].enabled) continue;
		if (widget_contains(&set->items[i], cx, cy)) return i;
	}

	return -1;

}

// maps a content-relative x onto a slider's value range
static int16_t slider_value_at(const z_widget_t *wd, int cx) {

	int span = wd->vmax - wd->vmin;
	int travel = wd->w - 4 - 6;

	if (span <= 0 || travel <= 0) return wd->vmin;

	int rel = cx - (wd->x + 2) - 3;
	if (rel < 0) rel = 0;
	if (rel > travel) rel = travel;

	return (int16_t)(wd->vmin + (rel * span) / travel);

}

int z_widget_mouse(z_widget_set_t *set, int cx, int cy, uint8_t buttons) {

	bool down = (buttons & Z_MOUSE_BTN_LEFT) != 0;
	bool was_down = (set->last_buttons & Z_MOUSE_BTN_LEFT) != 0;
	set->last_buttons = buttons;

	int activated = -1;

	if (down && !was_down) {

		// -- press --

		int hit = widget_hit(set, cx, cy);
		if (hit < 0) return -1;

		set->pressed = hit;

		// Clicking also moves keyboard focus. Without it, a user who
		// clicks a button and then presses Tab resumes from wherever
		// focus happened to be left, which is not where they are
		// looking.
		z_widget_focus_set(set, hit);

		z_widget_t *wd = &set->items[hit];

		if (wd->type == Z_WIDGET_SLIDER) {

			// a slider acts immediately on press, and keeps acting
			// while dragged -- deferring to release would make it
			// impossible to aim, since the whole point is watching
			// the thing it controls change as you move.
			int16_t v = slider_value_at(wd, cx);
			if (v != wd->value) { wd->value = v; wd->dirty = true; }
			activated = hit;

		} else {

			// buttons and toggles only redraw on press (to show
			// themselves held); the decision waits for release.
			wd->dirty = true;

		}

		z_widget_draw_all(set, false);
		return activated;

	}

	if (down && was_down) {

		// -- drag --

		if (set->pressed < 0) return -1;

		z_widget_t *wd = &set->items[set->pressed];

		if (wd->type == Z_WIDGET_SLIDER) {
			int16_t v = slider_value_at(wd, cx);
			if (v != wd->value) {
				wd->value = v;
				wd->dirty = true;
				z_widget_draw_all(set, false);
			}
			// reported every sample, changed or not, so a caller that
			// re-renders from the value doesn't need to track whether
			// this particular sample moved it.
			return set->pressed;
		}

		return -1;

	}

	if (!down && was_down) {

		// -- release --

		int idx = set->pressed;
		set->pressed = -1;

		if (idx < 0) return -1;

		z_widget_t *wd = &set->items[idx];

		// releasing outside the widget the press started in cancels
		// it -- the standard escape hatch for "I clicked the wrong
		// thing". Still needs a redraw to clear the held appearance.
		if (!widget_contains(wd, cx, cy)) {
			wd->dirty = true;
			z_widget_draw_all(set, false);
			return -1;
		}

		switch (wd->type) {

			case Z_WIDGET_TOGGLE:
			case Z_WIDGET_SWATCH:
				if (wd->group) {
					z_widget_select(set, idx);
				} else {
					wd->on = !wd->on;
					wd->dirty = true;
				}
				activated = idx;
				break;

			case Z_WIDGET_SLIDER:
				activated = idx;
				break;

			case Z_WIDGET_BUTTON:
			default:
				wd->dirty = true;
				activated = idx;
				break;

		}

		z_widget_draw_all(set, false);
		return activated;

	}

	return -1;

}

// ---------------------------------------------------------------
// scrollbars -- see zwidget.h for why these aren't a z_widget_type_t
// ---------------------------------------------------------------

// smallest the thumb is ever drawn. A thumb sized strictly
// proportionally vanishes on a long document (page/total of 1/500
// over a 300px bar is well under a pixel), leaving a scrollbar with
// nothing to grab. Clamping the SIZE while keeping the POSITION
// proportional is the standard resolution and what every scrollbar
// you have ever used does.
#define SB_THUMB_MIN   12

// How far the thumb is inset from each long edge of the bar. The
// thumb is therefore Z_SB_THICK - 2*SB_THUMB_INSET = 8px across,
// inside a 12px hit target: slim enough to read as a marker rather
// than a wall of ink, wide enough to grab without aiming.
#define SB_THUMB_INSET  2

// -- geometry helpers --
//
// Everything below works in "long axis" terms -- length, position and
// offset along the bar -- so one set of arithmetic covers both
// orientations. Only sb_abs_rect() and the draw know which way round
// it is.

// absolute screen rect of the whole scrollbar
static void sb_abs_rect(const z_scrollbar_t *sb, int *x0, int *y0,
	int *x1, int *y1) {

	z_clip_t clip;
	z_win_content_rect(sb->win, &clip);

	*x0 = clip.x0 + sb->x;
	*y0 = clip.y0 + sb->y;

	if (sb->orient == Z_SB_VERT) {
		*x1 = *x0 + Z_SB_THICK - 1;
		*y1 = *y0 + sb->len - 1;
	} else {
		*x1 = *x0 + sb->len - 1;
		*y1 = *y0 + Z_SB_THICK - 1;
	}

}

// Thumb size and offset along the bar, both in pixels. A size of 0
// means "nothing to scroll" -- the caller draws no thumb at all
// rather than a full-length one, which would otherwise put a solid
// bar of ink down the side of every document that happens to fit.
static void sb_thumb(const z_scrollbar_t *sb, int *off, int *size) {

	*off = 0;
	*size = 0;

	if (sb->len <= 0) return;
	if (sb->total <= 0 || sb->page >= sb->total) return;

	int size_px = (int)(((int64_t)sb->page * sb->len) / sb->total);
	if (size_px < SB_THUMB_MIN) size_px = SB_THUMB_MIN;
	if (size_px > sb->len) size_px = sb->len;

	int span = sb->total - sb->page;		// > 0, checked above
	int travel = sb->len - size_px;
	int off_px = travel > 0
		? (int)(((int64_t)sb->value * travel) / span)
		: 0;

	if (off_px < 0) off_px = 0;
	if (off_px > travel) off_px = travel;

	*off = off_px;
	*size = size_px;

}

// clamps v into [0, total - page]
static int32_t sb_clamp(const z_scrollbar_t *sb, int32_t v) {

	int32_t max = sb->total - sb->page;
	if (max < 0) max = 0;

	if (v < 0) v = 0;
	if (v > max) v = max;

	return v;

}

// the pointer's position along the long axis, relative to the bar's
// own origin
static int sb_pointer_pos(const z_scrollbar_t *sb, int cx, int cy) {
	return (sb->orient == Z_SB_VERT) ? (cy - sb->y) : (cx - sb->x);
}

// -- setup --

void z_scrollbar_init(z_scrollbar_t *sb, const z_win_t *win, z_sb_orient_t o) {

	memset(sb, 0, sizeof(*sb));

	sb->win = win;
	sb->orient = o;
	sb->page = 1;
	sb->total = 1;
	sb->dirty = true;

}

void z_scrollbar_set_geom(z_scrollbar_t *sb, int x, int y, int len) {

	if (len < 0) len = 0;

	sb->x = (int16_t)x;
	sb->y = (int16_t)y;
	sb->len = (int16_t)len;
	sb->dirty = true;

}

bool z_scrollbar_set_range(z_scrollbar_t *sb, int32_t total, int32_t page) {

	if (total < 0) total = 0;
	if (page < 0) page = 0;

	sb->total = total;
	sb->page = page;
	sb->dirty = true;

	int32_t v = sb_clamp(sb, sb->value);
	if (v == sb->value) return false;

	sb->value = v;
	return true;

}

bool z_scrollbar_set_value(z_scrollbar_t *sb, int32_t v) {

	v = sb_clamp(sb, v);
	if (v == sb->value) return false;

	sb->value = v;
	sb->dirty = true;

	return true;

}

// -- drawing --
//
// Two hardware fills: clear the bar, then paint the thumb. No frame,
// no arrows, no per-pixel work at all -- which also makes this cheap
// enough to call on every scroll step without thinking about it.

void z_scrollbar_draw(z_scrollbar_t *sb, bool force) {

	if (!force && !sb->dirty) return;

	sb->dirty = false;

	if (sb->len <= 0) return;

	int x0, y0, x1, y1;
	sb_abs_rect(sb, &x0, &y0, &x1, &y1);

	z_clip_t clip;
	z_win_content_rect(sb->win, &clip);

	// entirely outside the content area (a window shrunk below its
	// own furniture) -- draw nothing rather than trusting each
	// primitive below to clip it away. z_fb_hw_fill_rect() clamps to
	// the SCREEN, not to the window, so this is the difference
	// between drawing nothing and drawing over another app.
	if (x1 < clip.x0 || x0 > clip.x1 || y1 < clip.y0 || y0 > clip.y1)
		return;

	int fx0 = x0 < clip.x0 ? clip.x0 : x0;
	int fy0 = y0 < clip.y0 ? clip.y0 : y0;
	int fx1 = x1 > clip.x1 ? clip.x1 : x1;
	int fy1 = y1 > clip.y1 ? clip.y1 : y1;

	if (fx1 < fx0 || fy1 < fy0) return;

	z_fb_hw_fill_rect(fx0, fy0, fx1 - fx0 + 1, fy1 - fy0 + 1, 0);

	int off, size;
	sb_thumb(sb, &off, &size);

	if (size <= 0) return;		// nothing to scroll -- bar stays blank

	int tx0, ty0, tx1, ty1;

	if (sb->orient == Z_SB_VERT) {
		tx0 = x0 + SB_THUMB_INSET;
		tx1 = x1 - SB_THUMB_INSET;
		ty0 = y0 + off;
		ty1 = ty0 + size - 1;
	} else {
		tx0 = x0 + off;
		tx1 = tx0 + size - 1;
		ty0 = y0 + SB_THUMB_INSET;
		ty1 = y1 - SB_THUMB_INSET;
	}

	// clamp to the content area for the same reason as above --
	// z_fb_hw_fill_rect() has no clip parameter.
	if (tx0 < clip.x0) tx0 = clip.x0;
	if (ty0 < clip.y0) ty0 = clip.y0;
	if (tx1 > clip.x1) tx1 = clip.x1;
	if (ty1 > clip.y1) ty1 = clip.y1;
	if (tx1 < tx0 || ty1 < ty0) return;

	z_fb_hw_fill_rect(tx0, ty0, tx1 - tx0 + 1, ty1 - ty0 + 1, 1);

}

// -- input --

bool z_scrollbar_has_pointer(const z_scrollbar_t *sb, int cx, int cy) {

	if (sb->dragging) return true;
	if (sb->len <= 0) return false;

	int w = (sb->orient == Z_SB_VERT) ? Z_SB_THICK : sb->len;
	int h = (sb->orient == Z_SB_VERT) ? sb->len : Z_SB_THICK;

	return cx >= sb->x && cx < sb->x + w &&
		cy >= sb->y && cy < sb->y + h;

}

bool z_scrollbar_mouse(z_scrollbar_t *sb, int cx, int cy, uint8_t buttons) {

	bool down = (buttons & Z_MOUSE_BTN_LEFT) != 0;
	bool was_down = (sb->last_buttons & Z_MOUSE_BTN_LEFT) != 0;
	sb->last_buttons = buttons;

	bool changed = false;

	if (!down && was_down) {
		sb->dragging = false;
		return false;
	}

	if (down && was_down && sb->dragging) {

		// -- thumb drag --
		//
		// Deliberately does NOT check whether the pointer is still
		// inside the bar. A scrollbar drag that cancels the moment
		// the cursor strays a few pixels sideways is infuriating,
		// and wm's pointer capture (Z_WM_MOUSE, zwm.h) keeps
		// delivering events to us out there specifically so this can
		// keep working.

		int off, size;
		sb_thumb(sb, &off, &size);

		int travel = sb->len - size;
		int span = sb->total - sb->page;

		if (size > 0 && travel > 0 && span > 0) {

			int pos = sb_pointer_pos(sb, cx, cy) - (int)sb->drag_grab;
			if (pos < 0) pos = 0;
			if (pos > travel) pos = travel;

			changed = z_scrollbar_set_value(sb,
				(int32_t)(((int64_t)pos * span) / travel));

		}

		if (changed) z_scrollbar_draw(sb, false);

		return changed;

	}

	if (!(down && !was_down)) return false;

	// -- press --

	if (!z_scrollbar_has_pointer(sb, cx, cy)) return false;

	int off, size;
	sb_thumb(sb, &off, &size);

	if (size <= 0) return false;		// nothing to scroll

	int pos = sb_pointer_pos(sb, cx, cy);

	if (pos >= off && pos < off + size) {
		// on the thumb -- start a drag, don't jump
		sb->dragging = true;
		sb->drag_grab = pos - off;
		return false;
	}

	// in the trough, before or after the thumb -- page towards the
	// click. Paging rather than jumping straight to the clicked
	// position is the older and, for text, the more useful behavior:
	// it moves by a known amount that keeps a line or two of context,
	// rather than landing somewhere the reader now has to re-find
	// their place in.
	int32_t step = sb->page > 1 ? sb->page - 1 : 1;
	changed = z_scrollbar_set_value(sb,
		pos < off ? sb->value - step : sb->value + step);

	if (changed) z_scrollbar_draw(sb, false);

	return changed;

}

// ---------------------------------------------------------------
// list boxes -- see zwidget.h
// ---------------------------------------------------------------
//
// Row height, double-click interval and scrollbar inset match
// sw/common/zflist.c, so every list in the system behaves alike.

#define LB_ROW_H          (z_font_5x8.h + 2)
#define LB_DBLCLICK_TICKS 256
#define LB_FIND_TICKS     Z_TICK_HZ

static int lb_rows(const z_listbox_t *lb) {
	int n = (lb->h - 2) / LB_ROW_H;
	return n > 0 ? n : 0;
}

static void lb_scroll_to(z_listbox_t *lb, int row) {

	int rows = lb_rows(lb);

	if (rows > 0 && row >= 0) {
		if (row < lb->top) lb->top = row;
		if (row >= lb->top + rows) lb->top = row - rows + 1;
	}
	if (lb->top > lb->count - rows) lb->top = lb->count - rows;
	if (lb->top < 0) lb->top = 0;

	z_scrollbar_set_value(&lb->sb, lb->top);

}

void z_listbox_init(z_listbox_t *lb, const z_win_t *win) {

	memset(lb, 0, sizeof(*lb));
	lb->win = win;
	lb->sel = -1;
	lb->last_press_row = -1;
	lb->dirty = true;
	z_scrollbar_init(&lb->sb, win, Z_SB_VERT);

}

void z_listbox_set_geom(z_listbox_t *lb, int x, int y, int w, int h) {

	lb->x = (int16_t)x;
	lb->y = (int16_t)y;
	lb->w = (int16_t)w;
	lb->h = (int16_t)h;

	// Inset one pixel inside the frame: z_scrollbar_draw() blanks its
	// whole rect, and flush against the frame it would erase the
	// frame's right edge -- the bug zflist.c's set_geom() records.
	z_scrollbar_set_geom(&lb->sb, x + w - Z_SB_THICK - 1, y + 1, h - 2);
	z_scrollbar_set_range(&lb->sb, lb->count, lb_rows(lb));
	lb_scroll_to(lb, lb->sel);
	lb->dirty = true;

}

void z_listbox_set_items(z_listbox_t *lb, int count, z_list_label_fn label,
	void *user) {

	lb->count = count > 0 ? count : 0;
	lb->label = label;
	lb->user = user;

	if (lb->sel >= lb->count) lb->sel = -1;

	z_scrollbar_set_range(&lb->sb, lb->count, lb_rows(lb));
	lb_scroll_to(lb, lb->sel);
	lb->dirty = true;

}

void z_listbox_select(z_listbox_t *lb, int index) {

	if (index >= lb->count) index = lb->count - 1;
	if (index < -1) index = -1;

	lb->sel = index;
	lb_scroll_to(lb, index);
	lb->dirty = true;

}

int z_listbox_selected(const z_listbox_t *lb) {
	return lb->sel;
}

void z_listbox_set_focus(z_listbox_t *lb, bool focused) {
	if (lb->focused == focused) return;
	lb->focused = focused;
	lb->dirty = true;
}

void z_listbox_invalidate(z_listbox_t *lb) {
	lb->dirty = true;
	lb->ring_drawn = false;
	lb->sb.dirty = true;
}

void z_listbox_draw(z_listbox_t *lb, bool force) {

	if (!force && !lb->dirty) {
		z_scrollbar_draw(&lb->sb, false);
		return;
	}

	lb->dirty = false;

	z_clip_t content;
	z_win_content_rect(lb->win, &content);

	int x0 = content.x0 + lb->x;
	int y0 = content.y0 + lb->y;
	int x1 = x0 + lb->w - 1;
	int y1 = y0 + lb->h - 1;

	if (x1 < content.x0 || x0 > content.x1 ||
		y1 < content.y0 || y0 > content.y1) return;

	// Rows clip to the list area MINUS the scrollbar: a long label
	// running under the thumb reads as a rendering bug.
	z_clip_t clip;
	clip.x0 = x0 + 1;
	clip.y0 = y0 + 1;
	clip.x1 = x1 - Z_SB_THICK - 1;
	clip.y1 = y1 - 1;

	if (clip.x0 < content.x0) clip.x0 = content.x0;
	if (clip.y0 < content.y0) clip.y0 = content.y0;
	if (clip.x1 > content.x1) clip.x1 = content.x1;
	if (clip.y1 > content.y1) clip.y1 = content.y1;

	if (clip.x1 >= clip.x0 && clip.y1 >= clip.y0)
		z_fb_hw_fill_rect(clip.x0, clip.y0,
			clip.x1 - clip.x0 + 1, clip.y1 - clip.y0 + 1, 0);

	z_win_hw_box(lb->win, x0, y0, x1, y1, 1);

	// The focus ring: drawn in ink when focused, in background once
	// when focus has left -- same reasoning as a widget's ring (see
	// z_widget_draw()), and why a list needs 2px of space around it.
	if (lb->focused || lb->ring_drawn) {
		z_win_hw_box(lb->win, x0 - 1, y0 - 1, x1 + 1, y1 + 1, lb->focused ? 1 : 0);
		lb->ring_drawn = lb->focused;
	}

	int rows = lb_rows(lb);

	for (int r = 0; r < rows && lb->label; r++) {

		int row = lb->top + r;
		if (row >= lb->count) break;

		int ry = y0 + 1 + r * LB_ROW_H;
		bool selected = (row == lb->sel);

		if (selected && clip.x1 >= clip.x0)
			z_fb_hw_fill_rect(clip.x0, ry, clip.x1 - clip.x0 + 1,
				ry + LB_ROW_H - 1 <= clip.y1 ? LB_ROW_H : (clip.y1 - ry + 1), 1);

		const char *text = lb->label(lb->user, row);
		if (text)
			z_fb_draw_text2(clip.x0 + 2, ry + 1, text,
				selected ? 0 : 1, selected ? 1 : 0, &z_font_5x8, &clip);

	}

	z_scrollbar_draw(&lb->sb, true);

}

bool z_listbox_has_pointer(const z_listbox_t *lb, int cx, int cy) {
	if (lb->sb.dragging) return true;
	return cx >= lb->x && cx < lb->x + lb->w &&
		cy >= lb->y && cy < lb->y + lb->h;
}

int z_listbox_mouse(z_listbox_t *lb, int cx, int cy, uint8_t buttons) {

	bool down = (buttons & Z_MOUSE_BTN_LEFT) != 0;
	bool was_down = (lb->last_buttons & Z_MOUSE_BTN_LEFT) != 0;

	// The scrollbar first, and for the whole of a drag it started.
	if (z_scrollbar_has_pointer(&lb->sb, cx, cy)) {
		lb->last_buttons = buttons;
		if (z_scrollbar_mouse(&lb->sb, cx, cy, buttons)) {
			lb->top = (int)lb->sb.value;
			lb->dirty = true;
			z_listbox_draw(lb, false);
		}
		return Z_LIST_NONE;
	}

	// Keep its edge detector fed so a release elsewhere ends a drag.
	z_scrollbar_mouse(&lb->sb, cx, cy, buttons);
	lb->last_buttons = buttons;

	if (!(down && !was_down)) return Z_LIST_NONE;

	if (cx < lb->x + 1 || cx >= lb->x + lb->w - Z_SB_THICK - 1) return Z_LIST_NONE;
	if (cy < lb->y + 1 || cy >= lb->y + lb->h - 1) return Z_LIST_NONE;

	int r = (cy - lb->y - 1) / LB_ROW_H;
	int row = lb->top + r;
	if (r >= lb_rows(lb) || row >= lb->count) return Z_LIST_NONE;

	uint32_t now = z_uptime_ticks();
	bool dbl = row == lb->last_press_row &&
		(now - lb->last_press_tick) < LB_DBLCLICK_TICKS;

	lb->last_press_row = dbl ? -1 : row;	// a third click starts over
	lb->last_press_tick = now;

	z_listbox_select(lb, row);
	z_listbox_draw(lb, false);

	return dbl ? Z_LIST_ACTIVATED : Z_LIST_SELECTED;

}

static char lb_lower(char c) {
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool lb_prefix(const char *text, const char *prefix, int n) {
	for (int i = 0; i < n; i++)
		if (!text[i] || lb_lower(text[i]) != lb_lower(prefix[i])) return false;
	return true;
}

// Type-to-find.
//
// A letter on its own jumps to the FIRST row starting with it -- B is
// Bangkok, wherever the selection was. If the selection already starts
// with that letter, it moves to the NEXT such row instead, wrapping, so
// pressing B again walks Bangkok, Beijing, Berlin, Brisbane.
//
// Letters typed less than LB_FIND_TICKS apart build a prefix instead
// ("mun" is Munich, past Mumbai), matched against the first row in list
// order -- which, for a sorted list, is the one a person means.
//
// This used to search forward from the current selection for every
// key. From a row that already started with the letter it found that
// same row and did not move, and from anywhere else where it landed
// depended on where you started -- neither is what a keyboard user
// expects a letter to do.
static int lb_find(z_listbox_t *lb, char c) {

	uint32_t now = z_uptime_ticks();
	bool fresh = lb->find_len == 0 || (now - lb->find_tick) > LB_FIND_TICKS;
	bool same = lb->find_len == 1 && lb_lower(lb->find[0]) == lb_lower(c);

	lb->find_tick = now;

	if (fresh || same) {

		lb->find[0] = c;
		lb->find_len = 1;

		bool on_it = false;
		if (lb->sel >= 0) {
			const char *cur = lb->label(lb->user, lb->sel);
			on_it = cur && lb_prefix(cur, lb->find, 1);
		}

		int start = on_it ? lb->sel + 1 : 0;

		for (int i = 0; i < lb->count; i++) {
			int row = (start + i) % lb->count;
			const char *t = lb->label(lb->user, row);
			if (t && lb_prefix(t, lb->find, 1)) return row;
		}

		return -1;

	}

	if (lb->find_len < sizeof(lb->find)) lb->find[lb->find_len++] = c;

	for (int row = 0; row < lb->count; row++) {
		const char *t = lb->label(lb->user, row);
		if (t && lb_prefix(t, lb->find, lb->find_len)) return row;
	}

	return -1;

}

int z_listbox_key(z_listbox_t *lb, uint32_t keysym) {

	int rows = lb_rows(lb);
	int to;

	if (rows < 1) rows = 1;
	if (lb->count <= 0) return Z_LIST_NONE;

	switch (keysym) {
	case Z_KEY_UP:       to = lb->sel < 0 ? 0 : lb->sel - 1; break;
	case Z_KEY_DOWN:     to = lb->sel + 1; break;
	case Z_KEY_PAGEUP:   to = lb->sel - rows; break;
	case Z_KEY_PAGEDOWN: to = (lb->sel < 0 ? 0 : lb->sel) + rows; break;
	case Z_KEY_HOME:     to = 0; break;
	case Z_KEY_END:      to = lb->count - 1; break;

	case 0x0d:
		return lb->sel >= 0 ? Z_LIST_ACTIVATED : Z_LIST_NONE;

	default:
		if (!lb->label) return Z_LIST_NONE;
		if ((keysym >= 'a' && keysym <= 'z') || (keysym >= 'A' && keysym <= 'Z') ||
			(keysym >= '0' && keysym <= '9') || keysym == '+' || keysym == '-' ||
			(keysym == ' ' && lb->find_len > 0)) {
			to = lb_find(lb, (char)keysym);
			if (to < 0) return Z_LIST_NONE;
			break;
		}
		return Z_LIST_NONE;
	}

	if (to < 0) to = 0;
	if (to >= lb->count) to = lb->count - 1;

	if (to == lb->sel) return Z_LIST_NONE;

	z_listbox_select(lb, to);
	z_listbox_draw(lb, false);
	return Z_LIST_SELECTED;

}
