#ifndef Z_EDIT_H
#define Z_EDIT_H

#include <stdint.h>
#include <stdbool.h>

#include "zwin.h"
#include "zgfx.h"

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A single-line text field.
 *
 * -- why this exists --
 *
 * It was private to sw/common/zdialog.c, whose own comment said:
 *
 *   "kept private to this file rather than promoted into zwidget.h.
 *    There is exactly one caller ... If a second caller appears, that
 *    is the moment to move it, not before."
 *
 * sw/apps/web's URL bar is the second caller. Moving it here rather
 * than copying it means the next improvement lands in both, which is
 * the whole argument: the dialog's version could not scroll, so a
 * filename wider than the box simply ran off the end. This one can,
 * and the dialog gets that for free.
 *
 * -- what it is not --
 *
 * No selection, no clipboard, no multi-line, no undo. It is a place
 * to type a name or a URL, and every one of those would be a
 * different widget wearing this one's clothes.
 *
 * -- ownership --
 *
 * The caller owns the buffer. That keeps this struct small enough to
 * sit in an app's static state without a heap, and it means the
 * caller can hand the same buffer to whatever consumes the result
 * without a copy.
 */

typedef struct {

	char		*buf;			// caller-owned, NUL terminated
	int			cap;			// bytes in buf, including the NUL

	int			len;
	int			cur;			// caret, 0..len

	// First visible column. Non-zero when the text is wider than the
	// box -- the dialog's version had no equivalent and simply drew
	// past its own frame.
	int			scroll;

	// The caret is only drawn with focus. A field showing a caret
	// while something else also looks selected gives two equally
	// strong claims on where typing goes.
	bool		focus;

} z_edit_t;

// `initial` may be NULL. The caret lands at the end, which is where
// someone editing an existing value wants it.
void z_edit_init(z_edit_t *e, char *buf, int cap, const char *initial);

void z_edit_set(z_edit_t *e, const char *s);

// Handles printable characters, backspace, delete, and cursor
// movement. Returns true if anything changed and the field needs
// redrawing.
//
// Returns FALSE for Return and Escape without touching the text:
// they mean different things to different callers -- submit, cancel,
// close the window -- and a text field is the wrong place to decide.
bool z_edit_key(z_edit_t *e, uint32_t keysym);

// Places the caret from a click at content-relative `cx`, given the
// field box the caller drew.
void z_edit_click(z_edit_t *e, int cx, int box_x, const z_font_t *font);

// Draws the field, framed, at a content-relative rectangle.
//
// Everything is drawn with the BLITTER, including the frame -- see
// zdialog.c's own note on why. The blitter and the line rasterizer
// are independent engines writing the same VRAM with no ordering
// between them, and a field's frame and its first glyphs routinely
// share a 32-bit word.
void z_edit_draw(const z_win_t *win, z_edit_t *e,
	int x, int y, int w, int h, const z_font_t *font);

// Columns of text the box can show. Useful for a caller that wants to
// size a field to its content.
int z_edit_visible_cols(int w, const z_font_t *font);

#endif
