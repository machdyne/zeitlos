/*
 * space3d -- the game itself on a build host, driven by a script, every
 * screen written out as a picture. make -C sw/apps/space3d render
 *
 * space3d.c is compiled with -DAR_HOST_TESTS -Dmain=game_main and runs
 * on the arcade runtime's software framebuffer
 * (sw/common/games/tests/ar_host.c). Its lines are drawn as the game
 * runs, not by paint(), so frames are not checked against a full
 * repaint here (that is test-arcade's, line by line).
 *
 * The window in black and white, steering and firing; covered and
 * uncovered; its colour window (the nebula, sixteen colours), F10 off
 * and on; full screen in colour and in mono; the help (F1).
 */

#undef main

#include <stdio.h>
#include <string.h>

#include "../../../common/games/tests/ar_host.h"

int game_main(void);

#define T(f, k)         { (f), ARH_TAP, (k), 0, NULL, 0, 0, 0, 0 }
#define DOWN(f, k)      { (f), ARH_KEY_DOWN, (k), 0, NULL, 0, 0, 0, 0 }
#define UP(f, k)        { (f), ARH_KEY_UP, (k), 0, NULL, 0, 0, 0, 0 }
#define SHOT(f, n)      { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define MODE(f, m)      { (f), (m), 0, 0, NULL, 0, 0, 0, 0 }
#define WCOLOR(f, on)   { (f), ARH_WCOLOR, (on), 0, NULL, 0, 0, 0, 0 }
#define COVER(f, x, y, w, h) { (f), ARH_COVER, 0, 0, NULL, (x), (y), (w), (h) }
#define END(f)          { (f), ARH_END, 0, 0, NULL, 0, 0, 0, 0 }

static const ar_host_step_t script[] = {
	SHOT(20, "01_window"),
	DOWN(22, ARH_K_LEFT), UP(40, ARH_K_LEFT),
	T(42, ' '), T(54, ' '),
	SHOT(60, "02_steered_and_fired"),
	COVER(62, 150, 100, 200, 160),
	SHOT(70, "03_covered"),
	COVER(72, 0, 0, 0, 0),
	SHOT(76, "04_uncovered"),
	WCOLOR(78, 1),
	SHOT(84, "05_colour_window"),
	T(86, ' '),
	SHOT(96, "06_colour_window_fire"),
	T(98, ARH_K_F10),
	SHOT(104, "07_f10_off"),
	T(106, ARH_K_F10),
	SHOT(112, "08_f10_on"),
	MODE(114, ARH_FULL_COLOR),
	SHOT(124, "09_full_colour"),
	MODE(126, ARH_FULL_MONO),
	SHOT(134, "10_full_mono"),
	MODE(136, ARH_WINDOW),
	SHOT(144, "11_window_again"),
	T(146, ARH_K_F1),
	SHOT(150, "12_help"),
	T(152, 0x1b),
	SHOT(160, "13_after_help"),
	WCOLOR(162, 0),
	SHOT(170, "14_focus_elsewhere"),
	END(180)
};

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : "out";
	setvbuf(stdout, NULL, _IOLBF, 0);
	ar_host_run(script, dir);
	game_main();
	printf("render_space3d: %d pictures\n", ar_host_shots());
	return ar_host_shots() == 14 ? 0 : 1;
}
