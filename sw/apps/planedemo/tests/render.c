/*
 * planedemo -- the film on a build host: every other frame written as a
 * picture, for a video with the music (make -C sw/apps/planedemo/tests
 * video). planedemo.c runs unchanged (-Dmain=game_main) on the arcade
 * runtime's host machine (sw/common/games/tests/ar_host.c), eight
 * planes unless AR_HOST_PLANES says fewer.
 */

#undef main

#include <stdio.h>
#include <stdlib.h>

#include "../../../common/games/tests/ar_host.h"
#include "../pd_music.h"

int game_main(void);

static int every = 2;

void ar_host_hook(uint32_t frame) {
	char name[32];
	if (frame % (uint32_t)every) return;
	snprintf(name, sizeof(name), "f%05u", frame / (uint32_t)every);
	ar_host_snapshot(name);
}

int main(int argc, char **argv) {
	static ar_host_step_t script[2];
	const char *dir = argc > 1 ? argv[1] : "out/frames";
	if (argc > 2) every = atoi(argv[2]);
	if (every < 1) every = 1;
	/* a third argument: stop at that frame (a quick look at one scene) */
	script[0].frame = argc > 3 ? (uint32_t)atoi(argv[3]) : PD_END_FRAME + 30;
	script[0].kind = ARH_END;
	ar_host_run(script, dir);
	return game_main();
}
