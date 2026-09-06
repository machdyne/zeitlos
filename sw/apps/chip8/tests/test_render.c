/*
 * chip8 -- renderer tests. Host build, no framebuffer.
 *
 * What these are actually for: a dither that is half a pixel out of
 * phase, or a bit order that is reversed, both produce output that
 * "looks like something" and passes any test that only asks whether
 * pixels got set. So these check exact positions and exact counts.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../render.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond) do { \
	checks++; \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} \
} while (0)

#define CHECK_EQ(got, want) do { \
	long g_ = (long)(got), w_ = (long)(want); \
	checks++; \
	if (g_ != w_) { \
		printf("FAIL %s:%d: %s == %ld, wanted %ld\n", \
			__FILE__, __LINE__, #got, g_, w_); \
		failures++; \
	} \
} while (0)

/* Output pixel, in the framebuffer's own bit order: bit (x & 31) of
 * word (x >> 5), least significant bit leftmost. Written out longhand
 * rather than reusing anything from render.c, so a test cannot agree
 * with the code by sharing its mistake. */
static int out_px(const c8_render_t *r, int x, int y) {
	const uint32_t *row = (const uint32_t *)(r->buf + y * r->stride);
	return (int)((row[x >> 5] >> (x & 31)) & 1);
}

static void poke(c8_t *c, int p, int x, int y) {
	c->px[p][y][x >> 5] |= (uint32_t)1 << (31 - (x & 31));
	c->dirty |= (uint64_t)1 << y;
}

static c8_t *fresh(c8_profile_t prof) {
	c8_t *c = malloc(sizeof(*c));
	c8_init(c, c8_profile(prof), 1);
	return c;
}

/* -- geometry ------------------------------------------------------- */

static void test_geometry(void) {

	c8_render_t *r = malloc(sizeof(*r));
	c8_t *c = fresh(C8_PROFILE_SCHIP);

	int s;
	for (s = 1; s <= 4; s *= 2) {
		c8_render_init(r, s);
		CHECK_EQ(r->w, 128 * s);
		CHECK_EQ(r->h, 64 * s);
		CHECK_EQ(r->stride, 16 * s);
	}

	/* Anything that is not 1, 2 or 4 rounds down rather than being
	 * refused -- the caller is a keypress handler. */
	c8_render_init(r, 3);
	CHECK_EQ(r->scale, 2);
	c8_render_init(r, 99);
	CHECK_EQ(r->scale, 4);
	c8_render_init(r, 0);
	CHECK_EQ(r->scale, 1);

	/* The output is the same size in both guest resolutions. That is
	 * the whole point of defining scale against the hires pixel: a ROM
	 * switching modes must not resize the window. */
	c8_render_init(r, 2);
	c8_render(r, c, true);
	CHECK_EQ(r->pxw, 4);            /* lores */
	c->hires = true; c->w = 128; c->h = 64;
	c8_render(r, c, true);
	CHECK_EQ(r->pxw, 2);            /* hires, same output size */
	CHECK_EQ(r->w, 256);
	CHECK_EQ(r->h, 128);

	free(r); free(c);

}

/* -- bit order ------------------------------------------------------ */

static void test_bit_order(void) {

	c8_render_t *r = malloc(sizeof(*r));
	c8_t *c = fresh(C8_PROFILE_CHIP8);

	c8_render_init(r, 1);
	poke(c, 0, 0, 0);
	c8_render(r, c, true);

	/* Guest (0,0) is the LEFTMOST pixel, and the leftmost output pixel
	 * is the LOWEST bit. A reversed table would put this at x = 127
	 * and still look like a picture. */
	CHECK_EQ(out_px(r, 0, 0), 1);
	CHECK_EQ(out_px(r, 1, 0), 1);   /* lores at scale 1 is 2x2 */
	CHECK_EQ(out_px(r, 0, 1), 1);
	CHECK_EQ(out_px(r, 1, 1), 1);
	CHECK_EQ(out_px(r, 2, 0), 0);
	CHECK_EQ(out_px(r, 0, 2), 0);
	CHECK_EQ(out_px(r, 127, 0), 0);

	/* The far corner, which is the case that catches an off-by-one in
	 * the word packing rather than in the table. */
	memset(c->px, 0, sizeof(c->px));
	poke(c, 0, 63, 31);
	c8_render(r, c, true);
	CHECK_EQ(out_px(r, 126, 62), 1);
	CHECK_EQ(out_px(r, 127, 63), 1);
	CHECK_EQ(out_px(r, 125, 63), 0);

	free(r); free(c);

}

