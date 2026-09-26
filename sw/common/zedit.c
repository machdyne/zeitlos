/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See zedit.h.
 */

#include <string.h>

#include "zedit.h"
#include "zutf8.h"
#include "zkbd.h"		// Z_KEY_IS_TEXT

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
//
// cur and scroll are BYTE offsets into UTF-8, each at the start of a
// character; widths are display columns (z_utf8_cols(): two for a CJK
// character). docs/sdcard.md, "Long file names".
static void scroll_to_caret(z_edit_t *e, int cols) {

	if (e->scroll > e->len) e->scroll = e->len;
	if (e->scroll < 0) e->scroll = 0;
	if (e->cur < e->scroll) e->scroll = e->cur;

	// The caret sits AFTER the last visible character when it is at
	// the end of the text, so it needs a column of its own: the text
	// before it may use cols - 1. Getting this wrong hides the caret
	// exactly when someone is typing at the end, which is almost
	// always.
	while (e->scroll < e->cur &&
	       z_utf8_cols(e->buf + e->scroll, (size_t)(e->cur - e->scroll)) > cols - 1)
		e->scroll = z_utf8_next_off(e->buf, e->len, e->scroll);

}

bool z_edit_key(z_edit_t *e, uint32_t keysym) {

	if (e->secret && keysym >= 0x80 && Z_KEY_IS_TEXT(keysym)) return false;

	switch (keysym) {

	case Z_KEY_LEFT:
		if (e->cur > 0) { e->cur = z_utf8_prev_off(e->buf, e->len, e->cur); return true; }
		return false;

	case Z_KEY_RIGHT:
		if (e->cur < e->len) { e->cur = z_utf8_next_off(e->buf, e->len, e->cur); return true; }
		return false;

	case Z_KEY_HOME:
		if (e->cur != 0) { e->cur = 0; return true; }
		return false;

	case Z_KEY_END:
		if (e->cur != e->len) { e->cur = e->len; return true; }
		return false;

	case 0x7f:					// Backspace arrives as DEL -- see zkbd.c
		if (e->cur > 0) {
			// The whole character before the caret.
			int from = z_utf8_prev_off(e->buf, e->len, e->cur);
			memmove(&e->buf[from], &e->buf[e->cur],
				(size_t)(e->len - e->cur + 1));
			e->len -= e->cur - from;
			e->cur = from;
			return true;
		}
		return false;

	case Z_KEY_DELETE:
		if (e->cur < e->len) {
			int to = z_utf8_next_off(e->buf, e->len, e->cur);
			memmove(&e->buf[e->cur], &e->buf[to],
				(size_t)(e->len - to + 1));
			e->len -= to - e->cur;
			return true;
		}
		return false;

	case '\r':
	case '\n':
	case 0x1b:					// Escape
		// Not ours. See zedit.h.
		return false;

	default:
		// Any character a keyboard layout types, as UTF-8 -- file
		// names are long names now, and can be German or Japanese
		// (docs/sdcard.md). Controls and named keys are the caller's.
		// A character that does not fit whole is not inserted.
		if (Z_KEY_IS_TEXT(keysym)) {
			char u[Z_UTF8_MAX];
			int k = z_utf8_put(keysym, u);
			if (e->len + k > e->cap - 1) return false;
			memmove(&e->buf[e->cur + k], &e->buf[e->cur],
				(size_t)(e->len - e->cur + 1));
			memcpy(&e->buf[e->cur], u, (size_t)k);
			e->cur += k;
			e->len += k;
			return true;
		}
		return false;

	}

}

void z_edit_click(z_edit_t *e, int cx, int box_x, const z_font_t *font) {

	int col = (cx - box_x - PAD_X) / font->w;
	if (col < 0) col = 0;

	// Walk characters from the first visible one until `col` columns
	// are used; a wide character that straddles the click is not
	// stepped into.
	int off = e->scroll;
	while (off < e->len) {
		int next = z_utf8_next_off(e->buf, e->len, off);
		int w = z_utf8_cols(e->buf + off, (size_t)(next - off));
		if (w > col) break;
		col -= w;
		off = next;
	}

	e->cur = off;

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

	(void)n;
	// UTF-8 from the first visible character; the clip cuts it at
	// the box's right edge.
	if (e->secret) {
		// As many stars as characters from the first visible one --
		// all ASCII, so bytes are characters and characters columns.
		char stars[96];
		int k = e->len - e->scroll;
		if (k > (int)sizeof(stars) - 1) k = (int)sizeof(stars) - 1;
		if (k < 0) k = 0;
		memset(stars, '*', (size_t)k);
		stars[k] = 0;
		z_fb_draw_utf8(x0 + PAD_X, y0 + PAD_Y, stars, 1, font, &clip);
	} else
		z_fb_draw_utf8(x0 + PAD_X, y0 + PAD_Y, e->buf + e->scroll, 1, font, &clip);

	if (e->focus) {
		int cx = x0 + PAD_X +
			z_utf8_cols(e->buf + e->scroll, (size_t)(e->cur - e->scroll)) * font->w;
		if (cx >= clip.x0 && cx <= clip.x1)
			z_fb_hw_fill_rect(cx, y0 + 2, 1, y1 - y0 - 3, 1);
	}

}
