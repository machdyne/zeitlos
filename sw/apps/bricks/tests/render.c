/*
 * bricks -- the real game loop on a build host, driven by a script, with
 * every screen written out as a picture. make -C sw/apps/bricks render
 *
 * bricks.c is compiled with -Dmain=game_main -DBR_AUTOPLAY and runs
 * unchanged but for the autopilot playing (K stops it, J starts it);
 * the arcade runtime's machine half is sw/common/games/tests/ar_host.c,
 * here with AR_HOST_VERIFY: every frame is checked against a repaint
 * from nothing.
 */

#undef main

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../../../common/games/tests/ar_host.h"
#include "../../../common/games/zarcade.h"
#include "../bricks.h"

int game_main(void);

#define T(f, k)         { (f), ARH_TAP, (k), 0, NULL, 0, 0, 0, 0 }
#define SHOT(f, n)      { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define MODE(f, m)      { (f), (m), 0, 0, NULL, 0, 0, 0, 0 }
#define COVER(f, x, y, w, h) { (f), ARH_COVER, 0, 0, NULL, (x), (y), (w), (h) }
#define MOUSE(f, x, y, b) { (f), ARH_MOUSE, (b), 0, NULL, (x), (y), 0, 0 }

static const ar_host_step_t script[] = {
	SHOT(100, "01_title_window"),
	MODE(110, ARH_FULL_COLOR),
	SHOT(200, "02_title_color"),
	MODE(210, ARH_FULL_MONO),
	SHOT(240, "03_title_mono"),
	MODE(250, ARH_FULL_COLOR),
	T(260, ARH_K_RIGHT),                      /* start at round 2 ... */
	T(262, ARH_K_LEFT),                       /* ... no, round 1 */
	SHOT(270, "04_title_round"),

	/* a click starts it */
	MOUSE(300, 160, 150, 1),
	MOUSE(301, 160, 150, 0),
	SHOT(318, "05_bricks_arriving"),
	SHOT(400, "06_ready"),
	/* the mouse moves the paddle, the ball on it */
	MOUSE(402, 60, 200, 0),
	SHOT(403, "07_mouse_left"),
	MOUSE(404, 250, 200, 0),
	MOUSE(405, 250, 200, 1),                  /* to play */
	MOUSE(406, 250, 200, 0),
	SHOT(700, "08_play"),
	SHOT(1600, "09_play_later"),
	T(1610, 'p'),
	SHOT(1620, "10_pause"),
	T(1630, 'p'),

	/* in the window, then covered by another window */
	MODE(1700, ARH_WINDOW),
	SHOT(1900, "11_window"),
	COVER(1910, 150, 60, 120, 100),
	SHOT(2300, "12_window_covered"),
	COVER(2310, 0, 0, 0, 0),
	MODE(2320, ARH_FULL_MONO),
	SHOT(2700, "13_mono"),
	MODE(2710, ARH_FULL_COLOR),

	/* to the end: stop the autopilot (the hook keeps the paddle out of
	 * the way); the rest is driven by the hook */
	T(12000, 'K'),

	{ 24000, ARH_END, 0, 0, NULL, 0, 0, 0, 0 },
};

static void tap(uint32_t k) {
	extern void ar__key(uint32_t keysym, bool pressed, uint8_t mods);
	ar__key(k, true, 0);
	ar__key(k, false, 0);
}

static int caps_falling(void) {
	int i, n = 0;
	for (i = 0; i < BR_MAX_CAPS; i++) if (br.g.cap[i].type) n++;
	return n;
}

static int shots_live(void) {
	int i, n = 0;
	for (i = 0; i < BR_MAX_SHOTS; i++) if (br.g.shot[i].live) n++;
	return n;
}

