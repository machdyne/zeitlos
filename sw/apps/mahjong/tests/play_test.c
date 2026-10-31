/*
 * Zeitlos mahjong -- host tests for a session: what input does.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * mj_play.c is the part of the app that decides what a click or a key
 * means, so it is driven here exactly as mahjong.c drives it -- by
 * canvas coordinates and keysyms -- with no screen. Whole games are
 * won by clicking, and by the keyboard alone; the clock, the
 * statistics and stats.txt are checked; and a layout file that does
 * not parse must leave the table as it was.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mj_play.h"

static int checks, failures;

static void check(bool ok, const char *what)
{
	checks++;
	if (ok) return;
	failures++;
	printf("FAIL: %s\n", what);
}

static void check_eq(long got, long want, const char *what)
{
	checks++;
	if (got == want) return;
	failures++;
	printf("FAIL: %s -- got %ld, want %ld\n", what, got, want);
}

static mj_play_t P;

/* The layouts: the built-ins, then two "files" -- one good, one bad. */
static const char *good_file =
	"# a test\nname Little\nlayer\n[][][][]\n[][][][]\nlayer\n..[][]\n..[][]\n";
static const char *bad_file = "name Broken\nlayer\n[][]\n[]\n";
static int nbuiltin;

static const char *src(void *user, int i, char *buf, int cap)
{
	(void)user; (void)buf; (void)cap;
	if (i < nbuiltin) return mj_layout_builtin_text(i);
	if (i == nbuiltin) return good_file;
	if (i == nbuiltin + 1) return bad_file;
	return NULL;
}

static void out_clear(void)
{
	P.nrect = 0;
	P.all = P.relayout = P.bar = P.save = false;
	P.say[0] = 0;
	P.sound = 0;
	P.burst[0] = P.burst[1] = -1;
	P.sdx = P.sdy = 0;
}

static void setup(uint32_t seed)
{
	int i;
	mj_play_init(&P, src, NULL);
	nbuiltin = mj_layout_builtin_count();
	for (i = 0; i < nbuiltin; i++) {
		static const char *const names[] = { "Turtle", "Pyramid", "Fortress", "Waves" };
		mj_play_add_layout(&P, names[i]);
	}
	mj_play_add_layout(&P, "Little");
	mj_play_add_layout(&P, "Broken");
	P.now = 1000;
	mj_play_layout(&P, 0, seed);
	mj_view_layout(&P.v, 320, 240);
	out_clear();
}

/* A point where a click lands on tile t, or false. */
static bool point_on(int t, int *px, int *py)
{
	int fx, fy, x, y;
	mj_tile_face(&P.v, t, &fx, &fy);
	for (y = fy; y < fy + P.v.fh; y++)
		for (x = fx; x < fx + P.v.fw; x++)
			if (mj_hit(&P.v, x, y) == t) { *px = x; *py = y; return true; }
	return false;
}

static bool click_tile(int t)
{
	int x, y;
	if (!point_on(t, &x, &y)) return false;
	mj_play_click(&P, x, y, 777);
	return true;
}

