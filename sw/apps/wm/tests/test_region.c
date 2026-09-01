/*
 * Host test for wm's visible-region computation.
 *
 *   cc -std=gnu99 -o /tmp/t sw/apps/wm/tests/test_region.c && /tmp/t
 *
 * The algorithm is rectangle subtraction: a window's visible region
 * starts as its own rectangle and has every window in front of it
 * subtracted away. Getting it wrong in the LENIENT direction -- a
 * region larger than the truth -- puts back exactly the bug the whole
 * exercise removes, because an app is then permitted to draw over the
 * window in front of it. So the properties worth asserting are:
 *
 *   1. the region never includes a pixel an occluder covers
 *   2. the region never omits a pixel that is genuinely visible
 *   3. it degrades safely when it runs out of rectangles
 *
 * Property 3 matters more than it looks. Subtracting N occluders can
 * need more rectangles than the fixed array holds, and there are only
 * two honest ways out: keep a SUBSET of the visible area (draw less
 * than allowed -- pixels go stale until the next repaint) or keep a
 * SUPERSET (draw more than allowed -- corrupt the window in front).
 * The first is a cosmetic bug, the second is the bug being fixed, so
 * overflow must shrink. The test checks it does.
 *
 * The functions below are lifted from sw/apps/wm/wm.c. If you change
 * them there, change them here.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

typedef struct { int x0, y0, x1, y1; } z_clip_t;   /* inclusive */

#define WM_MAX_CLIP 8

/* ---- lifted from wm.c ---- */

static bool rect_empty(const z_clip_t *r) {
	return r->x1 < r->x0 || r->y1 < r->y0;
}

static bool rect_overlaps(const z_clip_t *a, const z_clip_t *b) {
	return !(a->x1 < b->x0 || b->x1 < a->x0 ||
	         a->y1 < b->y0 || b->y1 < a->y0);
}

/*
 * r minus cut, as up to four rectangles appended to out[].
 *
 * The four are the strips above, below, left and right of the
 * intersection -- the standard decomposition, and the reason a region
 * needs a list rather than a rectangle. Order does not matter to
 * correctness; they are emitted top, bottom, left, right so a
 * printout reads in a predictable order.
 *
 * Returns false if out[] filled up. The caller must treat that as
 * "shrink", never "keep what is left and carry on" -- see the header.
 */
static bool rect_subtract(const z_clip_t *r, const z_clip_t *cut,
	z_clip_t *out, int *n, int max) {

	if (!rect_overlaps(r, cut)) {
		if (*n >= max) return false;
		out[(*n)++] = *r;
		return true;
	}

	/* above */
	if (cut->y0 > r->y0) {
		if (*n >= max) return false;
		z_clip_t t = { r->x0, r->y0, r->x1, cut->y0 - 1 };
		out[(*n)++] = t;
	}
	/* below */
	if (cut->y1 < r->y1) {
		if (*n >= max) return false;
		z_clip_t t = { r->x0, cut->y1 + 1, r->x1, r->y1 };
		out[(*n)++] = t;
	}
	/* left, limited to the rows the cut actually covers */
	{
		int ty0 = cut->y0 > r->y0 ? cut->y0 : r->y0;
		int ty1 = cut->y1 < r->y1 ? cut->y1 : r->y1;
		if (cut->x0 > r->x0 && ty0 <= ty1) {
			if (*n >= max) return false;
			z_clip_t t = { r->x0, ty0, cut->x0 - 1, ty1 };
			out[(*n)++] = t;
		}
		/* right */
		if (cut->x1 < r->x1 && ty0 <= ty1) {
			if (*n >= max) return false;
			z_clip_t t = { cut->x1 + 1, ty0, r->x1, ty1 };
			out[(*n)++] = t;
		}
	}

	return true;
}

/*
 * The visible region of `win` given the occluders in front of it.
 *
 * Returns the count. A count of 0 means FULLY OCCLUDED -- callers must
 * send that as one empty rectangle rather than as an empty list,
 * because zgfx reads an empty list as "unrestricted".
 */
static int region_compute(const z_clip_t *win,
	const z_clip_t *occ, int nocc, z_clip_t *out, int max) {

	z_clip_t cur[WM_MAX_CLIP], nxt[WM_MAX_CLIP];
	int ncur = 0, nnxt;

	cur[ncur++] = *win;

	for (int i = 0; i < nocc; i++) {
		nnxt = 0;
		for (int j = 0; j < ncur; j++) {
			if (!rect_subtract(&cur[j], &occ[i], nxt, &nnxt, WM_MAX_CLIP)) {
				/*
				 * Out of room. Keeping what has accumulated so far
				 * would be a SUPERSET of the truth for the pieces not
				 * yet cut, so the remaining occluders are still
				 * applied -- the result is a subset, which is the safe
				 * direction. Anything that then does not fit is
				 * dropped, shrinking further.
				 */
				break;
			}
		}
		for (int j = 0; j < nnxt; j++) cur[j] = nxt[j];
		ncur = nnxt;
		if (ncur == 0) break;
	}

	int n = 0;
	for (int i = 0; i < ncur && n < max; i++)
		if (!rect_empty(&cur[i])) out[n++] = cur[i];

	return n;
}

