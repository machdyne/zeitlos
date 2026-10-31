/*
 * Zeitlos mahjong -- draw the board on the build machine, to look at.
 *
 *   cd sw/apps/mahjong && make render        -> tests/out, .ppm and .png
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The canvas is the whole picture on both faces -- the screen face
 * copies it plane by plane, the desktop face blits it -- so drawing the
 * canvas here is drawing what the board will show, pixel for pixel,
 * less only the copy. What a test cannot judge and a look can: whether
 * the stacks read as stacks, whether the shadows help or muddy, whether
 * the selection, the hint and the cursor are told apart in colour and
 * in black and white, and whether the views from the other corners look
 * like the same table.
 *
 * Every picture also gets checked for one thing a look would miss: a
 * repaint of a few small rectangles (what a match does) must leave the
 * canvas identical to a repaint of the whole -- the painter's order
 * and the boxes have to agree for that, and when they do not, a match
 * leaves a ghost of a shadow or a missing side on hardware.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mj_board.h"
#include "../mj_layout.h"

static mj_layout_t L;
static mj_game_t   G;
static mj_view_t   V;
static uint32_t    mem[16800], mem2[16800];
static int         failures;

static void write_ppm(const char *path, const mj_canvas_t *cv, int scale)
{
	FILE *f = fopen(path, "wb");
	int x, y, sx, sy;
	if (!f) { perror(path); exit(1); }
	fprintf(f, "P6\n%d %d\n255\n", cv->w * scale, cv->h * scale);
	for (y = 0; y < cv->h; y++)
		for (sy = 0; sy < scale; sy++)
			for (x = 0; x < cv->w; x++) {
				int v = mj_cv_get(cv, x, y);
				int r, g, b;
				if (!cv->planes) {
					r = g = b = v ? 0xee : 0x11;
				} else {
					uint16_t c;
					if (cv->planes == 3) v &= 7;
					c = mj_palette[v];
					r = ((c >> 8) & 15) * 17;
					g = ((c >> 4) & 15) * 17;
					b = (c & 15) * 17;
				}
				for (sx = 0; sx < scale; sx++) {
					fputc(r, f); fputc(g, f); fputc(b, f);
				}
			}
	fclose(f);
}

/* Takes two pairs, repainting only their boxes in one canvas and
 * everything in the other, and compares. */
static void check_partial(int w, int h, int planes, const char *what)
{
	mj_canvas_t a, b;
	uint16_t pa[4], pb[4];
	int n, k, i, words = mj_cv_words(w, h, planes);

	mj_cv_setup(&a, mem, 16800, w, h, planes, mj_inks, mj_ink_map);
	mj_cv_setup(&b, mem2, 16800, w, h, planes, mj_inks, mj_ink_map);
	mj_render_all(&V, &a);
	n = mj_pairs(&G, pa, pb, 4);
	for (k = 0; k < n && k < 2; k++) {
		static uint8_t before[MJ_MAX], after[MJ_MAX];
		int t[2], j, u;
		t[0] = pa[k]; t[1] = pb[k];
		mj_free_flags(&G, before);
		if (!mj_take(&G, t[0], t[1])) continue;
		mj_free_flags(&G, after);
		for (j = 0; j < 2; j++) {
			int x, y, bw, bh;
			mj_tile_box(&V, t[j], &x, &y, &bw, &bh);
			mj_render(&V, &a, x, y, bw, bh);
		}
		/* ...and, when dimming, what the move freed */
		if (V.show_free)
			for (u = 0; u < L.n; u++)
				if (before[u] != after[u] && mj_present(&G, u)) {
					int x, y, bw, bh;
					mj_tile_box(&V, u, &x, &y, &bw, &bh);
					mj_render(&V, &a, x, y, bw, bh);
				}
	}
	mj_render_all(&V, &b);
	for (i = 0; i < words; i++)
		if (mem[i] != mem2[i]) break;
	if (i < words) {
		printf("FAIL: %s: a partial repaint differs from a full one (word %d)\n", what, i);
		failures++;
	}
	while (k-- > 0) mj_undo(&G);
}