static void test_click_game(void)
{
	int p, bad = 0, pairs = 0;
	setup(42);

	check(P.g.L != 0 && P.g.left == 144, "a Turtle is dealt");
	check(strstr(P.v.status, "Turtle #42") != 0, "the bar names the layout and the deal");

	/* a tile that is not free: says so, picks nothing */
	{
		int t;
		for (t = 0; t < P.L.n; t++)
			if (!mj_free(&P.g, t)) {
				int x, y;
				if (point_on(t, &x, &y)) {
					mj_play_click(&P, x, y, 1);
					break;
				}
			}
		check(P.v.sel == -1, "a blocked tile is not picked up");
		check(strstr(P.v.msg, "not free") != 0, "and the bar says why");
		check_eq(P.sound, MJ_SND_NOPE, "with a sound");
		out_clear();
	}

	/* the deal's own solution, by clicks, the clock running */
	for (p = 0; p < P.g.nsol; p += 2) {
		int a = P.g.sol[p], b = P.g.sol[p + 1];
		P.now += 3;
		mj_play_tick(&P, P.now);
		if (!click_tile(a)) { bad++; break; }
		if (P.v.sel != a) { bad++; break; }
		if (!click_tile(b)) { bad++; break; }
		if (mj_present(&P.g, a) || mj_present(&P.g, b)) { bad++; break; }
		if (P.burst[0] != a || P.burst[1] != b) bad++;
		if (!P.nrect && !P.all) bad++;
		pairs++;
		out_clear();
	}
	check_eq(bad, 0, "every pair of the solution taken by two clicks");
	check_eq(pairs, 72, "72 pairs");
	check_eq(mj_state(&P.g), MJ_WON, "the table is clear");
	check(strstr(P.v.msg, "Cleared in") != 0, "the bar says so");
	check(!P.timing, "the clock stops");
	/* first pair at 1003; the last pair after 72 * 3 seconds */
	check_eq((long)P.elapsed, 71 * 3, "the clock counted from the first pair");
	{
		const mj_stat_t *s = mj_play_stat(&P, "Turtle");
		check(s && s->played == 1 && s->won == 1 && s->best == 213, "statistics: one played, one won, the time");
	}

	/* undo out of a win: the clock runs on */
	out_clear();
	mj_play_key(&P, 'u', 0, 1);
	check_eq(P.g.left, 2, "undo after the win");
	check(P.timing, "and the clock runs again");
	mj_play_key(&P, 'r', 0, 1);
	check_eq(P.g.left, 0, "redo wins again");
	{
		const mj_stat_t *s = mj_play_stat(&P, "Turtle");
		check(s && s->won == 2, "a second win counted");
	}
}

/* -- the keyboard alone -------------------------------------------------------- */

/* Moves the cursor to tile t with the arrows, by always stepping in the
 * direction that most reduces the distance; gives up after 200. */
static int tab_used;

/* Tab round the free tiles until the cursor is on t: always possible
 * for a free tile, within as many presses as there are free tiles. */
static bool tab_to(int t)
{
	int n;
	for (n = 0; n < 200 && P.v.cursor != t; n++) mj_play_key(&P, '\t', 0, 1);
	tab_used++;
	return P.v.cursor == t;
}

static bool steer_to(int t)
{
	int n;
	if (P.v.cursor < 0) mj_play_key(&P, 0x110003, 0, 1);
	for (n = 0; n < 200 && P.v.cursor != t; n++) {
		int cx, cy, tx, ty, dir;
		mj_tile_face(&P.v, P.v.cursor, &cx, &cy);
		mj_tile_face(&P.v, t, &tx, &ty);
		{
			int dx = tx - cx, dy = ty - cy;
			int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
			if (adx >= ady) dir = dx < 0 ? 0 : 1;
			else dir = dy < 0 ? 2 : 3;
			/* a step the arrows cannot take: try the other axis */
			if (mj_nav(&P.v, P.v.cursor, dir) < 0) {
				if (adx >= ady && dy) dir = dy < 0 ? 2 : 3;
				else if (dx) dir = dx < 0 ? 0 : 1;
			}
		}
		{
			static const uint32_t keys[4] = { 0x110002, 0x110003, 0x110000, 0x110001 };
			int before = P.v.cursor;
			mj_play_key(&P, keys[dir], 0, 1);
			if (P.v.cursor == before) return false;
		}
	}
	return P.v.cursor == t;
}

static void test_keyboard_game(void)
{
	int p, bad = 0, lost = 0;
	setup(7);

	mj_play_key(&P, 0x110003, 0, 1);
	check(P.v.cursor >= 0, "an arrow puts the cursor on a tile");
	check(P.keyboard, "and the keyboard is in use");
	check(P.say[0] != 0, "the tile under the cursor is spoken");

	for (p = 0; p < P.g.nsol; p += 2) {
		int a = P.g.sol[p], b = P.g.sol[p + 1];
		if (!steer_to(a)) { lost++; if (!tab_to(a)) { bad++; continue; } }
		mj_play_key(&P, '\r', 0, 1);
		if (P.v.sel != a) { bad++; continue; }
		if (!steer_to(b)) { lost++; if (!tab_to(b)) { bad++; continue; } }
		mj_play_key(&P, '\r', 0, 1);
		if (mj_present(&P.g, a)) bad++;
		out_clear();
	}
	/* The arrows move between tiles with nothing on them, by position;
	 * this steering is greedy, so it may fail to reach one now and then.
	 * The game must still be winnable by them -- the rest by clicks. */
	check_eq(bad, 0, "the keyboard alone wins: arrows, Tab where they miss, Enter");
	check_eq(mj_state(&P.g), MJ_WON, "the table is clear");
	{
		char what[80];
		snprintf(what, sizeof(what), "greedy arrows reach most tiles (%d of 144 needed Tab)", lost);
		check(lost <= 20, what);
		printf("  keyboard game: %d of 144 tiles reached by Tab after the arrows missed\n", lost);
	}
	check(P.v.sel < 0 || mj_present(&P.g, P.v.sel), "the selection is a tile on the table");
}

