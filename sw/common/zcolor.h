#ifndef ZCOLOR_H
#define ZCOLOR_H

/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Game-mode colour: layouts, palettes and planar drawing. See
 * docs/color.md.
 *
 * zsoc.h's z_color_* helpers are the register interface and nothing
 * more. This is the layer every colour program would otherwise write
 * for itself: where each plane lives, how to fill a rectangle in
 * colour c, how to put a sprite into four planes, and how to do all of
 * that across the 640x480 torus without drawing off the end of the
 * framebuffer.
 *
 * -- the model, restated --
 *
 * The framebuffer is still one 640x480x1bpp surface. In game mode the
 * viewport shows plane 0; plane k (1..3) is the same rectangle moved
 * by 320 columns, 240 rows or both, modulo 640x480. A pixel's colour
 * is bit k from plane k, through a 16-entry palette.
 *
 * Every function here takes coordinates IN PLANE 0 -- the same
 * framebuffer coordinates a monochrome game draws at -- and works out
 * the other planes itself. Plane 0 coordinates may run past 639 or 479
 * (a scrolling game's sliding window does): everything here folds onto
 * the torus and splits a rectangle that crosses the seam, so a caller
 * never has to.
 *
 * -- what the drawing costs --
 *
 * One blitter operation per plane per piece: a 16-colour fill is four
 * fills, a 16-colour sprite eight blits (mask and data per plane).
 * See docs/color.md, "Drawing".
 *
 * Needs zgfx.o (the blitter) linked in, like zgame.
 */

#include <stdint.h>
#include <stdbool.h>

#include "zsoc.h"

/* Where the planes are. `planes` is 1..4; `offsets` is
 * Z_COLOR_OFFSETS(p1, p2, p3) from zsoc.h. */
typedef struct {
	uint8_t planes;
	uint8_t offsets;
} z_color_layout_t;

/*
 * The useful layouts (docs/color.md, "Plane placement"):
 *
 *   z_color_16        planes at +320, +240, +320+240: the four
 *                     quadrants. No spare page; viewport fixed at (0,0).
 *
 *   z_color_8         planes at +320 and +240. The quadrant at
 *                     (320,240) is spare: sprite storage.
 *
 *   z_color_4_right   plane 1 at +320 columns. The viewport lives in
 *                     the LEFT half: double-buffer by flipping between
 *                     y=0 and y=240, or scroll vertically (zgame's
 *                     Z_GAME_SCROLL_V pages).
 *
 *   z_color_4_below   plane 1 at +240 rows. The viewport lives in the
 *                     TOP half: double-buffer by flipping between x=0
 *                     and x=320, or scroll horizontally (zgame's
 *                     Z_GAME_SCROLL_H pages, wrap on).
 *
 *   z_color_mono      one plane: monochrome game mode, through the
 *                     palette (entries 0 and 1).
 */
extern const z_color_layout_t z_color_16;
extern const z_color_layout_t z_color_8;
extern const z_color_layout_t z_color_4_right;
extern const z_color_layout_t z_color_4_below;
extern const z_color_layout_t z_color_mono;

/* The power-on hardware palette (EGA order), and the Commodore 64's
 * (black, white, red, cyan, ... -- 0 black and 1 white, so a
 * monochrome picture looks the same through it). RGB444. */
extern const uint16_t z_color_palette_ega[16];
extern const uint16_t z_color_palette_c64[16];

/* Colour on, with this layout. Adopted at the next frame boundary,
 * together with any GAME/VIEW write in the same frame. False on a
 * machine without colour (z_color_available()). Game mode must be on
 * for it to show; turning game mode off turns colour off. */
bool z_color_begin(const z_color_layout_t *l);

/* Colour off; game mode is left as it is. */
void z_color_end(void);

/* Load n palette entries starting at 0, at a frame boundary: waits for
 * the next one (z_game_wait_frame()) first, so the change cannot land
 * mid-picture. Up to 16.7ms. */
void z_color_palette_load(const uint16_t *rgb444, int n);

/* The same, without waiting -- for a caller that has just waited. */
void z_color_palette_load_now(const uint16_t *rgb444, int n);

/* Framebuffer position of (x, y) in plane `plane`, folded onto the
 * 640x480 torus. Plane 0 is (x, y) folded. */
void z_color_plane_xy(const z_color_layout_t *l, int plane,
	int x, int y, int *px, int *py);

/* A filled rectangle in colour c, every active plane set or cleared to
 * match c's bits. Bits of c above the layout's planes are ignored. */
void z_color_fill_rect(const z_color_layout_t *l,
	int x, int y, int w, int h, int c);

/* A 1bpp bitmap from main memory drawn in colour c where its bits are
 * set, leaving the rest alone (text, line art, a one-colour sprite).
 * Bitmap format and src_x/src_y/stride as z_fb_hw_blit_mem(). False if
 * the blitter has no raster ops (z_fb_hw_rop_available()). */
bool z_color_blit_mono(const z_color_layout_t *l,
	const void *src, int src_stride, int src_x, int src_y,
	int x, int y, int w, int h, int c);

/* An opaque multi-plane image: src[k] is plane k's bitmap, for each of
 * the layout's planes, all with the same stride and geometry. A NULL
 * src[k] clears that plane's rectangle. False if the blitter has no
 * memory-copy mode. */
bool z_color_blit_planes(const z_color_layout_t *l,
	const void *const *src, int src_stride, int src_x, int src_y,
	int x, int y, int w, int h);

/* A masked multi-plane sprite: in every plane, the mask is cleared
 * (ANDN) and that plane's data ORed in. mask is 1 where the sprite is
 * opaque; src[k] must be 0 wherever the mask is 0. A NULL src[k] is a
 * plane with no bits set. False if the blitter has no raster ops. Two
 * passes per plane, so draw into a page not on screen. */
bool z_color_blit_sprite(const z_color_layout_t *l,
	const void *const *src, const void *mask, int src_stride,
	int src_x, int src_y, int x, int y, int w, int h);

/* -- one plane at a time --
 *
 * The building blocks of everything above, for drawing that does not
 * fit them: a two-colour tile (one colour where the bitmap is set,
 * another where it is clear), a sprite with an outline in a second
 * colour. Same coordinates, same torus split. `plane` is 0..3. */
void z_color_plane_fill(const z_color_layout_t *l, int plane,
	int x, int y, int w, int h, int bit);
bool z_color_plane_blit(const z_color_layout_t *l, int plane,
	const void *src, int src_stride, int src_x, int src_y,
	int x, int y, int w, int h, int rop);

#endif
