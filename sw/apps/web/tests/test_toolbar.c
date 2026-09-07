/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Host test for toolbar.c -- the browser toolbar's geometry.
 *
 * Worth testing because the failure is quiet: a button whose drawn
 * rectangle and whose clickable rectangle differ by a pixel works
 * everywhere except at its own edge, and nobody reports that. Both
 * come from toolbar_geom(), so the thing to check is that the
 * rectangles it produces do not overlap, do not leave the window, and
 * agree with toolbar_hit() at every boundary.
 *
 * Swept across window widths rather than checked at one, because the
 * interesting cases are the narrow ones the author never resizes to.
 */

#include <stdio.h>
#include "../toolbar.h"

static int fails, checks;
static void ck(int c, const char *w) {
	checks++; if (!c) { fails++; printf("FAIL: %s\n", w); } }

int main(void) {

	// A sweep, including widths far narrower than any real window.
	for (int cw = 20; cw <= 800; cw++) {

		toolbar_t tb;
		toolbar_geom(cw, &tb);

		if (tb.field_w < 1) { fails++; printf("FAIL: cw=%d empty field\n", cw); break; }
		if (tb.field_x < 0) { fails++; printf("FAIL: cw=%d field off left\n", cw); break; }

		// Nothing may leave the window.
		if (tb.field_x + tb.field_w > cw) {
			fails++; printf("FAIL: cw=%d field runs past the edge\n", cw); break; }

		if (tb.buttons) {
			if (tb.fwd_x + TB_BTN_W > cw) {
				fails++; printf("FAIL: cw=%d forward button off the edge\n", cw); break; }
			// ...and nothing may overlap anything else.
			if (tb.field_x + tb.field_w > tb.back_x) {
				fails++; printf("FAIL: cw=%d field overlaps back\n", cw); break; }
			if (tb.back_x + TB_BTN_W > tb.fwd_x) {
				fails++; printf("FAIL: cw=%d buttons overlap\n", cw); break; }
		}

	}
	ck(1, "geometry stays inside the window and never overlaps");

	// Hit testing must agree with the geometry at every boundary --
	// this is the mismatch the whole file exists to prevent.
	{
		toolbar_t tb;
		toolbar_geom(540, &tb);

		ck(tb.buttons, "a normal window has buttons");
		ck(toolbar_hit(&tb, tb.back_x, 4) == TB_HIT_BACK,
			"back button, first pixel");
		ck(toolbar_hit(&tb, tb.back_x + TB_BTN_W - 1, 4) == TB_HIT_BACK,
			"back button, last pixel");
		ck(toolbar_hit(&tb, tb.back_x - 1, 4) != TB_HIT_BACK,
			"one pixel left of back is not back");
		ck(toolbar_hit(&tb, tb.back_x + TB_BTN_W, 4) != TB_HIT_BACK,
			"one pixel right of back is not back");

		ck(toolbar_hit(&tb, tb.fwd_x, 4) == TB_HIT_FORWARD,
			"forward button, first pixel");
		ck(toolbar_hit(&tb, tb.fwd_x + TB_BTN_W - 1, 4) == TB_HIT_FORWARD,
			"forward button, last pixel");

		ck(toolbar_hit(&tb, tb.field_x, 4) == TB_HIT_FIELD,
			"field, first pixel");
		ck(toolbar_hit(&tb, tb.field_x + tb.field_w - 1, 4) == TB_HIT_FIELD,
			"field, last pixel");

		// Below the bar is the page, whoever asks.
		ck(toolbar_hit(&tb, tb.field_x, TB_H) == TB_HIT_NONE,
			"a click below the bar is not the toolbar");
		ck(toolbar_hit(&tb, tb.back_x, TB_H) == TB_HIT_NONE,
			"nor over a button");
		ck(toolbar_hit(&tb, tb.field_x, -1) == TB_HIT_NONE,
			"nor above it");
	}

	// Narrow: the field survives, the buttons go, and nothing that is
	// not drawn can be clicked.
	{
		toolbar_t tb;
		toolbar_geom(50, &tb);
		ck(!tb.buttons, "a narrow window drops the buttons");
		ck(tb.field_w > 0, "and keeps the field");
		ck(toolbar_hit(&tb, 49, 4) != TB_HIT_BACK &&
			toolbar_hit(&tb, 49, 4) != TB_HIT_FORWARD,
			"a dropped button cannot be clicked");
	}

	// -- the button glyphs --
	//
	// The first version of this drew NOTHING: the buttons appeared as
	// empty frames and the code read as though it worked. So the
	// shape is checked here rather than trusted -- that it exists at
	// all, that it stays inside the button, and that the two arrows
	// are mirror images.
	{
		int h = TB_H - 3;
		int nleft = 0, nright = 0, outside = 0, asym = 0;

		for (int y = -2; y < h + 2; y++) {
			for (int x = -2; x < TB_BTN_W + 2; x++) {
				bool l = toolbar_arrow_px(x, y, TB_BTN_W, h, true);
				bool r = toolbar_arrow_px(x, y, TB_BTN_W, h, false);

				if ((l || r) && (x < 1 || y < 1 ||
					x >= TB_BTN_W - 1 || y >= h - 1))
					outside++;

				if (l) nleft++;
				if (r) nright++;

				// Mirroring: a left arrow at x is a right arrow at
				// the mirrored column. Getting this wrong gives two
				// buttons that point the same way, which is worse
				// than none.
				if (x >= 0 && x < TB_BTN_W && y >= 0 && y < h) {
					bool rm = toolbar_arrow_px(TB_BTN_W - 1 - x, y,
						TB_BTN_W, h, false);
					if (l != rm) asym++;
				}
			}
		}

		ck(nleft > 12, "the back arrow has a visible number of pixels");
		ck(nright > 12, "so does the forward arrow");
		ck(nleft == nright, "and they are the same size");
		ck(!outside, "neither touches or escapes the frame");
		ck(!asym, "the two arrows are mirror images");

		// Stipple: a disabled arrow is thinner but still an arrow.
		{
			int lit = 0;
			for (int y = 0; y < h; y++)
				for (int x = 0; x < TB_BTN_W; x++)
					if (toolbar_arrow_px(x, y, TB_BTN_W, h, true) &&
						toolbar_stipple(x, y)) lit++;
			ck(lit > 4, "a disabled arrow is still drawn");
			ck(lit < nleft, "but is fainter than an enabled one");
		}
	}

	printf("%s: %d checks, %d failures\n", fails?"FAIL":"ok", checks, fails);
	return fails ? 1 : 0;

}
