/*
 * kidgames -- the app itself on a build host, driven by a script, every
 * screen written out as a picture. Part of `make render`.
 *
 * kidgames.c is compiled with -Dmain=game_main and runs unchanged on
 * the arcade runtime's software framebuffer
 * (sw/common/games/tests/ar_host.c), every frame checked against a full
 * repaint (AR_HOST_VERIFY): what paint() -- the screen's repaint
 * callback -- draws must be what the screen drew as it went.
 *
 * Full screen first (as on a board), the menu, a game played a little
 * with keys and clicks, F1, back to the menu, the window, covered and
 * uncovered, the window closed.
 */

#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/games/tests/ar_host.h"

int game_main(void);

/* zrng.c is the board's (rdcycle); the host's numbers are kg_rand_host.c's */
void z_rng_reseed(void) { }

#define T(f, k)         { (f), ARH_TAP, (k), 0, NULL, 0, 0, 0, 0 }
#define SHOT(f, n)      { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define MODE(f, m)      { (f), (m), 0, 0, NULL, 0, 0, 0, 0 }
#define M(f, x, y, b)   { (f), ARH_MOUSE, (b), 0, NULL, (x), (y), 0, 0 }
#define COVER(f, x, y, w, h) { (f), ARH_COVER, 0, 0, NULL, (x), (y), (w), (h) }
#define END(f)          { (f), ARH_END, 0, 0, NULL, 0, 0, 0, 0 }

#define K_F11 0x11001au

static const ar_host_step_t script[] = {
	SHOT(10, "p01_menu_full"),
	T(12, ARH_K_DOWN), T(20, ARH_K_DOWN),
	SHOT(26, "p02_menu_third"),
	T(28, ARH_K_UP), T(36, ARH_K_UP),
	T(40, '\r'),                          /* Spelling Practice */
	SHOT(50, "p03_game"),
	T(52, 'C'), T(60, 'A'),
	SHOT(66, "p04_typed"),
	M(68, 160, 220, 0), M(70, 40, 200, 1), M(72, 40, 200, 0),   /* a key on the pad */
	SHOT(76, "p05_clicked"),
	T(78, ARH_K_F1),
	SHOT(84, "p06_help"),
	T(86, 0x1b),
	SHOT(92, "p07_help_closed"),
	T(94, 0x1b),                          /* back to the menu */
	SHOT(104, "p08_back_to_menu"),
	MODE(106, ARH_WINDOW),
	SHOT(112, "p09_window"),
	COVER(114, 150, 100, 200, 160),
	SHOT(120, "p10_covered"),
	T(122, ARH_K_DOWN),
	SHOT(130, "p11_covered_moved"),
	COVER(132, 0, 0, 0, 0),
	SHOT(138, "p12_uncovered"),
	END(150)
};

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "out";
	extern uint32_t ar_host_verify_bad(void);
	int r;

	setvbuf(stdout, NULL, _IOLBF, 0);
	setenv("AR_HOST_VERIFY", "1", 0);
	ar_host_run(script, dir);
	r = game_main();
	printf("kg_play: %d pictures, %u frames verified, %u differ\n",
		ar_host_shots(), ar_host_verified(), ar_host_verify_bad());
	return r || ar_host_shots() != 12 || ar_host_verify_bad();
}
