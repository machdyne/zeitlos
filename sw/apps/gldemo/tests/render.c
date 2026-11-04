/*
 * gldemo on a build host, its pictures written out: the window in
 * black and white, full screen in colour (256), wireframe, flat,
 * without depth, unlit, full screen in black and white.
 * make -C sw/apps/gldemo render
 */
#undef main
#include <stdio.h>
#include "../../../common/games/tests/ar_host.h"

int game_main(void);

#define T(f, k)         { (f), ARH_TAP, (k), 0, NULL, 0, 0, 0, 0 }
#define SHOT(f, n)      { (f), ARH_SHOT, 0, 0, (n), 0, 0, 0, 0 }
#define MODE(f, m)      { (f), (m), 0, 0, NULL, 0, 0, 0, 0 }
#define END(f)          { (f), ARH_END, 0, 0, NULL, 0, 0, 0, 0 }

static const ar_host_step_t script[] = {
	SHOT(10, "01_window"),
	MODE(12, ARH_FULL_COLOR),
	SHOT(20, "02_full_colour"),
	T(22, 'w'), SHOT(26, "03_wire"),
	T(28, 'w'), T(29, 's'), SHOT(33, "04_flat"),
	T(35, 's'), T(36, 'd'), SHOT(40, "05_nodepth"),
	T(42, 'd'), T(43, 'l'), SHOT(47, "06_unlit"),
	T(49, 'l'), T(50, 'p'), SHOT(54, "07_64_colours"),
	T(56, 'p'), SHOT(60, "08_16_colours"),
	MODE(62, ARH_FULL_MONO), SHOT(68, "09_full_mono"),
	END(72)
};

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : "out";
	setvbuf(stdout, NULL, _IOLBF, 0);
	ar_host_run(script, dir);
	game_main();
	printf("render_gldemo: %d pictures\n", ar_host_shots());
	return ar_host_shots() == 9 ? 0 : 1;
}