static void test_scaling(void) {

	c8_render_t *r = malloc(sizeof(*r));
	c8_t *c = fresh(C8_PROFILE_CHIP8);
	int x, y, on;

	/* One lores pixel at scale 4 is an 8x8 block and nothing else. */
	c8_render_init(r, 4);
	poke(c, 0, 3, 2);
	c8_render(r, c, true);

	on = 0;
	for (y = 0; y < r->h; y++)
		for (x = 0; x < r->w; x++)
			on += out_px(r, x, y);

	CHECK_EQ(on, 64);
	CHECK_EQ(out_px(r, 24, 16), 1);
	CHECK_EQ(out_px(r, 31, 23), 1);
	CHECK_EQ(out_px(r, 23, 16), 0);
	CHECK_EQ(out_px(r, 32, 16), 0);
	CHECK_EQ(out_px(r, 24, 24), 0);

	free(r); free(c);

}

/* -- greys ---------------------------------------------------------- */

/* Count lit pixels in the w x h block a single guest pixel occupies. */
static int block_count(const c8_render_t *r, int gx, int gy) {
	int x, y, n = 0;
	for (y = 0; y < r->pxw; y++)
		for (x = 0; x < r->pxw; x++)
			n += out_px(r, gx * r->pxw + x, gy * r->pxw + y);
	return n;
}

static void test_grey_levels(void) {

	c8_render_t *r = malloc(sizeof(*r));
	c8_t *c = fresh(C8_PROFILE_XOCHIP);

	/* scale 2, lores: pxw 4, so each guest pixel is a full 4x4 dither
	 * cell and the ratios are exact rather than approximate. */
	c8_render_init(r, 2);
	c->two_plane = true;

	/* colour 1 at (0,0), colour 2 at (1,0), colour 3 at (2,0),
	 * colour 0 at (3,0). */
	poke(c, 0, 0, 0);
	poke(c, 1, 1, 0);
	poke(c, 0, 2, 0);
	poke(c, 1, 2, 0);
	c8_render(r, c, true);

	CHECK_EQ(r->pxw, 4);

	/* Default palette is C8_PAL_FILL: colour 1 is Octo's fillColor,
	 * the PRIMARY fill, so it gets full brightness. Index order is
	 * not brightness order -- see render.h. */
	CHECK_EQ(block_count(r, 0, 0), 16);   /* colour 1 -- solid */
	CHECK_EQ(block_count(r, 1, 0), 11);   /* colour 2 */
	CHECK_EQ(block_count(r, 2, 0), 5);    /* colour 3 -- the blend */
	CHECK_EQ(block_count(r, 3, 0), 0);    /* colour 0 -- background */

	/* C8_PAL_INDEX is the literal index ramp, for a ROM that really
	 * does treat its colours as greyscale. */
	CHECK(c8_render_set_palette(r, C8_PAL_INDEX));
	c8_render(r, c, true);
	CHECK_EQ(block_count(r, 0, 0), 5);
	CHECK_EQ(block_count(r, 1, 0), 11);
	CHECK_EQ(block_count(r, 2, 0), 16);

	/* C8_PAL_SOLID throws the colour away and keeps legibility. */
	CHECK(c8_render_set_palette(r, C8_PAL_SOLID));
	c8_render(r, c, true);
	CHECK_EQ(block_count(r, 0, 0), 16);
	CHECK_EQ(block_count(r, 1, 0), 16);
	CHECK_EQ(block_count(r, 2, 0), 16);
	CHECK_EQ(block_count(r, 3, 0), 0);    /* background stays black */

	CHECK(!c8_render_set_palette(r, C8_PAL_SOLID));   /* no change */
	CHECK(!c8_render_set_palette(r, 99));             /* rejected */

	free(r); free(c);

}

/* A one-plane program's colour 1 is FOREGROUND. Getting this wrong
 * renders every CHIP-8 game as a dim wash, and it is invisible to any
 * test that only asks whether pixels are set. */
static void test_mono_is_solid(void) {

	c8_render_t *r = malloc(sizeof(*r));
	c8_t *c = fresh(C8_PROFILE_CHIP8);

	c8_render_init(r, 2);
	poke(c, 0, 0, 0);
	c8_render(r, c, true);

	CHECK(!r->grey);
	CHECK_EQ(block_count(r, 0, 0), 16);

	/* ... and it STAYS foreground once the program declares itself
	 * two-plane. This is the invariant that was broken: with the
	 * palette keyed to index order, this pixel dropped from solid to a
	 * 5/16 dither the moment a ROM first selected plane 1, so an
	 * XO-CHIP title's body text went unreadable as soon as it drew its
	 * first drop shadow. Same pixel, same colour index, same
	 * appearance. */
	c->two_plane = true;
	c->dirty = ~(uint64_t)0;
	c8_render(r, c, true);
	CHECK(r->grey);
	CHECK_EQ(block_count(r, 0, 0), 16);

	free(r); free(c);

}

