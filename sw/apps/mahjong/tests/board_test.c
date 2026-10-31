/*
 * Zeitlos mahjong -- host tests for the canvas and the board.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Three things a look at tests/out cannot settle:
 *
 *   - THE CANVAS'S WORD ARITHMETIC. Every pixel of the table goes
 *     through put_word()'s masks and shifts; an off-by-one at a word
 *     boundary is a column of wrong pixels every 32, which a picture
 *     shows only if it happens to fall on something. So random fills,
 *     rows, masks, text and shades are drawn into the canvas and into a
 *     one-pixel-at-a-time reference, in every plane count and in mono,
 *     with random clips, and compared pixel for pixel.
 *
 *   - PARTIAL REPAINTS. The app repaints only the boxes a move touched.
 *     If the drawing order, a tile's box and its shadow disagree
 *     anywhere, a match leaves a sliver of a shadow or a missing side
 *     -- on hardware, and only sometimes. So whole games are played on
 *     every layout, from every corner, at both sizes, in mono, 16 and 64
 *     colours, with and without dimming, repainting as the app does,
 *     and the canvas is compared after every move with one repainted
 *     whole.
 *
 *   - THE POINTER. mj_hit() works from geometry; the picture is drawn
 *     by other code. Each tile is drawn alone to see exactly which
 *     pixels it covers, the topmost owner of every pixel is worked out
 *     in drawing order, and mj_hit() must agree on every pixel of the
 *     board.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mj_board.h"
#include "../mj_layout.h"

static int checks, failures;

static void check(bool ok, const char *what)
{
	checks++;
	if (ok) return;
	failures++;
	printf("FAIL: %s\n", what);
}

static uint32_t lcg = 1;
static uint32_t rnd(uint32_t n)
{
	lcg = lcg * 1103515245u + 12345u;
	return (lcg >> 8) % n;
}

/* -- the canvas against a reference ----------------------------------------- */

#define RW 100
#define RH 37
static int ref[RH][RW];         /* an index per pixel */
static uint32_t mem[16801], mem2[16801];

static int cx0, cy0, cx1, cy1;  /* the reference's clip */

static void ref_px(int x, int y, int c)
{
	if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1) return;
	ref[y][x] = c;
}

/* What the canvas should hold at (x, y), from the reference. */
static int expect(const mj_canvas_t *cv, int x, int y)
{
	int c = ref[y][x];
	if (cv->planes) return c & ((1 << cv->planes) - 1);
	{
		int i = mj_ink_map[c & 63];
		return (mj_inks[i & 15][y & 7] >> (7 - (x & 7))) & 1;
	}
}

