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

			bool lit = wd->on || (set->pressed == idx);

			// body: solid when lit, empty when not. The pressed state
			// borrows the same visual as "on" deliberately -- with one
			// bit of colour there is no third appearance available,
			// and "held down looks like selected" is both the honest
			// reading and what the original Macintosh did.
			z_fb_hw_fill_rect(fx0 + 1, fy0 + 1,
				fx1 - fx0 - 1, fy1 - fy0 - 1, lit ? 1 : 0);

			z_win_hw_box(set->win, x0, y0, x1, y1, 1);

			int ink = lit ? 0 : 1;

			if (wd->icon) {

				widget_draw_icon(x0 + (wd->w - 16) / 2,
					y0 + (wd->h - 16) / 2, wd->icon, ink, &clip);

			} else if (wd->label) {

				// z_win_draw_text() takes content-relative
				// coordinates, unlike everything else in this
				// function -- see zwin.h's own note on that
				// inconsistency.
				int tw = (int)strlen(wd->label) * z_font_5x8.w;
				z_win_draw_text(set->win,
					wd->x + (wd->w - tw) / 2,
					wd->y + (wd->h - z_font_5x8.h) / 2,
					wd->label, ink, &z_font_5x8);

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
