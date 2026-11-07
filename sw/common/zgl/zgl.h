/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zgl.h -- zgl's window-system part: what GLX or WGL is elsewhere.
 * docs/zgl.md.
 *
 * zgl draws into the arcade runtime's 320x240 picture
 * (sw/common/games/zarcade.h, docs/arcade.md): the window, full
 * screen, F11, colour and the colour window are the runtime's, as for
 * every Zeitlos game. A program calls ar_init(), then zgl_open(), then
 * draws each frame with GL calls and ends it with zgl_frame(), before
 * ar_frame().
 */

#ifndef ZGL_ZGL_H
#define ZGL_ZGL_H

#include <stdbool.h>
#include <stdint.h>

#include "GL/gl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a frame is (zgl_desc_t.output) */
#define ZGL_LINES   1   /* lines and points into the picture as it is
                         * shown, by the line rasterizer, erasing only
                         * what moved: every board, every mode. Filled
                         * polygons are drawn as their outlines; no
                         * depth test (lines in submission order). */
#define ZGL_FRAMES  2   /* whole frames, filled, shaded and
                         * depth-tested, drawn in strips and put out
                         * a strip at a time: every board (software;
                         * link zgl_frames.o) */

/* Back ends (zgl_info_t.backend) */
#define ZGL_BACKEND_LINE      1   /* the line rasterizer (gpu_raster) */
#define ZGL_BACKEND_SOFTWARE  2
/* 3 was gpu_tri, the first triangle engine (retired) */
#define ZGL_BACKEND_T2        4   /* T2: the engine that runs the whole frame */

typedef struct {
	uint8_t  output;        /* ZGL_LINES or ZGL_FRAMES */
	/* how many planes a colour index is drawn in: indices 0 .. 2^n-1.
	 * 0: as many as the runtime shows (ar_colours()). Fewer leaves the
	 * other planes to the program (space3d's nebula). */
	uint8_t  index_bits;
	/* ZGL_LINES: the most lines (and points) a frame holds; 0 = 512 */
	uint16_t max_lines;
	/* ZGL_FRAMES: the most triangles and lines a frame holds; 0 = 2048
	 * (72 bytes each, from the heap) */
	uint16_t max_prims;
	/* ZGL_FRAMES: ZGL_CHUNKY and the like (below) */
	uint8_t  flags;
} zgl_desc_t;

/* zgl_desc_t.flags */
#define ZGL_CHUNKY     0x01  /* frames go to chunky VRAM, not through the
                              * arcade runtime: the program has chunky
                              * game mode on itself (docs/color.md,
                              * "Chunky"), as Doom does. Drawn by T2
                              * where the bitstream has it
                              * (docs/zgl_t2.md), else in software.
                              * Link zgl_chunky.o. */
#define ZGL_NO_ENGINE  0x02  /* ZGL_CHUNKY, in software even where there
                              * is T2 (to compare) */

typedef struct {
	uint8_t  output;
	uint8_t  backend;
	uint16_t max_lines;
	uint32_t frames;
	uint32_t lines;         /* in the last frame */
	uint32_t lines_drawn;   /* the rasterizer was given, last frame */
	uint32_t lines_erased;
	uint32_t overflows;     /* lines dropped (the list was full), ever */
	uint32_t prims;         /* ZGL_FRAMES: triangles and lines, last frame */
} zgl_info_t;

/* After ar_init(). False if out of memory, or ZGL_FRAMES without
 * zgl_frames.o linked. */
bool zgl_open(const zgl_desc_t *desc);
void zgl_close(void);

/* The end of a frame: what was submitted since the last one is the
 * picture now (with T2: is on its way there;
 * glFinish() waits for it). ZGL_LINES: last frame's lines that changed or went are
 * erased and this frame's drawn, in submission order. */
void zgl_frame(void);

/* The picture was repainted under zgl (the runtime's AR_EV_MODE, a
 * full-screen strobe of the program's own): none of last frame's
 * lines are on the glass any more, so none are erased. */
void zgl_invalidate(void);

const zgl_info_t *zgl_info(void);

/* -- textures, on T2 (docs/zgl.md, "Textures") --
 *
 * Texture `name` (1..63): 2^lw x 2^lh texels (each side 1 to 128), a
 * byte each, row by row, into T2's texture memory; `mode` how a texel x
 * and the triangle's own colour index c make the pixel (below); `key`:
 * texel 0 is not drawn. Made again with
 * the same name and size, it goes where it was. False: no T2 with
 * textures, or no room. Bind the name before compiling a list that uses
 * it; lists compiled before a texture is made again keep its place. */
#define ZGL_TEX_DECAL 0   /* x */
#define ZGL_TEX_ADD   1   /* x + c */
#define ZGL_TEX_TONE  2   /* c's ramp (16 entries), x's tone (0..15) lit by c's */
#define ZGL_TEX_RAMP  3   /* x's ramp and tone, lit by c's */
bool zglTexImage(unsigned name, int lw, int lh, const uint8_t *texels, int mode, bool key);
/* T2's texture memory in texels, 0 if it has none */
uint32_t zglTexRoom(void);

/* -- lists made ready for T2 now --
 *
 * On T2 a list is compiled into the engine's chunks the first time it is
 * called, and that frame pays for it (tens of thousands of cycles a
 * list). zglPrepareLists() compiles those of list..list+range-1 that are
 * not yet, as a call with lighting and GL_NORMALIZE as they are now
 * would: after making them, before the frames. Nothing without T2. */
void zglPrepareLists(GLuint list, GLsizei range);

/* -- extensions (not GL) ------------------------------------------------- */

/* Translation by whole units, for worlds wider than GLfixed's +-32767
 * (positions are kept with 24 integer bits, up to +-8,388,607). */
void zglTranslatei(GLint x, GLint y, GLint z);

/* glFrustum with whole-unit arguments: a far plane past GLfixed's
 * +-32767 without the double-precision glFrustum (and soft-float) */
void zglFrustumi(GLint l, GLint r, GLint b, GLint t, GLint n, GLint f);

/* The palette as ramps for shading: n entries from `first`, from
 * rgb444 `dark` to `light`. Smooth shading and lighting move along a
 * ramp (glMaterial GL_COLOR_INDEXES gives its ends), dithered between
 * neighbouring entries. */
void zglRamp(int first, int n, uint16_t dark, uint16_t light);

/* Sine and cosine of an angle in 1/65536 of a turn, as 16.16 */
GLfixed zglSin(uint32_t turn16);
GLfixed zglCos(uint32_t turn16);

#ifdef __cplusplus
}
#endif

#endif
