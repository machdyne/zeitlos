/*
 * Zeitlos mahjong -- the screen face through the arcade runtime, on the
 * build machine.
 *
 *   cd sw/apps/mahjong && make screen    -> tests/out/scr_*.ppm (and .png)
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The runtime's host build (sw/common/games/tests/ar_host.c) is a
 * framebuffer with the real zarcade.c on it: the window in black and
 * white, the colour window, full screen in colour and in mono, another
 * window over ours. This runs mj_face.c -- the same paint() and the
 * same flush the app uses -- and a session through all of them, keys
 * and clicks included, with AR_HOST_VERIFY on: after every frame the
 * runtime repaints everything from nothing and compares. A move whose
 * rectangles miss something it changed fails here; on hardware it
 * would be a stale corner until the next full repaint.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* zgfx's primitives in software, for the window layer the runtime's
 * host build links (sw/common/tests/zrender.h) */
#include "../../../common/tests/zrender.h"

#include "../mj_face.h"
#include "../../../common/games/tests/ar_host.h"

static mj_play_t P;
static int nb;

static const char *src(void *user, int i, char *buf, int cap)
{
	(void)user; (void)buf; (void)cap;
	return i < nb ? mj_layout_builtin_text(i) : NULL;
}

static ar_desc_t desc = {
	.name = "mahjong",
	.paint = mj_face_paint,
	.pointer = true,
};

static void show(int x, int y, int w, int h) { ar_dirty(x, y, w, h); }

static void flush(void)
{
	if (!mj_face_ready) {
		P.nrect = 0;
		P.all = P.relayout = P.bar = false;
		P.sdx = P.sdy = 0;
		return;
	}
	mj_face_flush(&P, true, show);
	P.nrect = 0;
	P.save = false;
	P.say[0] = 0;
	P.sound = 0;
	P.burst[0] = P.burst[1] = -1;
}

/* Clicks a point of tile t, as the pointer would. */
static void click_tile(int t)
{
	int fx, fy, x, y;
	mj_tile_face(&P.v, t, &fx, &fy);
	for (y = fy; y < fy + P.v.fh; y++)
		for (x = fx; x < fx + P.v.fw; x++)
			if (mj_hit(&P.v, x, y) == t) {
				mj_play_click(&P, x, y, 5);
				return;
			}
}

/* A press and a release of the left button at (x, y), through the
 * runtime's own queue -- what wm's mouse messages become. */
static void push_click(int x, int y, bool down)
{
	ar_event_t m;
	memset(&m, 0, sizeof(m));
	m.type = AR_EV_MOUSE;
	m.x = (int16_t)x;
	m.y = (int16_t)y;
	m.buttons = down ? 1 : 0;
	m.inside = 1;
	ar_push(&m);
}

static uint32_t seed_before;
static int left_before;
static int button_fails;

static void press_button(int b, bool down)
{
	push_click(P.v.bx[b] + P.v.bwid[b] / 2, P.v.bar_y + MJ_BAR_H / 2, down);
}

/* Things the script cannot say with keys: pairs taken by clicking, and
 * the bar's buttons pressed with the pointer, full screen. */
void ar_host_hook(uint32_t frame)
{
	uint16_t a, b;
	/* New, full screen (frames 16..): a new number dealt */
	if (frame == 17) { seed_before = P.g.seed; press_button(MJ_B_NEW, true); }
	if (frame == 18) press_button(MJ_B_NEW, false);
	if (frame == 20 && P.g.seed == seed_before) {
		printf("FAIL: New, clicked full screen, dealt nothing\n");
		button_fails++;
	}
	/* Undo after the pair taken at frame 21 */
	if (frame == 23) { left_before = P.g.left; press_button(MJ_B_UNDO, true); }
	if (frame == 24) press_button(MJ_B_UNDO, false);
	if (frame == 26 && P.g.left != left_before + 2) {
		printf("FAIL: Undo, clicked full screen, undid nothing (%d -> %d)\n", left_before, P.g.left);
		button_fails++;
	}
	if (frame == 9 || frame == 10 || frame == 21 || frame == 33 || frame == 45) {
		if (mj_pairs(&P.g, &a, &b, 1)) {
			click_tile(a);
			click_tile(b);
		}
	}
	if (frame == 27) mj_play_wheel(&P, 2, false);
	if (frame == 29) mj_play_wheel(&P, 3, true);    /* Shift + wheel: across */
	if (frame == 28) mj_play_hover(&P, 2, 100);
	mj_play_tick(&P, frame / 10);
	flush();
}

