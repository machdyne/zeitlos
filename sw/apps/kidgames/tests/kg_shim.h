#ifndef KG_SHIM_H
#define KG_SHIM_H

/*
 * kidgames -- host-side shim.
 *
 * kgui.c draws through the arcade runtime (sw/common/games/zarcade.h),
 * and on a build machine the runtime's machine half is
 * sw/common/games/tests/ar_host.c: a software framebuffer with the
 * blitter's own fills, patterns, dither and blits, a window at a fixed
 * place on it, and full screen. So the REAL kgui.c, kgpad.c, kgfont.c
 * and the games run unmodified here.
 *
 * kg_host_open() stands the app up (kg_init(), in the window: full
 * screen is the same picture at (0,0)), pictures go to `dir`.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../common/games/zarcade.h"
#include "../../../common/games/zarcade_int.h"
#include "../../../common/games/tests/ar_host.h"

#include "../kgui.h"

static inline bool kg_host_open(const char *dir)
{
	ar_host_run(NULL, dir);
	if (!kg_init()) return false;
	ar_set_fullscreen(false);
	ar_frame();             /* the window's first picture */
	return true;
}

/* Playfield pixel (x, y): lit or not. */
static inline int kg_host_pixel(int x, int y)
{
	return ar_host_lit(x, y, 1, 1);
}

/* Lit pixels on the desktop sheet (rows 0..479) outside the playfield:
 * what an app drawing past its edge would leave on other windows. */
static inline int kg_host_outside(void)
{
	static uint8_t fb[AR_HOST_FB_BYTES];
	int x, y, n = 0;
	ar_host_fb_copy(fb);
	for (y = 0; y < 480; y++)
		for (x = 0; x < 640; x++) {
			if (!fb[y * 640 + x]) continue;
			if (x >= ar__s.ox && x < ar__s.ox + AR_W &&
			    y >= ar__s.oy && y < ar__s.oy + AR_H) continue;
			n++;
		}
	return n;
}

#endif