static void test_canvas(int planes)
{
	mj_canvas_t cv;
	int round, x, y, bad = 0;
	char what[64];

	mj_cv_setup(&cv, mem, 16801, RW, RH, planes, mj_inks, mj_ink_map);
	mj_cv_fill(&cv, 0, 0, RW, RH, 0);
	memset(ref, 0, sizeof(ref));
	cx0 = cy0 = 0; cx1 = RW; cy1 = RH;

	for (round = 0; round < 4000; round++) {
		int op = (int)rnd(7), c = (int)rnd(planes >= 6 ? 64 : 16);
		int ax = (int)rnd(RW + 40) - 20, ay = (int)rnd(RH + 20) - 10;
		int w = (int)rnd(70), h = (int)rnd(20);

		if (rnd(8) == 0) {
			int qx = (int)rnd(RW), qy = (int)rnd(RH);
			int qw = (int)rnd(RW), qh = (int)rnd(RH);
			mj_cv_clip(&cv, qx, qy, qw, qh);
			cx0 = qx; cy0 = qy;
			cx1 = qx + qw > RW ? RW : qx + qw;
			cy1 = qy + qh > RH ? RH : qy + qh;
		} else if (rnd(10) == 0) {
			mj_cv_noclip(&cv);
			cx0 = cy0 = 0; cx1 = RW; cy1 = RH;
		}

		switch (op) {
		case 0:
			mj_cv_fill(&cv, ax, ay, w, h, c);
			for (y = ay; y < ay + h; y++)
				for (x = ax; x < ax + w; x++) ref_px(x, y, c);
			break;
		case 1: {
			uint32_t bits = (rnd(65536) << 16) ^ rnd(65536);
			int n = 1 + (int)rnd(32), i;
			mj_cv_row(&cv, ax, ay, bits, n, c);
			for (i = 0; i < n; i++)
				if ((bits >> i) & 1) ref_px(ax + i, ay, c);
			break;
		}
		case 2: {
			uint32_t rows[12];
			int bw = 1 + (int)rnd(32), bh = 1 + (int)rnd(12), i, r;
			for (r = 0; r < bh; r++) rows[r] = (rnd(65536) << 16) ^ rnd(65536);
			mj_cv_bits(&cv, rows, bw, bh, ax, ay, c);
			for (r = 0; r < bh; r++)
				for (i = 0; i < bw; i++)
					if ((rows[r] >> i) & 1) ref_px(ax + i, ay + r, c);
			break;
		}
		case 3: {
			/* a shade: one bit of the index set, in colour */
			int bit = (int)rnd(8) - 1;     /* -1 .. 6: some past the planes */
			if (!planes) break;            /* mono's shade is a pattern: below */
			mj_cv_shade(&cv, ax, ay, w, h, bit);
			if (bit < 0 || bit >= planes) break;
			for (y = ay; y < ay + h; y++)
				for (x = ax; x < ax + w; x++)
					if (x >= cx0 && y >= cy0 && x < cx1 && y < cy1)
						ref[y][x] |= 1 << bit;
			break;
		}
		case 5: {
			/* several colours in one row */
			uint32_t bits[3], all = (rnd(65536) << 16) ^ rnd(65536);
			uint8_t cc[3];
			int n = 1 + (int)rnd(32), i, k;
			for (k = 0; k < 3; k++) {
				uint32_t r = (rnd(65536) << 16) ^ rnd(65536);
				bits[k] = all & r;
				all &= ~r;
				cc[k] = (uint8_t)rnd(planes >= 6 ? 64 : 16);
			}
			mj_cv_row_multi(&cv, ax, ay, bits, cc, 3, n);
			for (k = 0; k < 3; k++)
				for (i = 0; i < n; i++)
					if ((bits[k] >> i) & 1) ref_px(ax + i, ay, cc[k]);
			break;
		}
		case 6: {
			/* a stamp: random planes and a random mask, 2 words wide */
			static uint32_t st[(6 + 1) * 2 * 12];
			int np = planes ? planes : 1, sh = 1 + (int)rnd(12), sw = 1 + (int)rnd(64);
			int r, j, i, k;
			for (i = 0; i < (np + 1) * 2 * sh; i++) st[i] = (rnd(65536) << 16) ^ rnd(65536);
			mj_cv_stamp(&cv, st, 2, sw, sh, ax, ay);
			for (r = 0; r < sh; r++)
				for (j = 0; j < 2; j++)
					for (i = 0; i < 32; i++) {
						int c = 0, px = ax + 32 * j + i;
						if (32 * j + i >= sw) continue;
						if (!((st[np * 2 * sh + r * 2 + j] >> i) & 1)) continue;
						for (k = 0; k < np; k++)
							c |= (int)((st[k * 2 * sh + r * 2 + j] >> i) & 1) << k;
						if (!planes) {
							/* mono: the stamp's bit is the pixel, not an index */
							if (px >= cx0 && ay + r >= cy0 && px < cx1 && ay + r < cy1)
								ref[ay + r][px] = c ? 6 : 0;   /* FACE solid, BG black */
						} else {
							ref_px(px, ay + r, c);
						}
					}
			break;
		}
		default: {
			static const char *const s[] = { "Turtle", "#4711", "144 left", "Hint" };
			const char *t = s[rnd(4)];
			int n = (int)strlen(t), k, r;
			mj_cv_text(&cv, ax, ay, t, c, &z_font_5x8);
			for (k = 0; k < n; k++) {
				int gi = z_font_index(&z_font_5x8, (uint8_t)t[k]);
				const uint8_t *g = z_font_5x8.glyphs + gi * 8;
				for (r = 0; r < 8; r++)
					for (x = 0; x < 5; x++)
						if (g[r] & (0x80 >> x)) ref_px(ax + k * 5 + x, ay + r, c);
			}
			break;
		}
		}
	}
	for (y = 0; y < RH; y++)
		for (x = 0; x < RW; x++)
			if (mj_cv_get(&cv, x, y) != expect(&cv, x, y)) bad++;
	snprintf(what, sizeof(what), "the canvas in %d planes matches the reference (%d differ)", planes, bad);
	check(bad == 0, what);

	/* mono's shade: lit pixels under a checkerboard go dark, the rest stay */
	if (!planes) {
		mj_cv_noclip(&cv);
		mj_cv_fill(&cv, 0, 0, RW, RH, MJ_C_FACE);
		mj_cv_shade(&cv, 3, 2, 50, 20, 0);
		bad = 0;
		for (y = 0; y < RH; y++)
			for (x = 0; x < RW; x++) {
				bool in = x >= 3 && x < 53 && y >= 2 && y < 22;
				int want = in ? !(((x + y) & 1) == 0 ? 0 : 1) : 1;
				want = in ? (((y & 1) ? ((x & 1) == 0) : ((x & 1) == 1))) : 1;
				if (mj_cv_get(&cv, x, y) != want) bad++;
			}
		check(bad == 0, "mono shade is a checkerboard over lit pixels");
	}
}