/* -- the other keys ----------------------------------------------------------------- */

static void test_keys(void)
{
	int a, b;
	setup(99);

	mj_play_key(&P, 'h', 0, 1);
	check(P.v.hint_a >= 0 && P.v.hint_b >= 0, "H lights a pair");
	check(mj_can_take(&P.g, P.v.hint_a, P.v.hint_b), "a pair that can be taken");
	a = P.v.hint_a; b = P.v.hint_b;
	mj_play_key(&P, 'H', 0x02, 1);
	check(P.v.hint_a != a || P.v.hint_b != b, "H again: the next pair");
	mj_play_key(&P, 0x1b, 0, 1);
	check(P.v.hint_a < 0, "Esc puts the hint out");

	out_clear();
	mj_play_key(&P, 's', 0, 1);
	check(P.all, "S redeals the table");
	check_eq(P.g.shuffles, 1, "a shuffle counted");
	check_eq(P.sound, MJ_SND_SHUFFLE, "with its sound");

	out_clear();
	mj_play_key(&P, 'v', 0, 1);
	check_eq(P.v.corner, 1, "V turns the view");
	check(P.relayout && P.save, "laid out again, remembered");
	mj_play_key(&P, 'V', 0x02, 1);
	check_eq(P.v.corner, 0, "shift V turns it back");

	mj_play_key(&P, 'f', 0, 1);
	check(P.v.show_free, "F dims the blocked tiles");

	out_clear();
	mj_play_key(&P, 'z', 0, 1);
	check(!P.relayout && strstr(P.v.msg, "window") != 0, "no zoom where the face has none");
	P.can_zoom = true;
	mj_play_key(&P, 'z', 0, 1);
	check(P.zoom == 1 && P.relayout, "Z zooms on the screen face");

	mj_play_key(&P, 'd', 0, 1);
	check(P.random, "D: random deals");
	mj_play_key(&P, 'n', 0, 31337);
	check_eq(P.g.seed, 31337, "N deals the number it is given");
	check_eq(P.g.mode, MJ_DEAL_RANDOM, "a random one");
	check(strstr(P.v.status, "#31337?") != 0, "and the bar marks it");
	mj_play_key(&P, 'd', 0, 1);

	/* A: the same deal again */
	{
		uint16_t pa, pb;
		uint8_t first[MJ_MAX];
		mj_play_key(&P, 'n', 0, 555);
		memcpy(first, P.g.face, sizeof(first));
		mj_pairs(&P.g, &pa, &pb, 1);
		click_tile(pa);
		click_tile(pb);
		check_eq(P.g.left, 142, "a pair taken");
		mj_play_key(&P, 'a', 0, 1);
		check(P.g.left == 144 && !memcmp(first, P.g.face, sizeof(first)), "A deals the same tiles again");
	}
}

/* -- layouts ---------------------------------------------------------------------- */

static void test_layouts(void)
{
	setup(5);
	mj_play_key(&P, 'l', 0, 9);
	check(!strcmp(P.L.name, "Pyramid"), "L: the next layout");
	check(P.relayout, "laid out again");
	mj_play_key(&P, 'L', 0x02, 9);
	check(!strcmp(P.L.name, "Turtle"), "shift L: back");

	check(mj_play_layout(&P, mj_play_find_layout(&P, "Little"), 3), "a layout from a file");
	check_eq(P.g.left, 6, "dealt");
	check(!mj_play_layout(&P, mj_play_find_layout(&P, "Broken"), 3), "a broken file is refused");
	check(!strcmp(P.L.name, "Little") && P.g.left == 6, "and the table stays as it was");
	check(strstr(P.v.msg, "Broken") != 0 && strstr(P.v.msg, "missing") != 0,
	      "the bar says which file, and what is wrong");
	check_eq(mj_play_find_layout(&P, "Nowhere"), -1, "an unknown name");
}

