/*
 * blocks -- the real game loop on a build host, driven by a script, with
 * every screen written out as a picture. make -C sw/apps/blocks render
 *
 * blocks.c is compiled with -Dmain=game_main -DBK_AUTOPLAY and runs
 * unchanged but for the autopilot playing (K stops it, J starts it);
 * the arcade runtime's machine half is sw/common/games/tests/ar_host.c,
 * here with AR_HOST_VERIFY: every frame is checked against a repaint
 * from nothing, including every frame of the stack falling by copies.
 */

#undef main

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../../common/games/tests/ar_host.h"
#include "../../../common/games/zarcade.h"
#include "../blocks.h"

int game_main(void);

#define T(f, k)         { (f), ARH_TAP, (k), 0, NULL, 0, 0, 0, 0 }
#define SHOT(f, n)      { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define MODE(f, m)      { (f), (m), 0, 0, NULL, 0, 0, 0, 0 }
#define COVER(f, x, y, w, h) { (f), ARH_COVER, 0, 0, NULL, (x), (y), (w), (h) }

static const ar_host_step_t script[] = {
	SHOT(100, "01_title_window"),
	MODE(110, ARH_FULL_COLOR),
	SHOT(200, "02_title_color"),
	MODE(210, ARH_FULL_MONO),
	SHOT(240, "03_title_mono"),
	MODE(250, ARH_FULL_COLOR),
	T(260, ARH_K_RIGHT), T(270, ARH_K_RIGHT),       /* start at level 3 */
	SHOT(280, "04_title_level"),

	/* endless, in colour, the autopilot playing */
	T(300, '\r'),
	SHOT(340, "05_ready"),
	SHOT(700, "06_play"),
	SHOT(2400, "07_play_later"),
	T(2410, 'p'),
	SHOT(2420, "08_pause"),
	T(2430, 'p'),

	/* in the window, then covered by another window: the fall must
	 * fall back to repainting, and still match */
	MODE(2500, ARH_WINDOW),
	SHOT(2700, "09_window"),
	COVER(2710, 150, 60, 120, 100),
	SHOT(3200, "10_window_covered"),
	COVER(3210, 0, 0, 0, 0),
	MODE(3220, ARH_FULL_MONO),
	SHOT(3500, "11_mono"),
	MODE(3510, ARH_FULL_COLOR),

	/* to the end: stop the autopilot; the rest is driven by the hook,
	 * since when the stack tops out is not known in advance */
	T(4000, 'K'),

	{ 20000, ARH_END, 0, 0, NULL, 0, 0, 0, 0 },
};

static void tap(uint32_t k) {
	extern void ar__key(uint32_t keysym, bool pressed, uint8_t mods);
	ar__key(k, true, 0);
	ar__key(k, false, 0);
}

/* Pictures of the moments that do not come at a known frame. */
void ar_host_hook(uint32_t frame) {
	static bool flash, fall, four, over40, curtain, mid40;
	static int games;
	(void)frame;
	/* after a game: Enter, initials, back to the title, then forty
	 * lines with the autopilot on */
	if (bk.st == ST_OVER && bk.st_frames == 50) {
		ar_host_snapshot(games ? "31_forty_over" : "30_game_over");
		tap('\r');
	}
	if (bk.st == ST_NAME && bk.st_frames == 5) {
		ar_host_snapshot(games ? "32_name_forty" : "13_name");
		tap('z'); tap('o'); tap('e'); tap('\r');
	}
	if (bk.st == ST_SCORES && bk.st_frames == 20) {
		ar_host_snapshot(games ? "33_scores_forty" : "14_scores");
		tap('\r');
		games++;
	}
	if (bk.st == ST_TITLE && games == 1 && bk.st_frames == 30) {
		tap('J'); tap(ARH_K_DOWN); tap(ARH_K_DOWN); tap('\r');
		games++;
	}
	/* then easy, little pieces: the skill line, the mode, and a stack
	 * dropped straight down until it is rescued */
	if (bk.st == ST_TITLE && games == 3) {
		switch (bk.st_frames) {
		case 30: tap(ARH_K_UP); tap(ARH_K_UP); tap(ARH_K_UP); break;
		case 34: ar_host_snapshot("40_title_skill"); tap(ARH_K_LEFT); break;
		case 38: ar_host_snapshot("41_title_easy"); tap(ARH_K_DOWN); tap(ARH_K_DOWN); break;
		case 42: ar_host_snapshot("42_title_little"); tap('\r'); games++; break;
		default: break;
		}
	}
	{
		static uint32_t little_at;
		static bool saved, playing;
		if (games == 4 && bk.st == ST_PLAY && bk.mode == BK_MODE_LITTLE) {
			if (!little_at) little_at = frame;
			if (frame == little_at + 900) ar_host_snapshot("43_little_play");
			if (frame == little_at + 1000) tap('K');
			if (frame > little_at + 1000 && !saved && (frame % 8) == 0) tap(' ');
			if (!saved && bk.g.rescues < BK_RESCUES) {
				saved = true;
				ar_host_snapshot("44_little_saved");
				tap('J');
				playing = true;
			}
			if (playing && frame == little_at + 4000) ar_host_snapshot("45_little_later");
		}
	}
	if (bk.st == ST_PLAY && bk.mode == BK_MODE_40 && bk.g.lines >= 20 &&
	    !mid40) {
		ar_host_snapshot("15_forty");
		mid40 = true;
	}
	if (!flash && bk.st == ST_FLASH && bk.st_frames == 2) {
		ar_host_snapshot("20_clear_flash");
		flash = true;
	}
	if (!fall && bk.st == ST_FALL && bk.st_frames == 6) {
		ar_host_snapshot("21_clear_falling");
		fall = true;
	}
	if (!four && bk.st == ST_FLASH && bk.st_frames == 4 &&
	    __builtin_popcount(bk.rows) >= 3) {
		ar_host_snapshot("22_big_clear");
		four = true;
	}
	if (!curtain && bk.st == ST_CURTAIN && bk.st_frames == 20) {
		ar_host_snapshot("23_curtain");
		curtain = true;
	}
	if (!over40 && bk.st == ST_OVER && bk.mode == BK_MODE_40 &&
	    bk.st_frames == 10) {
		ar_host_snapshot("24_forty_done");
		over40 = true;
	}
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : ".";
	int r;
	ar_host_set_settings("skill normal\n");
	ar_host_run(script, dir);
	r = game_main();
	printf("render: %d pictures, %u frames, %llu blitter operations, "
		"%u notes\n", ar_host_shots(), ar_host_frame(),
		(unsigned long long)arh_ops, zsfxh_notes);
	printf("render: scores file:\n%s", ar_host_scores());
	printf("render: settings file:\n%s", ar_host_settings());
	printf("render: last spoken: %s (%d lines)\n", ar_host_said, ar_host_says);
	printf("render: %u frames checked against a full repaint, %u differed\n",
		ar_host_verified(), ar_host_verify_bad());
	{
		extern uint64_t ar__gap_max, ar__gap_sum, ar__gap_n;
		printf("render: a moving overlay is off the screen for %.1f blitter "
			"operations on average, %llu at most\n",
			ar__gap_n ? (double)ar__gap_sum / (double)ar__gap_n : 0.0,
			(unsigned long long)ar__gap_max);
	}
	return r || ar_host_verify_bad() != 0;
}