/* -- partial repaints ------------------------------------------------------------ */

static mj_layout_t L;
static mj_game_t G;
static mj_view_t V;

static void load(int i)
{
	char err[64];
	if (!mj_layout_parse(mj_layout_builtin_text(i), &L, err, sizeof(err))) {
		printf("layout %d: %s\n", i, err);
		exit(1);
	}
}

static void repaint_tile(mj_canvas_t *cv, int t)
{
	int x, y, w, h;
	if (t < 0) return;
	mj_tile_box(&V, t, &x, &y, &w, &h);
	mj_render(&V, cv, x, y, w, h);
}

static int words_differ(int n)
{
	int i;
	for (i = 0; i < n; i++)
		if (mem[i] != mem2[i]) return i + 1;
	return 0;
}

static int game_partials(int layout, int corner, int size, int planes, bool dim, int w, int h)
{
	mj_canvas_t a, b;
	static uint8_t before[MJ_MAX], after[MJ_MAX];
	int p, bad = 0, n = mj_cv_words(w, h, planes), moves = 0;

	load(layout);
	mj_new(&G, &L, (uint32_t)(layout * 100 + corner * 10 + size + 1), MJ_DEAL_SOLVABLE);
	mj_view_init(&V, &G);
	V.corner = corner;
	V.size = size;
	V.rich = planes >= 6;
	V.show_free = dim;
	mj_view_layout(&V, w, h);
	if (V.scrolls) mj_view_scroll(&V, 37, 23);
	mj_cv_setup(&a, mem, 16801, w, h, planes, mj_inks, mj_ink_map);
	mj_cv_setup(&b, mem2, 16801, w, h, planes, mj_inks, mj_ink_map);
	mj_render_all(&V, &a);

	for (p = 0; p + 1 < G.nsol; p += 2) {
		int ta = G.sol[p], tb = G.sol[p + 1], t;
		int old_sel = V.sel, old_cur = V.cursor;

		/* pick one up, move the cursor, as play does */
		V.sel = ta;
		V.cursor = tb;
		repaint_tile(&a, old_sel);
		repaint_tile(&a, old_cur);
		repaint_tile(&a, ta);
		repaint_tile(&a, tb);

		mj_free_flags(&G, before);
		if (!mj_take(&G, ta, tb)) { bad++; break; }
		mj_free_flags(&G, after);
		V.sel = -1;
		repaint_tile(&a, ta);
		repaint_tile(&a, tb);
		if (dim)
			for (t = 0; t < L.n; t++)
				if (before[t] != after[t] && mj_present(&G, t)) repaint_tile(&a, t);

		/* every few moves, compare with a repaint from nothing */
		if ((moves++ % 6) == 0 || G.left == 0) {
			mj_render_all(&V, &b);
			mj_render_bar(&V, &a);
			if (words_differ(n)) bad++;
		}
	}
	return bad;
}