/* The dither must be anchored to the CONTENT, so two adjacent pixels
 * of the same colour tile into a continuous texture rather than each
 * restarting the pattern. Checked as a ratio over a run: a phase error
 * would keep the per-pixel count right and the joins wrong, so count
 * over the whole run, not per block. */
static void test_dither_tiles(void) {

	c8_render_t *r = malloc(sizeof(*r));
	c8_t *c = fresh(C8_PROFILE_XOCHIP);
	int x, y, n = 0;

	c8_render_init(r, 1);           /* pxw 2 -- deliberately small */

	/* A 16x16 block of colour 3 -- the blend, which is level 5/16
	 * under the default palette and so actually dithers. Colour 1 is
	 * solid and would tell us nothing about phase. */
	for (x = 0; x < 16; x++)
		for (y = 0; y < 16; y++) {
			poke(c, 0, x, y);
			poke(c, 1, x, y);
		}
	c->two_plane = true;

	c8_render(r, c, true);

	for (y = 0; y < 32; y++)
		for (x = 0; x < 32; x++)
			n += out_px(r, x, y);

	/* 32x32 output pixels of level 5/16. With a content-anchored
	 * dither that is exactly 5/16 of them; with a per-block dither at
	 * pxw 2 it would be some other number entirely, because each 2x2
	 * block would sample the same corner of the matrix every time. */
	CHECK_EQ(n, 32 * 32 * 5 / 16);

	free(r); free(c);

}

/* -- dirty tracking ------------------------------------------------- */

static void test_dirty_band(void) {

	c8_render_t *r = malloc(sizeof(*r));
	c8_t *c = fresh(C8_PROFILE_CHIP8);

	c8_render_init(r, 2);
	c8_render(r, c, true);
	CHECK_EQ(r->dirty_y0, 0);
	CHECK_EQ(r->dirty_y1, 128);

	/* Nothing drawn since: nothing to blit. */
	CHECK(!c8_render(r, c, false));

	poke(c, 0, 5, 10);
	CHECK(c8_render(r, c, false));
	CHECK_EQ(r->dirty_y0, 10 * 4);
	CHECK_EQ(r->dirty_y1, 11 * 4);

	/* Two rows far apart give one band covering both -- one blit of a
	 * few extra rows beats several blits of exactly the right ones on
	 * an engine with no queue. */
	poke(c, 0, 0, 2);
	poke(c, 0, 0, 20);
	CHECK(c8_render(r, c, false));
	CHECK_EQ(r->dirty_y0, 2 * 4);
	CHECK_EQ(r->dirty_y1, 21 * 4);

	free(r); free(c);

}

/* A scale change must force a full re-expansion even with no guest
 * drawing since -- the old buffer is the right pixels at the wrong
 * size. */
static void test_scale_change_forces_full(void) {

	c8_render_t *r = malloc(sizeof(*r));
	c8_t *c = fresh(C8_PROFILE_CHIP8);

	c8_render_init(r, 1);
	poke(c, 0, 0, 0);
	c8_render(r, c, true);
	CHECK(!c8_render(r, c, false));

	CHECK(c8_render_set_scale(r, 4));
	CHECK(c8_render(r, c, false));
	CHECK_EQ(r->dirty_y1, 64 * 4);
	CHECK_EQ(block_count(r, 0, 0), 64);

	CHECK(!c8_render_set_scale(r, 4));   /* no change, no work */

	free(r); free(c);

}

/* Switching guest resolution must rebuild the tables. Without this the
 * output stays at the old pxw and the picture is half-size in one
 * corner -- which looks like a scrolling bug, not a table bug. */
static void test_mode_change(void) {

	c8_render_t *r = malloc(sizeof(*r));
	c8_t *c = fresh(C8_PROFILE_SCHIP);

	c8_render_init(r, 2);
	poke(c, 0, 0, 0);
	c8_render(r, c, true);
	CHECK_EQ(block_count(r, 0, 0), 16);

	c->hires = true; c->w = 128; c->h = 64;
	memset(c->px, 0, sizeof(c->px));
	poke(c, 0, 0, 0);
	c8_render(r, c, false);
	CHECK_EQ(r->pxw, 2);
	CHECK_EQ(block_count(r, 0, 0), 4);

	free(r); free(c);

}

int main(void) {

	test_geometry();
	test_bit_order();
	test_scaling();
	test_grey_levels();
	test_mono_is_solid();
	test_dither_tiles();
	test_dirty_band();
	test_scale_change_forces_full();
	test_mode_change();

	printf("%s: %d checks, %d failures\n",
		failures ? "FAILED" : "ok", checks, failures);

	return failures ? 1 : 0;

}
