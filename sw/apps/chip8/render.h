#ifndef CHIP8_RENDER_H
#define CHIP8_RENDER_H

/*
 * chip8 -- guest display to a 1bpp bitmap the blitter can copy.
 *
 * Like core.c, this file includes nothing from sw/common. It produces
 * a bitmap in memory; chip8.c is what hands that bitmap to
 * z_fb_hw_blit_mem(). Keeping the two apart is what makes the dither
 * and the scaling testable on a host, which matters more here than it
 * looks: a dither that is half a pixel out of phase is invisible in a
 * unit test that only checks "some pixels are set", and obvious in one
 * that checks the exact bytes.
 *
 * -- scale --
 *
 * `scale` is FRAMEBUFFER PIXELS PER HIRES PIXEL. A lores pixel is
 * drawn at twice that.
 *
 *     scale   content     lores px   hires px
 *       1     128x64        2x2        1x1
 *       2     256x128       4x4        2x2
 *       4     512x256       8x8        4x4
 *
 * Defined against the HIRES pixel and not the lores one because
 * SUPER-CHIP and XO-CHIP ROMs switch resolution at run time. With this
 * definition the output is 128*scale by 64*scale in BOTH modes -- the
 * window never resizes itself mid-game, and the picture stays the same
 * physical size when a ROM flips to hires. It also means the guest
 * display always exactly fills the output, in either mode, so there
 * are never leftover rows or columns to clear.
 *
 * -- bit order --
 *
 * The output matches the FRAMEBUFFER's convention, not the core's:
 * pixel x is bit (x & 31) of word (x >> 5), least significant bit
 * leftmost (see zgfx.h). The core stores planes MSB-first because
 * sprite data is MSB-first. Nothing has to reconcile those by hand --
 * the expansion tables below are built in the output's order, so the
 * reversal is a property of a table rather than a loop.
 *
 * -- greys --
 *
 * Four XO-CHIP colours on a display with two. Each output pixel is
 * thresholded against a 4x4 ordered dither indexed by its
 * CONTENT-RELATIVE position, so the picture looks identical wherever
 * the window is and its greys do not crawl when it moves. That is the
 * opposite of z_fb_hw_fill_shade()'s screen-relative choice, and
 * deliberately: a shaded fill is a background wash that should tile
 * with its neighbours, where this is one coherent object.
 */

#include <stdint.h>
#include <stdbool.h>

#include "core.h"

#define C8_SCALE_MIN  1
#define C8_SCALE_MAX  4

/* How the four XO-CHIP colour indices map onto grey levels.
 *
 * -- why this is not simply 0, 5, 11, 16 --
 *
 * A colour index is a PALETTE SLOT, not a brightness. Index 1 is
 * Octo's `fillColor`, index 2 is `fillColor2` (plane 2 alone) and
 * index 3 is `blendColor` (where the planes overlap). Nothing about
 * that ordering says index 3 is brighter than index 1, and in Octo's
 * own default theme it is not -- the blend colour is the darkest of
 * the four.
 *
 * Mapping index order straight onto brightness, which is what this
 * originally did, puts `fillColor` -- the PRIMARY fill, what most
 * XO-CHIP text and sprites are drawn in -- on the dimmest non-black
 * grey. It also creates a discontinuity with the one-plane case: a
 * colour 1 pixel renders solid white until the moment a ROM first
 * touches plane 1, and then drops to a 5/16 dither. The same pixel,
 * two appearances. That is what an XO-CHIP title with a drop-shadowed
 * intro looks like on this display: unreadable body text under a
 * brighter shadow.
 *
 * So the default puts `fillColor` at full brightness and orders the
 * rest below it. Background stays black in every mapping -- a dithered
 * background is noise across the whole screen, and 0 is the only level
 * that is exactly, seamlessly off.
 */
typedef enum {
	/* fillColor brightest. Continuous with the one-plane case, and
	 * right for the great majority of ROMs, which use plane 2 for
	 * shadows, outlines and detail behind a primary image. */
	C8_PAL_FILL = 0,

	/* Index order: 0, 5, 11, 16. Right for a ROM that genuinely
	 * treats its four colours as a ramp -- some do, for greyscale
	 * artwork. */
	C8_PAL_INDEX,

	/* Every non-background colour solid. No greys at all, maximum
	 * legibility, all colour information discarded. The one to reach
	 * for on a text-heavy ROM whose palette fights the display. */
	C8_PAL_SOLID,

	C8_PAL_COUNT
} c8_palette_t;

const char *c8_palette_name(int pal);

#define C8_OUT_W(s)   (128 * (s))
#define C8_OUT_H(s)   (64 * (s))
#define C8_STRIDE(s)  (16 * (s))

/* Worst case is scale 4: 512x256, 64 bytes a row.
 *
 * The four spare bytes are not slack. z_fb_hw_blit_mem() documents
 * that the blitter may read up to one word past the last source word
 * it needs whenever source and destination are not word-aligned to
 * each other, so the buffer must not end exactly at the last valid
 * byte of its allocation. */
#define C8_BUF_BYTES  (C8_STRIDE(C8_SCALE_MAX) * C8_OUT_H(C8_SCALE_MAX) + 4)

typedef struct {

	int scale;

	/* Output geometry, which depends only on scale -- see the header
	 * comment for why these do not change with the guest's
	 * resolution. */
	int w, h, stride;

	/* Output pixels per GUEST pixel in the mode last rendered:
	 * `scale` in hires, twice that in lores. This, and not `scale`,
	 * is what the tables are built against. */
	int pxw;
	bool grey;

	/* c8_palette_t. Only consulted when `grey` -- a one-plane ROM has
	 * no palette question to answer. */
	int pal;

	/* Mono: one entry per nibble of guest pixels, giving 4*pxw output
	 * bits in framebuffer order. */
	uint32_t exp[16];

	/* Grey: the same, but indexed by (plane0 nibble << 4 | plane1
	 * nibble) and by which of the four dither rows the output row
	 * falls on. 4KB, built once per mode change. */
	uint32_t gexp[4][256];

	/* Set when the tables no longer match pxw/grey. */
	bool tables_valid;

	/* Output rows touched by the last c8_render(), half-open. Empty
	 * when y1 <= y0. This is what chip8.c blits -- a full-screen blit
	 * every frame would work and would also be most of the cost. */
	int dirty_y0, dirty_y1;

	uint8_t buf[C8_BUF_BYTES];

} c8_render_t;

/* `scale` is clamped to 1, 2 or 4. Anything else rounds down to the
 * nearest of those rather than being rejected: the caller is usually
 * a keypress handler and a silently-ignored keypress is worse than a
 * slightly different scale. */
void c8_render_init(c8_render_t *r, int scale);

/* Returns true if the scale actually changed, so the caller knows
 * whether it needs to resize its window. */
bool c8_render_set_scale(c8_render_t *r, int scale);

/* Returns true if the palette actually changed, so the caller knows
 * whether it needs to force a redraw. */
bool c8_render_set_palette(c8_render_t *r, int pal);

/* Expand the guest display into r->buf.
 *
 * Consumes and clears c->dirty, so only guest rows that changed are
 * re-expanded. Pass force = true after anything that invalidates the
 * output without the guest having drawn -- a scale change, a window
 * move, a wm repaint.
 *
 * Returns true if anything was written, i.e. whether there is
 * something to blit.
 */
bool c8_render(c8_render_t *r, c8_t *c, bool force);

#endif