static void test_partials(void)
{
	static const int planes[4] = { 0, 3, 4, 6 };
	int l, c, s, k, bad = 0, runs = 0;
	char what[96];

	for (l = 0; l < mj_layout_builtin_count(); l++)
		for (c = 0; c < 4; c++)
			for (s = 0; s < MJ_ART_SIZES; s++)
				for (k = 0; k < 4; k++) {
					int b = game_partials(l, c, s, planes[k], (k + c) & 1, 320, 240);
					runs++;
					if (b) {
						snprintf(what, sizeof(what),
							 "partial repaints: layout %d corner %d size %d planes %d (%d)",
							 l, c, s, planes[k], b);
						check(false, what);
					}
					bad += b;
				}
	/* and the desktop's odd sizes, where rows do not end on a word */
	bad += game_partials(0, 0, MJ_ART_LARGE, 0, false, 471, 351);
	bad += game_partials(2, 1, MJ_ART_SMALL, 0, true, 333, 250);
	runs += 2;
	snprintf(what, sizeof(what), "%d whole games repainted in parts match full repaints", runs);
	check(bad == 0, what);
}

/* -- covered tiles are skipped, and nothing shows it ---------------------------------
 *
 * mj_render() leaves out a tile with a tile on top and its three
 * neighbours on the sides' side all there. The argument that nothing it
 * would draw survives is in mj_board.c; this is the proof by picture:
 * every layout, corner, size and depth, through whole games, drawn both
 * ways and compared word for word. */
static void test_cull(void)
{
	static const int planes[4] = { 0, 3, 4, 6 };
	int l, c, s, k, bad = 0, runs = 0, skipped = 0;
	char what[96];

	for (l = 0; l < mj_layout_builtin_count(); l++)
		for (c = 0; c < 4; c++)
			for (s = 0; s < MJ_ART_SIZES; s++)
				for (k = 0; k < 4; k++) {
					mj_canvas_t a, b;
					int p, n = mj_cv_words(320, 240, planes[k]);
					load(l);
					mj_new(&G, &L, (uint32_t)(l * 7 + c + 1), MJ_DEAL_SOLVABLE);
					mj_view_init(&V, &G);
					V.corner = c;
					V.size = s;
					V.rich = planes[k] >= 6;
					V.show_free = k & 1;
					mj_view_layout(&V, 320, 240);
					mj_cv_setup(&a, mem, 16801, 320, 240, planes[k], mj_inks, mj_ink_map);
					mj_cv_setup(&b, mem2, 16801, 320, 240, planes[k], mj_inks, mj_ink_map);
					for (p = 0; p <= G.nsol; p += 12) {
						int t, q;
						for (t = 0; t < L.n; t++) {
							for (q = 0; q < 4; q++)
								if (V.cover[t][q] < 0 || !mj_present(&G, V.cover[t][q])) break;
							if (q == 4 && mj_present(&G, t)) skipped++;
						}
						mj_board_cull = 1;
						mj_render_all(&V, &a);
						mj_board_cull = 0;
						mj_render_all(&V, &b);
						mj_board_cull = 1;
						if (words_differ(n)) bad++;
						runs++;
						for (q = p; q < p + 12 && q + 1 < G.nsol; q += 2)
							mj_take(&G, G.sol[q], G.sol[q + 1]);
					}
				}
	snprintf(what, sizeof(what), "skipping covered tiles changes nothing (%d pictures, %d tiles skipped)",
		 runs, skipped);
	check(bad == 0 && skipped > 1000, what);
}

/* -- scrolling moves the picture and draws only the new strip ----------------------
 *
 * mj_render_scrolled() shifts the canvas and draws the strips that came
 * into view. It must leave exactly the picture a full repaint at the
 * new position gives -- in every depth, from every corner, for moves
 * in every direction, and in mono where only some moves keep the
 * dithers in step (the others it must refuse, not get wrong). */