/* -- stats.txt ---------------------------------------------------------------------- */

static void test_stats(void)
{
	static char text[2048];
	int n;
	setup(1);
	P.random = true;
	P.v.show_free = true;
	P.v.corner = 3;
	P.zoom = 1;
	{
		mj_stat_t *s = &P.stats[P.nstats++];
		memset(s, 0, sizeof(*s));
		strcpy(s->name, "My Own Layout");
		s->played = 12; s->won = 5; s->best = 333;
	}
	n = mj_play_save_stats(&P, text, sizeof(text));
	check(n > 0 && strstr(text, "layout Turtle\n") != 0, "the layout is saved");
	check(strstr(text, "stat 12 5 333 My Own Layout\n") != 0, "a statistic with a name of several words");

	setup(1);
	mj_play_load_stats(&P, text);
	check(!strcmp(P.want, "Turtle"), "the layout read back");
	check(P.random && P.v.show_free && P.v.corner == 3 && P.zoom == 1, "the settings read back");
	{
		const mj_stat_t *s = mj_play_stat(&P, "My Own Layout");
		check(s && s->played == 12 && s->won == 5 && s->best == 333, "the statistic read back");
	}
	mj_play_load_stats(&P, "junk\r\nstat\nstat 1 2\nlayout\ncorner 9\n\n");
	check(P.v.corner == 1, "nonsense lines do no harm (corner 9 is 1)");
	check(!mj_play_stat(&P, ""), "a statistic with no name is not kept");
}

static void test_wheel(void)
{
	int ox, oy;
	setup(8);
	ox = P.v.scroll_x; oy = P.v.scroll_y;
	mj_play_wheel(&P, 3, false);
	check(P.v.scroll_x == ox && P.v.scroll_y == oy && !P.sdx && !P.sdy,
	      "the wheel does nothing to a board that fits");

	P.can_zoom = true;
	mj_play_button(&P, MJ_B_ZOOM, 1);
	P.v.size = MJ_ART_LARGE;
	mj_view_layout(&P.v, 320, 240);
	out_clear();
	mj_play_wheel(&P, 2, false);
	check(P.sdy == 48 && P.sdx == 0, "the wheel scrolls down, two notches 48 pixels");
	out_clear();
	mj_play_wheel(&P, 2, true);
	check(P.sdx == 48 && P.sdy == 0, "with a modifier, across");
	out_clear();
	mj_play_wheel(&P, -1, true);
	check(P.sdx == -24, "and back");
	out_clear();

	/* a board too wide but not too tall: a plain wheel goes across */
	P.v.size = MJ_ART_LARGE;
	mj_view_layout(&P.v, 320, 400);
	ox = P.v.scroll_x;
	mj_play_wheel(&P, 1, false);
	check(P.v.bh + 4 <= P.v.area_h, "(a layout this tall fits 400 rows)");
	check(P.v.scroll_x == ox + 24 && P.sdx == 24, "a plain wheel scrolls a wide-only board across");
}

static void test_time(void)
{
	char b[16];
	mj_play_time(0, b, sizeof(b));
	check(!strcmp(b, "0:00"), "0:00");
	mj_play_time(75, b, sizeof(b));
	check(!strcmp(b, "1:15"), "1:15");
	mj_play_time(3600 + 62, b, sizeof(b));
	check(!strcmp(b, "1:01:02"), "1:01:02");
}

static void test_messages(void)
{
	setup(3);
	P.now = 2000;
	mj_play_key(&P, 'u', 0, 1);
	check(strstr(P.v.msg, "Nothing to undo") != 0, "undo with nothing to undo says so");
	mj_play_tick(&P, 2001);
	check(P.v.msg[0] != 0, "a message stays a while");
	mj_play_tick(&P, 2010);
	check(P.v.msg[0] == 0, "and then goes");
	check(P.bar, "the bar repainted when it does");
}

int main(void)
{
	test_time();
	test_click_game();
	test_keyboard_game();
	test_keys();
	test_layouts();
	test_stats();
	test_messages();
	test_wheel();
	printf("play_test: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
