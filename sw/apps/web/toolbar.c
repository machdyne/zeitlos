/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See toolbar.h.
 */

#include "toolbar.h"

void toolbar_geom(int content_w, toolbar_t *tb) {

	// [ field ][Go][<][>] -- Go beside the field it acts on, the
	// history buttons after it.
	int bx = content_w - 3 * TB_BTN_W - 2 * TB_BTN_GAP - TB_MARGIN;

	tb->buttons = true;

	// A window too narrow for both keeps the field and drops the
	// buttons. The field is the part with no keyboard equivalent --
	// Back and Forward have 'b' and 'f', Go is Enter and Stop is
	// Escape -- so it is the part to keep when there is not room for
	// everything.
	if (bx - TB_MARGIN * 2 < TB_MIN_FIELD) {
		tb->buttons = false;
		tb->go_x = tb->back_x = tb->fwd_x = content_w;	// off the end, unhittable
		tb->field_x = TB_MARGIN;
		tb->field_w = content_w - 2 * TB_MARGIN;
		if (tb->field_w < 1) tb->field_w = 1;
		return;
	}

	tb->field_x = TB_MARGIN;
	tb->field_w = bx - TB_MARGIN * 2;
	tb->go_x = bx;
	tb->back_x = bx + TB_BTN_W + TB_BTN_GAP;
	tb->fwd_x = tb->back_x + TB_BTN_W + TB_BTN_GAP;

}

toolbar_hit_t toolbar_hit(const toolbar_t *tb, int x, int y) {

	if (y < 0 || y >= TB_H) return TB_HIT_NONE;

	if (tb->buttons) {
		if (x >= tb->go_x && x < tb->go_x + TB_BTN_W)
			return TB_HIT_GO;
		if (x >= tb->back_x && x < tb->back_x + TB_BTN_W)
			return TB_HIT_BACK;
		if (x >= tb->fwd_x && x < tb->fwd_x + TB_BTN_W)
			return TB_HIT_FORWARD;
	}

	if (x >= tb->field_x && x < tb->field_x + tb->field_w)
		return TB_HIT_FIELD;

	return TB_HIT_NONE;

}

// A solid triangle, inset from the frame.
//
// Defined by a half-width that grows with distance from the tip, so
// the shape is stated once rather than assembled from column and
// height arithmetic at the call site.
bool toolbar_arrow_px(int dx, int dy, int w, int h, bool left) {

	int reach = 4;					// tip to base
	int cy = (h - 1) / 2;
	int x0, d;

	if (dx < 0 || dy < 0 || dx >= w || dy >= h) return false;

	// ONE shape, and the other arrow is its mirror.
	//
	// Computing each independently gave two triangles that were each
	// correct and were not mirror images of one another, because the
	// span they occupied was not symmetric about the button's centre
	// line. Mirroring the coordinate makes that impossible by
	// construction rather than by arithmetic that has to agree.
	if (!left) dx = w - 1 - dx;

	// Tip column, placed so the whole arrow is centred.
	x0 = (w - 1 - reach) / 2;

	d = dx - x0;
	if (d < 0 || d > reach) return false;

	// The half-height grows a pixel per column, so the edges are a
	// clean 45 degrees at this size.
	return (dy >= cy - d) && (dy <= cy + d);

}

bool toolbar_stipple(int dx, int dy) {
	return ((dx + dy) & 1) == 0;
}

bool toolbar_cross_px(int dx, int dy, int w, int h) {

	// Two diagonals, each two pixels wide in every row: a box one column
	// wider than it is tall, so each row holds a pair per diagonal and
	// the mark is the same top to bottom as left to right. Sized so
	// the box centres exactly across the button's width.
	int size = (h < w ? h : w) - 9;		// rows
	int bw, x0, y0, x, y;

	if (size < 3) return false;
	if ((w - (size + 1)) & 1) size--;	// centre on whole pixels
	bw = size + 1;
	x0 = (w - bw) / 2;
	y0 = (h - size) / 2;

	if (dx < 0 || dy < 0 || dx >= w || dy >= h) return false;
	x = dx - x0;
	y = dy - y0;
	if (x < 0 || y < 0 || x >= bw || y >= size) return false;

	return x == y || x == y + 1 ||					// "\"
		x == size - 1 - y || x == size - y;			// "/"

}