static void test_scroll(void)
{
	static const int planes[4] = { 0, 3, 4, 6 };
	int c, k, i, bad = 0, done = 0, refused = 0;
	char what[96];

	load(0);
	mj_new(&G, &L, 31, MJ_DEAL_SOLVABLE);
	for (i = 0; i < 30; i += 2) mj_take(&G, G.sol[i], G.sol[i + 1]);
	for (c = 0; c < 4; c++)
		for (k = 0; k < 4; k++) {
			mj_canvas_t a, b;
			int n = mj_cv_words(320, 240, planes[k]);
			mj_view_init(&V, &G);
			V.corner = c;
			V.size = MJ_ART_LARGE;
			V.rich = planes[k] >= 6;
			V.sel = G.sol[31];
			mj_view_layout(&V, 320, 240);
			mj_cv_setup(&a, mem, 16801, 320, 240, planes[k], mj_inks, mj_ink_map);
			mj_cv_setup(&b, mem2, 16801, 320, 240, planes[k], mj_inks, mj_ink_map);
			mj_render_all(&V, &a);
			for (i = 0; i < 40; i++) {
				int ox = V.scroll_x, oy = V.scroll_y;
				int dx = (int)rnd(9) * 4 - 16, dy = (int)rnd(9) * 4 - 16;
				if (rnd(4) == 0) { dx = (int)rnd(61) - 30; dy = (int)rnd(41) - 20; }
				if (!mj_view_scroll(&V, dx, dy)) continue;
				if (!mj_render_scrolled(&V, &a, V.scroll_x - ox, V.scroll_y - oy)) {
					refused++;
					mj_render_all(&V, &a);
				}
				mj_render_all(&V, &b);
				/* the bar is not the scroll's: copy a's into b's place */
				mj_render_bar(&V, &a);
				if (words_differ(n)) bad++;
				done++;
			}
		}
	snprintf(what, sizeof(what), "scrolled pictures match full repaints (%d moves, %d refused)", done, refused);
	check(bad == 0 && done > 300, what);
}

/* -- the pointer ------------------------------------------------------------------- */

static int8_t  cover[240][320];
static int16_t owner[240][320];

static void test_hit(int layout, int corner, int size)
{
	static mj_game_t one;
	mj_canvas_t cv;
	int i, x, y, bad = 0, t;
	char what[96];

	load(layout);
	mj_new(&G, &L, 7, MJ_DEAL_SOLVABLE);
	/* a few pairs gone, so holes and exposed lower tiles are tested */
	for (i = 0; i < 20; i += 2) mj_take(&G, G.sol[i], G.sol[i + 1]);
	mj_view_init(&V, &G);
	V.corner = corner;
	V.size = size;
	mj_view_layout(&V, 320, 240);
	mj_cv_setup(&cv, mem, 16801, 320, 240, 4, mj_inks, mj_ink_map);

	for (y = 0; y < 240; y++)
		for (x = 0; x < 320; x++) owner[y][x] = -1;

	/* each tile alone on the table, in drawing order */
	one = G;
	V.g = &one;
	for (i = 0; i < V.norder; i++) {
		int u = V.order[i];
		if (!mj_present(&G, u)) continue;
		for (t = 0; t < L.n; t++) one.face[t] = t == u ? G.face[u] : MJ_GONE;
		mj_render(&V, &cv, 0, 0, 320, V.area_h);
		for (y = 0; y < V.area_h; y++)
			for (x = 0; x < 320; x++) {
				int c = mj_cv_get(&cv, x, y);
				cover[y][x] = c != MJ_C_BG && c != MJ_C_BG2;
				if (cover[y][x]) owner[y][x] = (int16_t)u;
			}
	}
	V.g = &G;

	for (y = 0; y < V.area_h; y++)
		for (x = 0; x < 320; x++)
			if (mj_hit(&V, x, y) != owner[y][x]) bad++;
	snprintf(what, sizeof(what), "hits agree with the picture: layout %d corner %d size %d (%d px differ)",
		 layout, corner, size, bad);
	check(bad == 0, what);
}

/* -- the keyboard ------------------------------------------------------------------- */