static void shot(const char *dir, const char *name, int w, int h, int planes, int scale)
{
	char path[256];
	mj_canvas_t cv;
	mj_cv_setup(&cv, mem, 16800, w, h, planes, mj_inks, mj_ink_map);
	mj_view_layout(&V, w, h);
	mj_render_all(&V, &cv);
	snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
	write_ppm(path, &cv, scale);
	check_partial(w, h, planes, name);
	mj_view_layout(&V, w, h);
}

static void deal(int layout, uint32_t seed)
{
	char err[64];
	if (!mj_layout_parse(mj_layout_builtin_text(layout), &L, err, sizeof(err))) {
		printf("layout: %s\n", err);
		exit(1);
	}
	mj_new(&G, &L, seed, MJ_DEAL_SOLVABLE);
	mj_view_init(&V, &G);
	strcpy(V.status, "Turtle  #4711  144 left  0:00");
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/out";
	int i, a, b;

	/* The screen face, whole: the Turtle at the small size, 16 colours. */
	deal(0, 4711);
	V.size = MJ_ART_SMALL;
	shot(dir, "screen_16", 320, 240, 4, 3);
	V.rich = true;
	shot(dir, "screen_64", 320, 240, 6, 3);
	V.rich = false;
	shot(dir, "screen_8", 320, 240, 3, 3);
	shot(dir, "screen_mono", 320, 240, 0, 3);

	/* Play a little: a selection, a hint, the cursor, some pairs gone. */
	for (i = 0; i < 12; i++) {
		uint16_t pa, pb;
		if (mj_pairs(&G, &pa, &pb, 1)) mj_take(&G, pa, pb);
	}
	mj_hint(&G, &a, &b);
	V.hint_a = a; V.hint_b = b;
	V.sel = mj_nav_first(&V);
	V.cursor = mj_nav(&V, V.sel, 1);
	snprintf(V.msg, sizeof(V.msg), "That tile is not free");
	shot(dir, "play_16", 320, 240, 4, 3);
	V.rich = true;
	shot(dir, "play_64", 320, 240, 6, 3);
	V.rich = false;
	shot(dir, "play_mono", 320, 240, 0, 3);
	V.show_free = true;
	shot(dir, "play_free", 320, 240, 4, 3);
	shot(dir, "play_free_mono", 320, 240, 0, 3);
	V.show_free = false;
	V.msg[0] = 0;

	/* The other corners. */
	for (i = 1; i < 4; i++) {
		char n[32];
		V.corner = i;
		snprintf(n, sizeof(n), "corner%d", i);
		V.rich = true;
		shot(dir, n, 320, 240, 6, 3);
	}
	V.corner = 0;
	V.rich = false;

	/* Close: the large tiles on the screen, scrolled to the middle. */
	V.size = MJ_ART_LARGE;
	V.buttons |= 1 << MJ_B_ZOOM;
	mj_view_layout(&V, 320, 240);
	mj_view_scroll(&V, 60, 60);
	shot(dir, "close_16", 320, 240, 4, 3);

	/* The desktop face: mono, large tiles, a 480x360 window. */
	shot(dir, "desktop_large", 476, 340, 0, 2);
	V.size = MJ_ART_SMALL;
	shot(dir, "desktop_small", 316, 226, 0, 2);

	/* The other layouts. */
	for (i = 1; i < mj_layout_builtin_count(); i++) {
		char n[32];
		deal(i, 99);
		V.rich = true;
		snprintf(n, sizeof(n), "layout%d", i);
		shot(dir, n, 320, 240, 6, 3);
	}

	printf("render: pictures in %s, %d failures\n", dir, failures);
	return failures ? 1 : 0;
}
