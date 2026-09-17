#ifndef KG_SHIM_H
#define KG_SHIM_H

/*
 * kidgames -- host-side shim.
 *
 * sw/common/tests/zrender.h supplies software versions of most of the
 * z_fb_* entry points, so the REAL zwin.c, kgui.c, kgpad.c and
 * kgfont.c run unmodified on a build machine. It does not supply
 * z_fb_hw_fill_shade(), which this app leans on heavily, so that is
 * here.
 *
 * -- WHY THIS IMPLEMENTS A REAL DITHER --
 *
 * zrender.h's own z_fb_hw_fill_shade_async() is a light stipple, which
 * is the right call for its purpose: make a shaded area visibly
 * different from a solid one. For this app that is not enough. Shade
 * is the substitute for eight ncurses colour pairs (kg.h), and the
 * question a render has to answer is "can a six-year-old tell the
 * selected row from the unselected ones", which a stipple that
 * saturates to solid at level 4 cannot answer -- every shade this app
 * uses is above 3.
 *
 * So this is the hardware's actual scheme: a 4x4 ordered dither,
 * seventeen levels, indexed by ABSOLUTE framebuffer row and column.
 * Screen-aligned, not rectangle-aligned, which is what lets two
 * adjacent shaded fills join with no seam -- and getting that wrong
 * here would make the render look better than the board, which is the
 * one direction a render harness must never be wrong in.
 */

#include "../../../common/tests/zrender.h"

/*
 * The blitter's hardware scissor. zrender.h models the visible region
 * in software (zr_allows) and has no scissor to reset, so this is a
 * no-op here -- but kgui.c must still CALL it on the game-mode path,
 * because on real hardware the scissor is persistent state and leaving
 * it set silently clips whatever blit comes next, in this process or
 * the next one scheduled (zgfx.c, hw_fill_rect_core).
 */
void z_gfx_blit_scissor_reset(void) { }

/*
 * The memory-source blit's capability probe. zrender.h implements the
 * blit itself but not the question "does this bitstream have it",
 * which only means anything on real hardware.
 *
 * true here, so the render harness draws the art path rather than the
 * "no picture on this bitstream" fallback. The fallback is reachable
 * in a render by flipping this to false, which is worth doing whenever
 * that message's layout changes -- it is otherwise invisible on every
 * machine anyone is likely to test on, and so is exactly the kind of
 * branch that rots.
 */
bool z_fb_hw_blit_mem_available(void) { return true; }

/* Bayer 4x4, the classic recursive construction. A cell is lit when
 * its threshold is below the level, so level 0 is black and level 16
 * is solid -- matching Z_SHADE_MAX. */
static const uint8_t KG_BAYER[4][4] = {
	{  0,  8,  2, 10 },
	{ 12,  4, 14,  6 },
	{  3, 11,  1,  9 },
	{ 15,  7, 13,  5 },
};

void z_fb_hw_fill_shade(int x, int y, int w, int h, int level)
{
	int i, j;

	if (level <= 0) { z_fb_hw_fill_rect(x, y, w, h, 0); return; }
	if (level >= 16) { z_fb_hw_fill_rect(x, y, w, h, 1); return; }

	for (j = 0; j < h; j++)
		for (i = 0; i < w; i++) {
			int px = x + i, py = y + j;
			/* & 3 on the ABSOLUTE coordinate, never on i/j. */
			int t = KG_BAYER[py & 3][px & 3];
			z_fb_set_pixel(px, py, t < level ? 1 : 0, NULL);
		}
}

#endif
