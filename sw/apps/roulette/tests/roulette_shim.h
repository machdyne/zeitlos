#ifndef ROULETTE_SHIM_H
#define ROULETTE_SHIM_H

/*
 * Zeitlos roulette -- host-side drawing shim.
 *
 * sw/common/tests/zrender.h supplies software versions of the z_fb_*
 * primitives. It supplies the shaded fill only in its _async form, and
 * approximates it as ((x + y) & 3) < level -- which saturates at level
 * 4, so every level zgfx.h defines from 4 to 16 comes out SOLID.
 *
 * That is not a harmless approximation. sw/apps/poker shipped its
 * disabled buttons unreadable because of it: black text on what the
 * render showed as a light stipple and the hardware showed as a solid
 * fill. This board leans on the dither much harder -- it is how a red
 * pocket is told from a black one -- so the render has to be faithful
 * or it is worse than no render at all.
 *
 * Z_SHADE_MAX is 16 and the hardware generates a screen-aligned 4x4
 * dither, so that is what this does. Screen aligned, not rectangle
 * aligned, so two adjacent shaded cells join with no seam.
 *
 * sw/apps/poker/tests/poker_shim.h carries an identical copy. They are
 * not shared because putting it in zrender.h would collide with that
 * one; if zrender.h ever grows a real dither, delete both.
 */

#include "../../../common/tests/zrender.h"

void z_fb_hw_fill_shade(int x, int y, int w, int h, int level)
{
    static const int bayer[4][4] = {
        {  0,  8,  2, 10 },
        { 12,  4, 14,  6 },
        {  3, 11,  1,  9 },
        { 15,  7, 13,  5 }
    };
    int i, j;

    if (level < 0) level = 0;
    if (level > Z_SHADE_MAX) level = Z_SHADE_MAX;

    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++) {
            int px = x + i, py = y + j;
            z_fb_set_pixel(px, py, bayer[py & 3][px & 3] < level ? 1 : 0,
                NULL);
        }
}

bool z_fb_hw_dither_available(void) { return true; }

#endif