/* ---- test ---- */

static int fails;

static bool covered(const z_clip_t *rs, int n, int x, int y) {
	for (int i = 0; i < n; i++)
		if (x >= rs[i].x0 && x <= rs[i].x1 &&
		    y >= rs[i].y0 && y <= rs[i].y1) return true;
	return false;
}

/* Exhaustive pixel comparison against the definition of "visible". */
static void check(const char *name, const z_clip_t *win,
	const z_clip_t *occ, int nocc) {

	z_clip_t out[WM_MAX_CLIP];
	int n = region_compute(win, occ, nocc, out, WM_MAX_CLIP);
	int over = 0, under = 0;

	for (int y = win->y0 - 2; y <= win->y1 + 2; y++)
		for (int x = win->x0 - 2; x <= win->x1 + 2; x++) {
			bool inwin = x >= win->x0 && x <= win->x1 &&
			             y >= win->y0 && y <= win->y1;
			bool hidden = covered(occ, nocc, x, y);
			bool want = inwin && !hidden;
			bool got = covered(out, n, x, y);

			if (got && !want) over++;    /* would draw where it must not */
			if (!got && want) under++;   /* would leave a visible pixel stale */
		}

	if (over) {
		printf("  FAIL %-34s %d px OUTSIDE the visible area (unsafe)\n",
		       name, over);
		fails++;
	} else if (under) {
		printf("  FAIL %-34s %d visible px missing (%d rects)\n",
		       name, under, n);
		fails++;
	} else {
		printf("  ok   %-34s %d rect(s)\n", name, n);
	}
}

int main(void) {
	z_clip_t win = { 100, 100, 299, 299 };

	printf("== nothing in front ==\n");
	check("unoccluded", &win, NULL, 0);

	printf("\n== one occluder, each edge ==\n");
	{ z_clip_t o[1] = {{  50,  50, 199, 349 }}; check("covers the left",   &win, o, 1); }
	{ z_clip_t o[1] = {{ 200,  50, 349, 349 }}; check("covers the right",  &win, o, 1); }
	{ z_clip_t o[1] = {{  50,  50, 349, 199 }}; check("covers the top",    &win, o, 1); }
	{ z_clip_t o[1] = {{  50, 200, 349, 349 }}; check("covers the bottom", &win, o, 1); }

	printf("\n== the cases a single rectangle cannot express ==\n");
	{ z_clip_t o[1] = {{ 150, 150, 249, 249 }}; check("hole in the middle", &win, o, 1); }
	{ z_clip_t o[1] = {{ 200, 200, 349, 349 }}; check("corner bitten out",  &win, o, 1); }
	{ z_clip_t o[1] = {{ 150,  50, 249, 199 }}; check("notch from the top", &win, o, 1); }

	printf("\n== fully covered ==\n");
	{ z_clip_t o[1] = {{  50,  50, 349, 349 }}; check("completely hidden", &win, o, 1); }

	printf("\n== several occluders ==\n");
	{ z_clip_t o[2] = {{ 50, 50, 199, 349 }, { 250, 50, 349, 349 }};
	  check("both sides", &win, o, 2); }
	{ z_clip_t o[3] = {{ 90, 90, 150, 150 }, { 250, 90, 320, 150 },
	                   { 90, 250, 150, 320 }};
	  check("three corners", &win, o, 3); }

	printf("\n== no occluder at all touches it ==\n");
	{ z_clip_t o[2] = {{ 400, 400, 500, 500 }, { 0, 0, 50, 50 }};
	  check("disjoint occluders", &win, o, 2); }

	printf("\n== overflow must SHRINK, never grow ==\n");
	/*
	 * Enough scattered occluders to exhaust WM_MAX_CLIP. The only
	 * assertion that matters is that nothing outside the true visible
	 * area survives -- "under" is acceptable here and is reported
	 * separately by check(), so this case is expected to fail on
	 * `under` and must not fail on `over`.
	 */
	{
		z_clip_t o[6] = {
			{ 120, 120, 139, 139 }, { 180, 120, 199, 139 },
			{ 240, 120, 259, 139 }, { 120, 200, 139, 219 },
			{ 180, 200, 199, 219 }, { 240, 200, 259, 219 },
		};
		z_clip_t out[WM_MAX_CLIP];
		int n = region_compute(&win, o, 6, out, WM_MAX_CLIP);
		int over = 0;
		for (int y = win.y0; y <= win.y1; y++)
			for (int x = win.x0; x <= win.x1; x++) {
				bool want = !covered(o, 6, x, y);
				if (covered(out, n, x, y) && !want) over++;
			}
		if (over) {
			printf("  FAIL overflow grew the region: %d unsafe px\n", over);
			fails++;
		} else {
			printf("  ok   overflow shrank safely (%d rects, 0 unsafe px)\n", n);
		}
	}

	printf("\n%s\n", fails ? "FAIL" : "PASS -- region computation is correct and safe");
	return fails ? 1 : 0;
}
