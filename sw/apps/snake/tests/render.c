/*
 * snake -- the real game loop on a build host, driven by a script, with
 * every screen written out as a picture. make -C sw/apps/snake render
 *
 * snake.c is compiled with -Dmain=game_main -DSN_AUTOPLAY and runs
 * unchanged but for the autopilot steering (K stops it, J starts it);
 * the arcade runtime's machine half is sw/common/games/tests/ar_host.c.
 */

#undef main

#include <stdio.h>
#include <string.h>

#include "../../../common/games/tests/ar_host.h"
#include "../../../common/games/zarcade.h"
#include "../snake.h"

int game_main(void);

#define T(f, k)         { (f), ARH_TAP, (k), 0, NULL, 0, 0, 0, 0 }
#define SHOT(f, n)      { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define MODE(f, m)      { (f), (m), 0, 0, NULL, 0, 0, 0, 0 }
#define COVER(f, x, y, w, h) { (f), ARH_COVER, 0, 0, NULL, (x), (y), (w), (h) }

static const ar_host_step_t script[] = {
	/* the title, in each mode, with its demo snake */
	SHOT(200, "01_title_window"),
	MODE(210, ARH_FULL_COLOR),
	SHOT(420, "02_title_color"),
	MODE(430, ARH_FULL_MONO),
	SHOT(470, "03_title_mono"),
	MODE(480, ARH_FULL_COLOR),
	T(500, ARH_K_DOWN), T(510, ARH_K_UP),

	/* classic, in colour, played by the autopilot */
	T(530, '\r'),
	SHOT(560, "04_ready"),
	SHOT(900, "05_play"),
	SHOT(1800, "06_play_long"),
	T(1810, 'p'),
	SHOT(1820, "07_pause"),
	T(1830, 'p'),
	T(1840, 'K'),
	SHOT(1885, "08_crash"),
	SHOT(1905, "09_dissolve"),
	SHOT(2100, "10_game_over"),
	T(2110, '\r'),
	SHOT(2120, "11_name"),
	T(2130, 'z'), T(2140, 'e'), T(2150, 'd'),
	T(2160, '\r'),
	SHOT(2170, "12_scores"),
	T(2180, '\r'),
	T(2185, 'J'),

	/* two players, in the window, partly covered by another window */
	MODE(2190, ARH_WINDOW),
	T(2200, ARH_K_DOWN), T(2205, ARH_K_DOWN), T(2210, ARH_K_DOWN),
	T(2220, '\r'),
	COVER(2230, 180, 120, 200, 160),
	SHOT(2600, "13_versus_covered"),
	COVER(2610, 0, 0, 0, 0),
	SHOT(2620, "14_versus_window"),
	/* wm gives the window the colour: in colour while uncovered, in
	 * black and white again while covered (docs/color.md) */
	{ 2621, ARH_WCOLOR, 1, 0, NULL, 0, 0, 0, 0 },
	SHOT(2624, "14b_versus_window_colour"),
	COVER(2625, 180, 120, 200, 160),
	SHOT(2627, "14c_versus_window_covered"),
	COVER(2628, 0, 0, 0, 0),
	SHOT(2629, "14d_versus_window_colour_again"),
	MODE(2630, ARH_FULL_COLOR),
	SHOT(2800, "15_versus_color"),
	T(2810, 'K'),
	SHOT(2900, "16_versus_crash"),
	SHOT(3100, "17_round"),
	SHOT(3200, "18_round2_ready"),
	T(3210, 0x1b),
	T(3220, 'J'),

	/* the open field, mono full screen */
	MODE(3230, ARH_FULL_MONO),
	T(3240, ARH_K_UP), T(3245, ARH_K_UP),
	T(3250, '\r'),
	SHOT(4200, "19_open_mono"),
	T(4210, 'p'), T(4220, 0x1b),

	/* mazes, until the first level is cleared */
	MODE(4230, ARH_FULL_COLOR),
	T(4240, ARH_K_DOWN),
	T(4260, '\r'),
	SHOT(4700, "20_maze"),
	SHOT(9000, "21_maze_later"),

	/* easy: the skill line, then classic until the snake bumps the
	 * wall and waits */
	T(9010, 0x1b), T(9020, 0x1b),
	T(9030, ARH_K_UP), T(9040, ARH_K_UP), T(9050, ARH_K_UP),
	SHOT(9060, "22_title_skill_normal"),
	T(9070, ARH_K_LEFT),
	SHOT(9080, "23_title_skill_easy"),
	T(9090, ARH_K_DOWN),
	T(9100, '\r'),
	SHOT(9400, "24_easy_play"),
	T(9410, 'K'),
	SHOT(10400, "25_easy_bumped"),
	T(10410, 'J'),
	SHOT(11500, "26_easy_later"),

	{ 11510, ARH_END, 0, 0, NULL, 0, 0, 0, 0 },
};

/* The open field wraps: a picture of a head half out of the top (cut
 * at the field's edge, not hidden), whenever the autopilot goes that
 * way. */
static void tap(uint32_t k) {
	extern void ar__key(uint32_t keysym, bool pressed, uint8_t mods);
	ar__key(k, true, 0);
	ar__key(k, false, 0);
}

void ar_host_hook(uint32_t frame) {
	static int step;
	static uint32_t at;
	const sn_snake_t *s = &sn.g.s[0];
	uint16_t h;
	if (sn.st != ST_PLAY || sn.mode != SN_MODE_OPEN || !s->alive || !s->len) return;
	h = s->body[s->head];
	switch (step) {
	case 0:          /* take over from the autopilot: straight up */
		tap('K');
		tap(ARH_K_UP);
		step++;
		break;
	case 1:
		if (s->dir == SN_UP && sn_cy(h) == 0 && (s->progress >> 8) == 4) {
			ar_host_snapshot("19a_open_out_of_the_top");
			tap(ARH_K_LEFT);
			at = frame;
			step++;
		}
		break;
	case 2:          /* round, and down through the bottom */
		if (frame > at + 20) { tap(ARH_K_DOWN); step++; }
		break;
	case 3:
		if (s->dir == SN_DOWN && sn_cy(h) == SN_GH - 1 &&
		    (s->progress >> 8) == 4) {
			ar_host_snapshot("19b_open_in_at_the_top");
			tap('J');
			step++;
		}
		break;
	default:
		break;
	}
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : ".";
	int r;
	/* the first part of the script is the game as it was: normal */
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