static void test_nav(void)
{
	int t, d, bad = 0, moved = 0;
	load(0);
	mj_new(&G, &L, 3, MJ_DEAL_SOLVABLE);
	mj_view_init(&V, &G);
	mj_view_layout(&V, 320, 240);

	t = mj_nav_first(&V);
	check(t >= 0 && mj_free(&G, t), "the cursor starts on a free tile");

	for (t = 0; t < L.n; t++) {
		int fx, fy;
		if (!mj_present(&G, t)) continue;
		mj_tile_face(&V, t, &fx, &fy);
		for (d = 0; d < 4; d++) {
			int u = mj_nav(&V, t, d), ux, uy, k;
			if (u < 0) continue;
			moved++;
			mj_tile_face(&V, u, &ux, &uy);
			for (k = 0; k < L.nabove[u]; k++)
				if (mj_present(&G, L.above[u][k])) bad++;
			if (d == 0 && !(ux < fx)) bad++;
			if (d == 1 && !(ux > fx)) bad++;
			if (d == 2 && !(uy < fy)) bad++;
			if (d == 3 && !(uy > fy)) bad++;
		}
	}
	check(moved > 100, "the arrows go somewhere");

	/* Tab: once round visits every free tile exactly once, either way */
	{
		int k, nfree = 0, step;
		for (t = 0; t < L.n; t++) if (mj_free(&G, t)) nfree++;
		for (step = -1; step <= 1; step += 2) {
			static uint8_t seen[MJ_MAX];
			int cur = -1, dup = 0, visits = 0;
			memset(seen, 0, sizeof(seen));
			for (k = 0; k < nfree; k++) {
				cur = mj_nav_tab(&V, cur, step);
				if (cur < 0 || !mj_free(&G, cur)) { dup++; break; }
				if (seen[cur]++) dup++;
				visits++;
			}
			check(dup == 0 && visits == nfree,
			      step > 0 ? "Tab visits every free tile once" : "shift-Tab visits every free tile once");
			check(mj_nav_tab(&V, cur, step) == mj_nav_tab(&V, -1, step), "and comes round to the start");
		}
	}
	check(bad == 0, "every arrow lands ahead, on a tile with nothing on it");
}

/* -- fitting and scrolling --------------------------------------------------------- */

static void test_fit(void)
{
	int l;
	for (l = 0; l < mj_layout_builtin_count(); l++) {
		char what[64];
		load(l);
		mj_new(&G, &L, 1, MJ_DEAL_SOLVABLE);
		snprintf(what, sizeof(what), "built-in %d fits 320x240 whole at the small size", l);
		check(mj_view_fits(&G, MJ_ART_SMALL, 320, 240), what);
		snprintf(what, sizeof(what), "built-in %d does not fit 320x240 at the large size", l);
		check(!mj_view_fits(&G, MJ_ART_LARGE, 320, 240), what);
	}
	load(0);
	mj_new(&G, &L, 1, MJ_DEAL_SOLVABLE);
	check(mj_view_fits(&G, MJ_ART_LARGE, 462, 341), "the Turtle fits the default window at the large size");

	mj_view_init(&V, &G);
	V.size = MJ_ART_LARGE;
	mj_view_layout(&V, 320, 240);
	check(V.scrolls, "close up, the Turtle scrolls");
	check(!mj_view_scroll(&V, -10, -10), "no scrolling past the top left");
	check(mj_view_scroll(&V, 10000, 10000), "scrolls to the far corner");
	check(!mj_view_scroll(&V, 10, 10), "and no further");
	{
		int x, y, w, h, t;
		bool all_in = true;
		for (t = 0; t < L.n; t++) {
			mj_view_reveal(&V, t);
			mj_tile_face(&V, t, &x, &y);
			w = V.fw + V.d;
			h = V.fh + V.d;
			if (x < 0 || y < 0 || x + w > 320 || y + h > V.area_h) all_in = false;
		}
		check(all_in, "revealing any tile brings it, sides and all, into view");
	}
	V.size = MJ_ART_SMALL;
	mj_view_layout(&V, 320, 240);
	check(!V.scrolls, "whole, the Turtle does not scroll");
}

int main(void)
{
	test_canvas(0);
	test_canvas(3);
	test_canvas(4);
	test_canvas(6);
	test_fit();
	test_nav();
	test_hit(0, 0, MJ_ART_SMALL);
	test_hit(0, 2, MJ_ART_SMALL);
	test_hit(3, 1, MJ_ART_SMALL);
	test_hit(2, 3, MJ_ART_LARGE);
	test_partials();
	test_cull();
	test_scroll();
	printf("board_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
