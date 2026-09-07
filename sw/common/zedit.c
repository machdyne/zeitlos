/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See zedit.h.
 */

#include <string.h>

#include "zedit.h"
#include "zkbd.h"

#define PAD_X 3
#define PAD_Y 3

int z_edit_visible_cols(int w, const z_font_t *font) {
	int cols = (w - 2 * PAD_X) / font->w;
	return cols < 1 ? 1 : cols;
}

void z_edit_set(z_edit_t *e, const char *s) {

	e->len = 0;

	if (s)
		for (; s[e->len] && e->len < e->cap - 1; e->len++)
			e->buf[e->len] = s[e->len];

	e->buf[e->len] = '\0';
	e->cur = e->len;
	e->scroll = 0;

}

void z_edit_init(z_edit_t *e, char *buf, int cap, const char *initial) {
	memset(e, 0, sizeof(*e));
	e->buf = buf;
	e->cap = cap;
	z_edit_set(e, initial);
}

// Keeps the caret inside the visible window.
//
// Called from draw rather than from key handling, because only the
// draw knows how wide the box is -- and a field that scrolls only
// when a key arrives would sit wrong after a resize.
static void scroll_to_caret(z_edit_t *e, int cols) {

	if (e->cur < e->scroll) e->scroll = e->cur;

	// The caret sits AFTER the last visible character when it is at
	// the end of the text, so the test is >=, not >. Getting this
	// wrong hides the caret exactly when someone is typing at the
	// end, which is almost always.
	if (e->cur >= e->scroll + cols) e->scroll = e->cur - cols + 1;

	if (e->scroll > e->len) e->scroll = e->len;
	if (e->scroll < 0) e->scroll = 0;

}

bool z_edit_key(z_edit_t *e, uint32_t keysym) {

	switch (keysym) {

	case Z_KEY_LEFT:
		if (e->cur > 0) { e->cur--; return true; }
		return false;

	case Z_KEY_RIGHT:
		if (e->cur < e->len) { e->cur++; return true; }
		return false;

	case Z_KEY_HOME:
		if (e->cur != 0) { e->cur = 0; return true; }
		return false;

	case Z_KEY_END:
		if (e->cur != e->len) { e->cur = e->len; return true; }
		return false;

	case 0x7f:					// Backspace arrives as DEL -- see zkbd.c
		if (e->cur > 0) {
			memmove(&e->buf[e->cur - 1], &e->buf[e->cur],
				(size_t)(e->len - e->cur + 1));
			e->cur--;
			e->len--;
			return true;
		}
		return false;

	case Z_KEY_DELETE:
		if (e->cur < e->len) {
			memmove(&e->buf[e->cur], &e->buf[e->cur + 1],
				(size_t)(e->len - e->cur));
			e->len--;
			return true;
		}
		return false;

	case '\r':
	case '\n':
	case 0x1b:					// Escape
		// Not ours. See zedit.h.
		return false;

	default:
		// Printable ASCII only. This field holds a filename or a URL,
		// and anything outside this range in either is something the
		// caller should be deciding about, not something to insert
		// silently.
		if (keysym >= 0x20 && keysym < 0x7f && e->len < e->cap - 1) {
			memmove(&e->buf[e->cur + 1], &e->buf[e->cur],
				(size_t)(e->len - e->cur + 1));
			e->buf[e->cur] = (char)keysym;
			e->cur++;
			e->len++;
			return true;
		}
		return false;

	}

}

void z_edit_click(z_edit_t *e, int cx, int box_x, const z_font_t *font) {

	int col = (cx - box_x - PAD_X) / font->w;

	if (col < 0) col = 0;
	col += e->scroll;
	if (col > e->len) col = e->len;

	e->cur = col;

}

void z_edit_draw(const z_win_t *win, z_edit_t *e,
	int x, int y, int w, int h, const z_font_t *font) {

	z_clip_t content, clip;
	int x0, y0, x1, y1, cols, n;

	if (w <= 2 * PAD_X || h <= 2) return;

	z_win_content_rect(win, &content);

	x0 = content.x0 + x;
	y0 = content.y0 + y;
	x1 = x0 + w - 1;
	y1 = y0 + h - 1;

	if (x1 <= x0 || y1 <= y0) return;

	cols = z_edit_visible_cols(w, font);
	scroll_to_caret(e, cols);

	// One engine, so program order is drawing order -- see zedit.h.
	z_fb_hw_fill_rect(x0 + 1, y0 + 1, x1 - x0 - 1, y1 - y0 - 1, 0);
	z_fb_hw_fill_rect(x0, y0, x1 - x0 + 1, 1, 1);
	z_fb_hw_fill_rect(x0, y1, x1 - x0 + 1, 1, 1);
	z_fb_hw_fill_rect(x0, y0, 1, y1 - y0 + 1, 1);
	z_fb_hw_fill_rect(x1, y0, 1, y1 - y0 + 1, 1);

	clip.x0 = x0 + 2;
	clip.y0 = y0 + 1;
	clip.x1 = x1 - 2;
	clip.y1 = y1 - 1;

	if (clip.x1 < clip.x0) return;

	n = e->len - e->scroll;
	if (n > cols) n = cols;

	for (int i = 0; i < n; i++)
		z_fb_draw_char(x0 + PAD_X + i * font->w, y0 + PAD_Y,
			(unsigned char)e->buf[e->scroll + i], 1, font, &clip);

	if (e->focus) {
		int cx = x0 + PAD_X + (e->cur - e->scroll) * font->w;
		if (cx >= clip.x0 && cx <= clip.x1)
			z_fb_hw_fill_rect(cx, y0 + 2, 1, y1 - y0 - 3, 1);
	}

}
