#ifndef CHESS_SHIM_H
#define CHESS_SHIM_H

/*
 * Zeitlos chess -- host-side drawing shim.
 *
 * sw/common/tests/zrender.h provides software versions of the z_fb_*
 * primitives that zwin.c and zwidget.c call, so those can run
 * unmodified on a build machine. This file adds the handful of things
 * board_ui.c needs that it does not, and replaces one that it does
 * provide differently.
 *
 * Keep this file as small as zrender.h allows. Everything in it is a
 * place where the host and the hardware can disagree, and a test that
 * passes against a shim not behaving like the blitter is worse than
 * no test. It has already shrunk once: it used to carry its own
 * z_gfx_visible_clip() and a pair of scissor helpers, and zrender.h
 * grew the paint session that made both unnecessary.
 *
 * -- z_fb_hw_blit_mem, which zrender.h does have --
 *
 * zrender.h's version ORs: it sets the destination pixel where the
 * source bit is 1 and LEAVES IT ALONE where the source bit is 0. That
 * is right for what it was written for -- dropping an image onto a
 * cleared panel -- and wrong for this app.
 *
 * A chess tile is an OPAQUE 24x24 square that has to REPLACE what is
 * under it, and the light squares of this board are large areas of
 * zero bits. Against an ORing blit a piece that moved away would never
 * be erased, and tests/test_layout.c's "the knight was erased from b1"
 * check could not exist.
 *
 * If zrender.h ever grows a copying blit, delete this half of the file
 * rather than keeping both.
 *
 * -- the capability probes --
 *
 * board_ui.c asks what this bitstream's blitter can do, the way zgfx.h
 * says to. On a build machine there is no bitstream, so the answers
 * are constants. Raster ops and the cookie-cut mode are irrelevant
 * either way: this app deliberately uses neither, which is what
 * gen_pieces.py's header is about.
 */

#define _GNU_SOURCE

#define z_fb_hw_blit_mem zr_blit_mem_or_only
#include "../../../common/tests/zrender.h"
#undef z_fb_hw_blit_mem

/*
 * A copying 1bpp blit, honouring the paint session the way the
 * hardware scissor does.
 *
 * zrender.h's own primitives do not consult the visible region --
 * z_fb_set_pixel() applies only the explicit clip it is handed. That
 * is fine here, because no test installs a region: there is no window
 * manager on a build machine, and a visible count of 0 means
 * unrestricted. Honouring z_gfx_paint_current() anyway costs one
 * comparison and stops this quietly becoming wrong the day a test
 * does install one.
 *
 * Source format is the hardware's: one uint32_t per row, least
 * significant bit LEFTMOST -- the opposite of font and icon data.
 */
bool z_fb_hw_blit_mem(const void *src, int src_stride,
	int src_x, int src_y, int dst_x, int dst_y, int w, int h) {

	const uint32_t *s = (const uint32_t *)src;
	int stride_w = src_stride / 4;      /* BYTES on the way in, as the
	                                     * hardware call takes them */
	z_clip_t pass;
	bool in_pass = z_gfx_paint_current(&pass) != 0;
	int x, y;

	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			int dx = dst_x + x, dy = dst_y + y;
			int sx = src_x + x, sy = src_y + y;
			uint32_t word;
			if (in_pass && (dx < pass.x0 || dx > pass.x1 ||
				dy < pass.y0 || dy > pass.y1)) continue;
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

/* zrender.h has the _async form only. */
void z_fb_hw_fill_shade(int x, int y, int w, int h, int level) {
	z_fb_hw_fill_shade_async(x, y, w, h, level);
}

/* Reads a pixel back, so a test can assert what was DRAWN and not only
 * that drawing did not crash. z_render_get() is static inside
 * zrender.h; this is the same thing under a name the tests can use. */
static int shim_get(int x, int y) {
	return z_render_get(x, y);
}

#endif