/* Pictures of the moments that do not come at a known frame. */
void ar_host_hook(uint32_t frame) {
	static bool capsule, multi, laser, wide, catching, lost, clear, next, ended;
	static int games;

	if (!capsule && br.st == ST_PLAY && caps_falling()) {
		ar_host_snapshot("20_capsule");
		capsule = true;
	}
	if (!multi && br.st == ST_PLAY && br_balls(&br.g) >= 3) {
		ar_host_snapshot("21_three_balls");
		multi = true;
	}
	if (!laser && br.st == ST_PLAY && shots_live() >= 2) {
		ar_host_snapshot("22_laser");
		laser = true;
	}
	if (!wide && br.st == ST_PLAY && br.g.power == BR_P_WIDE) {
		ar_host_snapshot("23_wide");
		wide = true;
	}
	if (!catching && br.st == ST_PLAY && br.g.power == BR_P_CATCH &&
	    br.g.ball[0].stuck && br.g.ball[0].stuck_left < 140) {
		ar_host_snapshot("24_caught");
		catching = true;
	}
	if (!lost && br.st == ST_LOST && br.st_frames == 8) {
		ar_host_snapshot("25_paddle_lost");
		lost = true;
	}
	if (!clear && br.st == ST_CLEAR && br.st_frames == 20) {
		ar_host_snapshot("26_round_clear");
		clear = true;
	}
	if (clear && !next && br.st == ST_READY && br.st_frames == 24) {
		ar_host_snapshot("27_next_round_arriving");
		next = true;
	}

	/* Capsules the autopilot may not happen to catch, dropped on the
	 * paddle so their pictures are drawn and checked too */
	if ((frame == 2800 || frame == 3400) && br.st == ST_PLAY) {
		br.g.cap[0].type = frame == 2800 ? BR_CAP_LASER : BR_CAP_CATCH;
		br.g.cap[0].x = (int16_t)br.g.px;
		br.g.cap[0].y = (int32_t)(BR_PADDLE_Y - 30) << 8;
	}

	/* The autopilot takes a long time over the last few bricks of a
	 * round: to see the end of one, thin the wall out to its lowest
	 * row (gold stays). */
	if (frame == 3000 && br.st == ST_PLAY) {
		int r, c, low = -1;
		for (r = 0; r < BR_ROWS; r++)
			for (c = 0; c < BR_COLS; c++)
				if (br.g.brick[r][c] && br.g.brick[r][c] != BR_GOLD) low = r;
		br.g.bricks_left = 0;
		for (r = 0; r < BR_ROWS; r++)
			for (c = 0; c < BR_COLS; c++) {
				if (!br.g.brick[r][c] || br.g.brick[r][c] == BR_GOLD) continue;
				if (r != low) br.g.brick[r][c] = BR_EMPTY;
				else br.g.bricks_left++;
			}
		br_draw_field_dirty();
	}

	/* with the autopilot off, keep the paddle away from the ball */
	if (frame >= 12000 && br.st == ST_PLAY) {
		int bx = br.g.ball[0].x >> 8;
		br_paddle_centre(&br.g, bx < 160 ? 290 : 30);
	}

	/* after the game: Enter, initials, the table, back to the title */
	if (br.st == ST_OVER && br.st_frames == 50) {
		ar_host_snapshot("30_game_over");
		tap('\r');
	}
	if (br.st == ST_NAME && br.st_frames == 5) {
		ar_host_snapshot("31_name");
		tap('z'); tap('o'); tap('e'); tap('\r');
	}
	if (br.st == ST_SCORES && br.st_frames == 20 && !games) {
		ar_host_snapshot("32_scores");
		tap('\r');
		games++;
	}
	if (games && !ended && br.st == ST_TITLE && br.st_frames == 30) {
		ar_host_snapshot("33_title_again");
		ended = true;
	}
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : ".";
	int r;
	ar_host_run(script, dir);
	r = game_main();
	printf("render: %d pictures, %u frames, %llu blitter operations, "
		"%u notes\n", ar_host_shots(), ar_host_frame(),
		(unsigned long long)arh_ops, zsfxh_notes);
	printf("render: scores file:\n%s", ar_host_scores());
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
