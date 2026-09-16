#ifndef POKER_SHIM_H
#define POKER_SHIM_H

/*
 * Zeitlos poker -- host-side drawing shim.
 *
 * sw/common/tests/zrender.h provides software versions of the z_fb_*
 * primitives so the window layer runs unmodified on a build machine.
 * table_ui.c needs three things it does not provide, for the same
 * reasons sw/apps/chess/tests/chess_shim.h needed them -- see that
 * file, which this one follows deliberately rather than inventing a
 * second arrangement.
 *
 * -- the blitter scissor --
 *
 * z_gfx_blit_scissor() programs GPU registers. On a build machine
 * there are none, so it becomes a clip rectangle the software blit
 * below honours. It has to be REAL and not a stub returning true:
 * table_ui.c's blit_card() relies on it to keep a card inside the
 * content rectangle, and a stub would let the tests pass while the
 * shipped code painted over the window frame.
 *
 * -- a COPYING blit --
 *
 * zrender.h's z_fb_hw_blit_mem() ORs: it sets the destination where
 * the source bit is 1 and leaves it alone where the source bit is 0.
 * That is right for dropping an image onto a cleared panel and wrong
 * here, because a card is an OPAQUE tile that must REPLACE what is
 * under it.
 *
 * WHAT THIS DOES AND DOES NOT CURRENTLY PROVE. pt_draw_all() clears
 * the content area before it draws anything, so on that path OR and
 * COPY produce identical output and an ORing shim passes every test
 * in this directory. That was established by mutating this file and
 * watching nothing fail. The copying version is kept because it is
 * what the hardware does, and because the moment anything draws a
 * card over existing pixels -- a partial redraw, a card moving -- the
 * difference becomes the whole ballgame: a card face is mostly 1s
 * with the ink as 0s, so an ORing blit over an existing card would
 * leave a solid white rectangle with the rank and pips gone.
 *
 * tests/test_layout.c compares a drawn card against its source tile
 * bit for bit instead, which is a stronger statement than opacity and
 * catches the mirroring hazard cards.h warns about.
 */

#define _GNU_SOURCE

#define z_fb_hw_blit_mem zr_blit_mem_or_only
#include "../../../common/tests/zrender.h"
#undef z_fb_hw_blit_mem

static z_clip_t shim_scissor = { 0, 0, Z_SCREEN_W - 1, Z_SCREEN_H - 1 };

/*
 * zgfx.c's version, which is not linked here.
 *
 * zrender.h's z_gfx_visible_count() returns 0 with no wm running,
 * which means UNRESTRICTED (see zgfx.h) and not "invisible". zrender.h
 * keeps its region rectangles private, so if a test ever does install
 * one this falls back to the caller's own clip: a superset of the
 * right answer, never a subset. A test can therefore still fail by
 * drawing somewhere it should not, and cannot pass by doing so.
 */
bool z_gfx_visible_clip(int i, const z_clip_t *clip, z_clip_t *out)
{
    z_clip_t r;
    int n = z_gfx_visible_count();

    if (n == 0) {
        if (i != 0) return false;
    } else if (i < 0 || i >= n) {
        return false;
    }

    r.x0 = 0; r.y0 = 0;
    r.x1 = Z_SCREEN_W - 1; r.y1 = Z_SCREEN_H - 1;

    if (clip) {
        if (clip->x0 > r.x0) r.x0 = clip->x0;
        if (clip->y0 > r.y0) r.y0 = clip->y0;
        if (clip->x1 < r.x1) r.x1 = clip->x1;
        if (clip->y1 < r.y1) r.y1 = clip->y1;
    }

    if (r.x1 < r.x0 || r.y1 < r.y0) return false;

    *out = r;
    return true;
}

bool z_gfx_blit_scissor(int i, const z_clip_t *clip)
{
    z_clip_t eff;
    if (!z_gfx_visible_clip(i, clip, &eff)) return false;
    shim_scissor = eff;
    return true;
}

void z_gfx_blit_scissor_reset(void)
{
    shim_scissor.x0 = 0;
    shim_scissor.y0 = 0;
    shim_scissor.x1 = Z_SCREEN_W - 1;
    shim_scissor.y1 = Z_SCREEN_H - 1;
}

/* A copying 1bpp blit honouring the scissor. Same source format the
 * hardware reads: one uint32_t per row, least significant bit
 * leftmost. */
bool z_fb_hw_blit_mem(const void *src, int src_stride,
    int src_x, int src_y, int dst_x, int dst_y, int w, int h)
{
    const uint32_t *s = (const uint32_t *)src;
    int stride_w = src_stride / 4;
    int x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int dx = dst_x + x, dy = dst_y + y;
            int sx = src_x + x, sy = src_y + y;
            uint32_t word;
            if (dx < shim_scissor.x0 || dx > shim_scissor.x1) continue;
            if (dy < shim_scissor.y0 || dy > shim_scissor.y1) continue;
            word = s[sy * stride_w + (sx >> 5)];
            z_fb_set_pixel(dx, dy, (int)((word >> (sx & 31)) & 1), NULL);
        }
    }

    return true;
}

bool z_fb_hw_blit_mem_available(void) { return true; }
bool z_fb_hw_rop_available(void) { return true; }
bool z_fb_hw_cookie_available(void) { return false; }
bool z_fb_hw_dither_available(void) { return true; }
void z_fb_hw_sync(void) { }

/* A REAL 4x4 ordered dither, not zrender.h's stipple.
 *
 * zrender.h approximates a shade with ((x+y) & 3) < level, which is
 * fine for its purpose and saturates at level 4 -- so every level
 * zgfx.h defines from 4 to 16 comes out SOLID. The first render of
 * this app's disabled buttons used level 6 and produced solid white
 * boxes with the white labels invisible inside them, which looks
 * exactly like a renderer that forgot to draw the text.
 *
 * zgfx.h's Z_SHADE_MAX is 16 and the hardware generates a screen
 * aligned 4x4 dither, so that is what this does. Screen aligned, not
 * rectangle aligned: indexed by absolute row and column, so two
 * adjacent shaded fills join with no seam.
 */
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
            z_fb_set_pixel(px, py,
                bayer[py & 3][px & 3] < level ? 1 : 0, NULL);
        }
}

/* Reads a pixel back, so a test can assert what was DRAWN and not only
 * that drawing did not crash. z_render_get() is static inside
 * zrender.h; this is the same thing under a name the tests can use. */
static int shim_get(int x, int y)
{
    return z_render_get(x, y);
}

#endif