#define K(f, k)     { (f), ARH_TAP, (k), 0, NULL, 0, 0, 0, 0 }
#define SHOT(f, n)  { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define MODE(f, m)  { (f), (m), 0, 0, NULL, 0, 0, 0, 0 }
#define WCOL(f, on) { (f), ARH_WCOLOR, (on), 0, NULL, 0, 0, 0, 0 }
#define COVER(f, x, y, w, h) { (f), ARH_COVER, 0, 0, NULL, (x), (y), (w), (h) }
#define END(f)      { (f), ARH_END, 0, 0, NULL, 0, 0, 0, 0 }

static const ar_host_step_t script[] = {
	SHOT(3, "scr_window"),
	WCOL(4, 1),
	K(6, 'h'),
	SHOT(8, "scr_colour_window"),
	K(11, ARH_K_RIGHT),
	K(12, ARH_K_RIGHT),
	K(13, '\r'),
	SHOT(14, "scr_cursor"),
	MODE(16, ARH_FULL_COLOR),
	SHOT(19, "scr_full_64"),
	K(22, 'z'),
	SHOT(25, "scr_full_zoom"),
	K(30, 'v'),
	SHOT(32, "scr_full_corner"),
	K(34, 'z'),
	K(36, 'f'),
	MODE(38, ARH_FULL_MONO),
	SHOT(40, "scr_full_mono"),
	MODE(42, ARH_WINDOW),
	COVER(43, 100, 60, 80, 50),
	K(44, 'h'),
	COVER(47, 0, 0, 0, 0),
	SHOT(49, "scr_window_after"),
	END(52),
};

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/out";
	ar_event_t e;
	int i;
	bool quit = false;
	int last_planes = -1, twice = 0;

	mj_play_init(&P, src, NULL);
	nb = mj_layout_builtin_count();
	mj_play_add_layout(&P, "Turtle");
	for (i = 1; i < nb; i++) {
		static const char *const n[] = { "", "Pyramid", "Fortress", "Waves" };
		mj_play_add_layout(&P, n[i]);
	}
	P.now = 0;
	mj_play_layout(&P, 0, 4711);

	memcpy(desc.palette, mj_palette, sizeof(desc.palette));
	memcpy(desc.ink, mj_inks, sizeof(desc.ink));
	ar_window_colour(true);
	ar_host_run(script, dir);
	if (!ar_init(&desc)) return 1;
	ar_set_colours(64);
	ar_pal_load(mj_palette, MJ_C_COLOURS);
	mj_face_begin(false);

	while (!quit) {
		while (ar_poll(&e)) {
			if (e.type == AR_EV_QUIT) quit = true;
			else if (e.type == AR_EV_KEY && e.pressed) mj_play_key(&P, e.key, e.mods, 9);
			else if (e.type == AR_EV_MOUSE) {
				/* as mahjong.c's mouse(): the press edge */
				static bool was;
				bool down = (e.buttons & 1) != 0;
				if (down && !was) mj_play_click(&P, e.x, e.y, ar_host_frame() + 100);
				was = down;
			}
			else if (e.type == AR_EV_MODE) {
				ar_pal_load(mj_palette, MJ_C_COLOURS);
			}
		}
			if (mj_face_check(&P)) {
			/* a compose for a mode the canvas already held is
			 * one too many */
			if (mj_cv.planes == last_planes) twice++;
			last_planes = mj_cv.planes;
		}
		mj_face_pulse(&P, ar_host_frame());
		flush();
		if (!quit) ar_frame();
	}
	printf("screen: %d pictures in %s, %u frames verified, %u differ; %d left on the table; "
	       "composed %d times\n",
	       ar_host_shots(), dir, ar_host_verified(), ar_host_verify_bad(), P.g.left, mj_face_composes);
	/* Six pictures to compose, each once: the colour window (black and
	 * white skipped: colour comes within the wait), full screen in
	 * colour, in mono, the colour window again, covered (black and
	 * white), uncovered (colour). */
	if (twice || mj_face_composes > 6) {
		printf("FAIL: composed %d times, %d of them for a mode it already had\n",
		       mj_face_composes, twice);
		return 1;
	}
	return ar_host_verify_bad() || button_fails || P.g.left >= 144 ? 1 : 0;
}
