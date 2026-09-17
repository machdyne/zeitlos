#ifndef CRAPS_SHIM_H
#define CRAPS_SHIM_H

/*
 * Zeitlos craps -- host-side drawing shim.
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
 * bj_board.c's blit_card() relies on it to keep a card inside the
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
 * tests/board_test.c compares the dealer's hole card against the
 * back tile bit for bit, which catches the mirroring hazard zcardart.h
 * warns about as well as proving the card is face down.
 */

#define _GNU_SOURCE

#define z_fb_hw_blit_mem zr_blit_mem_or_only
#include "../../../common/tests/zrender.h"
#undef z_fb_hw_blit_mem

static z_clip_t shim_scissor = { 0, 0, Z_SCREEN_W - 1, Z_SCREEN_H - 1 };

/* -- what zrender.h supplies now, and what it still does not --
 *
 * zrender.h grew z_gfx_visible_clip() and the z_gfx_paint_* walkers
 * when the compositor became damage-based, so the copy that used to be
 * here has been deleted -- two definitions is a duplicate symbol at
 * link time, not a fallback.
 *
 * It did NOT grow z_gfx_blit_scissor(). Its z_fb_hw_blit_mem() ignores
 * any scissor and ORs rather than copies, so both of those are still
 * needed below and both still matter: an ORing blit would draw a card
 * face as a solid white rectangle and lose the rank, the pips and the
 * border at once.
 */

bool z_gfx_blit_scissor(int i, const z_clip_t *clip)
{
    z_clip_t eff;

    /* NO REGION MEANS UNRESTRICTED, and this has to say so itself
     * rather than ask zrender.h.
     *
     * zgfx.c's z_gfx_visible_clip() treats gfx_region_n == 0 as the
     * whole screen -- that is how an app draws before wm has sent a
     * region, and how it draws in game mode, where z_gfx_clear_visible()
     * is called deliberately. zrender.h's copy returns FALSE for that
     * case instead, which does not match the hardware and contradicts
     * its own paint walker two functions below ("a single unrestricted
     * pass when there is no region at all").
     *
     * The consequence is not subtle: every blit in a host test is
     * skipped, so cards simply do not appear, while the same code draws
     * correctly on the device. That is a harness that lies in the
     * expensive direction. Worked around here rather than by editing
     * zrender.h, which is shared. */
    if (z_gfx_visible_count() == 0) {
        eff.x0 = 0;
        eff.y0 = 0;
        eff.x1 = Z_SCREEN_W - 1;
        eff.y1 = Z_SCREEN_H - 1;
        if (clip) {
            if (clip->x0 > eff.x0) eff.x0 = clip->x0;
            if (clip->y0 > eff.y0) eff.y0 = clip->y0;
            if (clip->x1 < eff.x1) eff.x1 = clip->x1;
            if (clip->y1 < eff.y1) eff.y1 = clip->y1;
        }
        if (eff.x1 < eff.x0 || eff.y1 < eff.y0) return false;
        shim_scissor = eff;
        return true;
    }

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
 * zrender.h approximates a shade with ((x + y) & 3) < level, which
 * saturates at level 4 -- so every level zgfx.h defines from 4 to 16
 * comes out SOLID. sw/apps/poker shipped its disabled buttons
 * unreadable because of that, and this app's buttons lean on the same
 * treatment.
 *
 * Screen aligned, not rectangle aligned, so two adjacent shaded
 * controls join with no seam.
 *
 * sw/apps/poker and sw/apps/roulette carry identical copies. They are
 * not shared because putting this in zrender.h would collide with
 * theirs; if zrender.h ever grows a real dither, delete all three.
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
            z_fb_set_pixel(px, py, bayer[py & 3][px & 3] < level ? 1 : 0,
                NULL);
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
