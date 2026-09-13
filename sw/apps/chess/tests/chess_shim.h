#ifndef CHESS_SHIM_H
#define CHESS_SHIM_H

/*
 * Zeitlos chess -- host-side drawing shim.
 *
 * sw/common/tests/zrender.h provides software versions of the z_fb_*
 * primitives that zwin.c and zwidget.c call, so those two can run
 * unmodified on a build machine. board_ui.c calls two things it does
 * not provide, and one thing it provides differently, so they are
 * here.
 *
 * -- the blitter scissor --
 *
 * z_gfx_blit_scissor() programs GPU registers. On a build machine
 * there are none, so it becomes a clip rectangle the software blit
 * below honours. It has to be REAL and not a stub returning true:
 * board_ui.c's blit_tile() relies on it to keep a tile inside the
 * content rectangle, and a stub would let tests pass while the
 * shipped code painted over the window frame.
 *
 * -- z_fb_hw_blit_mem, again --
 *
 * zrender.h has one, and it ORs: it sets the destination pixel where
 * the source bit is 1 and leaves it alone where the source bit is 0.
 * That is right for the thing zrender.h was written for -- dropping
 * an image onto a cleared panel -- and wrong for this app, because a
 * chess tile is an OPAQUE 24x24 square that has to REPLACE what is
 * under it. The white squares of this board are large areas of zero
 * bits, and with an ORing blit a piece that moved away would never be
 * erased.
 *
 * It matters beyond the pictures: "does the board repaint correctly
 * after a move" is exactly the kind of thing tests/test_layout.c is
 * for, and it cannot test it against a blit that behaves differently
 * from the hardware. So this file renames zrender.h's version out of
 * the way and supplies a copying one.
 *
 * If zrender.h ever grows a copying blit of its own, delete this half
 * of the file rather than keeping both.
 */

#define _GNU_SOURCE

#define z_fb_hw_blit_mem zr_blit_mem_or_only
#include "../../../common/tests/zrender.h"
#undef z_fb_hw_blit_mem

static z_clip_t shim_scissor = { 0, 0, Z_SCREEN_W - 1, Z_SCREEN_H - 1 };

/*
 * zgfx.c's version, which is not linked here.
 *
 * The visible region -- the part of a window not covered by the
 * windows in front of it -- is set by wm, and on a build machine
 * there is no wm: zrender.h's z_gfx_visible_count() returns 0, which
 * means UNRESTRICTED (see zgfx.h) and not "invisible". So the
 * interesting case is the only one that can occur, and the other is
 * handled conservatively rather than pretended about: zrender.h keeps
 * its region rectangles private, so if a test ever does install one,
 * this falls back to the caller's own clip. That is a superset of the
 * right answer, never a subset, so a test can still only fail by
 * drawing somewhere it should not -- it cannot pass by drawing
 * somewhere it should not.
 */
bool z_gfx_visible_clip(int i, const z_clip_t *clip, z_clip_t *out) {

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

/* A copying 1bpp blit, honouring the scissor set above. Same source
 * format the hardware reads: one uint32_t per row, least significant
 * bit leftmost. */
bool z_gfx_blit_scissor(int i, const z_clip_t *clip) {
	z_clip_t eff;
	if (!z_gfx_visible_clip(i, clip, &eff)) return false;
	shim_scissor = eff;
	return true;
}

void z_gfx_blit_scissor_reset(void) {
	shim_scissor.x0 = 0;
	shim_scissor.y0 = 0;
	shim_scissor.x1 = Z_SCREEN_W - 1;
	shim_scissor.y1 = Z_SCREEN_H - 1;
}

bool z_fb_hw_blit_mem(const void *src, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h) {

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
void z_fb_hw_sync(void) { }

/* zrender.h's fill_shade is the _async form only. */
void z_fb_hw_fill_shade(int x, int y, int w, int h, int level) {
	z_fb_hw_fill_shade_async(x, y, w, h, level);
}

/* Reads a pixel back, so a test can assert what was DRAWN and not
 * only that drawing did not crash. z_render_get() is static inside
 * zrender.h; this is the same thing under a name the tests can use. */
static int shim_get(int x, int y) {
	return z_render_get(x, y);
}

#endif
