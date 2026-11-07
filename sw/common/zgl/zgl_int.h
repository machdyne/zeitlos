/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zgl_int.h -- zgl's state, shared by the front end (zgl.c) and the
 * back ends (zgl_lines.c). Not for programs.
 */

#ifndef ZGL_INT_H
#define ZGL_INT_H

#include <stdbool.h>
#include <stdint.h>

#include "GL/gl.h"
#include "zgl.h"

#define ZGL_W 320
#define ZGL_H 240

#define ZGL_MV_STACK    16
#define ZGL_PROJ_STACK  2
#define ZGL_POLY_MAX    32

/* a vertex on the screen, for ZGL_FRAMES: picture coordinates 28.4
 * (pixel centres whole), depth 0..65535 with 15 bits of fraction (0
 * near), the colour index 8.8 */
typedef struct {
	int32_t x, y, z, ci;
} zgl_svtx_t;

/* a vertex in clip coordinates, 20.12, with its colour index; which
 * of the six planes of the view volume it is outside (bits, 0: inside);
 * and, once found, the pixel it lands on */
typedef struct {
	int32_t x, y, z, w;
	int32_t ci;        /* the colour index, 8.8 (shading, lighting) */
	int16_t px, py;
	uint8_t c;
	uint8_t out;
	uint8_t pv;        /* px, py are valid */
	uint8_t sv;        /* ZGL_FRAMES: sx, sy, sz are valid */
	int32_t sx, sy;    /* ...its picture position (24.8, project()) */
	int32_t sz;        /* ...and depth (zgl_svtx_t's) */
} zgl_vtx_t;

/* transformed vertices, by object position: a model's lines share
 * corners, and GL_LINES gives each line its own copy of them */
#define ZGL_VCACHE_BITS 6
#define ZGL_VCACHE (1 << ZGL_VCACHE_BITS)
typedef struct {
	int32_t ox, oy, oz;
	uint32_t gen;
	zgl_vtx_t v;
	/* the lit colour index, for the normal and index it was found with */
	int32_t nx, ny, nz;
	uint8_t idx, ci_ok;
} zgl_vcache_t;

typedef struct {
	/* matrices: columns 0-2 12.20, column 3 24.8 (zgl.c) */
	int32_t mv[ZGL_MV_STACK][16];
	int32_t proj[ZGL_PROJ_STACK][16];
	int32_t mvp[16];
	uint8_t mv_top, proj_top;
	bool mvp_dirty;
	bool persp;                  /* the projection is a perspective one */
	int32_t near_w, far_w;       /* ...its near and far, as w (20.12) */
	bool p_exact[ZGL_PROJ_STACK];  /* glFrustum on identity: near, far as given */
	int32_t p_near[ZGL_PROJ_STACK], p_far[ZGL_PROJ_STACK];
	GLenum matrix_mode;

	/* viewport and scissor, in picture coordinates (y down) */
	int32_t vp_x, vp_y, vp_w, vp_h;
	int32_t sc_x0, sc_y0, sc_x1, sc_y1;

	bool cull, depth_test, scissor, lighting, normalize;
	/* GL_TEXTURE_2D (T2 only: zgl_t2.c); the texture coordinate (texels
	 * 12.4) and the texture bound (glBindTexture()'s name, 0 none) */
	bool texture;
	int32_t tex_s, tex_t;
	uint32_t tex_bound;
	bool light_on[2];
	GLenum cull_face;
	bool front_ccw;
	GLenum poly_front, poly_back;
	bool smooth;
	uint8_t output;          /* ZGL_LINES or ZGL_FRAMES */

	uint8_t index, clear_index;
	int32_t normal[3];
	/* lighting: a light's direction in eye space (16.16, unit length,
	 * towards the light) and diffuse brightness; the material's ambient
	 * and diffuse indices (8.8) */
	int32_t light_dir[2][3];
	int32_t light_d[2];
	/* a positional light (GL_POSITION's w not 0): where it is, in eye
	 * space (24.8), and its attenuation 1 / (kc + kl d + kq d^2), d in
	 * world units: kc 16.16, kl 8.24, kq 0.32 */
	bool light_pt[2];
	int32_t light_pos[2][3];
	int32_t light_kc[2], light_kl[2];
	uint32_t light_kq[2];
	int32_t mat_a, mat_d;

	/* primitive assembly */
	bool in_begin;
	GLenum prim;
	unsigned count;
	zgl_vtx_t vq[4];
	zgl_vtx_t *l0;           /* GL_LINES: the first of the pair (cached) */
	unsigned l0_slot;
	zgl_vtx_t poly[ZGL_POLY_MAX];
	int nv;
	/* a filled primitive's corners so far, as pointers: into the vertex
	 * cache, or to vs[] where a corner had to move out of the way of a
	 * newer one in the same cache entry -- so a corner is not copied
	 * on its way to the triangle */
	zgl_vtx_t *vp[4];
	zgl_vtx_t vs[4];

	GLenum error;

	/* a display list being made (zgl_lists.c): calls are recorded,
	 * and with GL_COMPILE_AND_EXECUTE carried out too */
	bool listing;

	uint32_t gen;            /* changes whenever a cached vertex would */
	uint32_t mvp_serial;     /* the mvp made again (zgl_t2.c sends it only then) */
	uint32_t lserial;        /* a matrix, a light or an enable changed: zgl_t2.c's light made again */
	zgl_vcache_t vc[ZGL_VCACHE];
} zgl_state_t;

extern zgl_state_t zgl_s;

void zgl_reset_state(void);
void zgl_prim_vertex(zgl_vtx_t *v);
void zgl_line(zgl_vtx_t *a, zgl_vtx_t *b);
void zgl_point(zgl_vtx_t *a);
void zgl_polygon(zgl_vtx_t **v, int n);

/* the back end's: one line (or a point, x0 == x1 and y0 == y1) in
 * picture coordinates, inside the viewport */
void zgl_back_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t c);

/* display lists (zgl_lists.c, linked only by a program that uses
 * them): record one call while a list is being made; true if it is to
 * be carried out as well */
enum {
	ZGLR_BEGIN = 1, ZGLR_END, ZGLR_VERTEX, ZGLR_NORMAL, ZGLR_INDEX, ZGLR_MATERIAL,
	ZGLR_TRANSLATE, ZGLR_SCALE, ZGLR_ROTATE, ZGLR_PUSH, ZGLR_POP, ZGLR_IDENTITY,
	ZGLR_SHADE, ZGLR_ENABLE, ZGLR_DISABLE, ZGLR_CALL, ZGLR_TEXCOORD, ZGLR_BIND
};
bool zglr_rec(int op, int32_t a, int32_t b, int32_t c, int32_t d) __attribute__((weak));

/* a recorded call */
typedef struct {
	uint8_t op;
	int32_t a, b, c, d;
} zglr_op_t;

/* T2 (zgl_t2.c, linked by programs that want it): the engine that runs
 * the whole frame */
bool zgl_t2_open(zgl_info_t *info) __attribute__((weak));
void zgl_t2_close(void) __attribute__((weak));
void zgl_t2_tri(const zgl_svtx_t *a, const zgl_svtx_t *b, const zgl_svtx_t *c) __attribute__((weak));
void zgl_t2_line(const zgl_svtx_t *a, const zgl_svtx_t *b) __attribute__((weak));
void zgl_t2_frame(void) __attribute__((weak));
bool zgl_t2_call(const zglr_op_t *ops, uint32_t n, void **cache, uint8_t *state) __attribute__((weak));
void zgl_t2_compile(const zglr_op_t *ops, uint32_t n, void **cache, uint8_t *state) __attribute__((weak));
void zgl_t2_free(void *p) __attribute__((weak));

/* zgl.c: v made a unit vector (16.16), as GL_NORMALIZE does; zero stays zero */
void zgl_unit3(int32_t *v);
/* zgl.c: the square root of a 64-bit value, rounded down */
uint32_t zgl_isqrt64(uint64_t v);
/* zgl.c: a positional light's attenuation, 16.16 (at most one), at
 * distance d (16.16 world units) */
int32_t zgl_atten(int l, int64_t d);
/* zgl.c: the matrix from object to clip, made if it is out of date */
void zgl_sync_mvp(void);
/* zgl_frames.c: the frame's rectangle (the viewport and scissor) */
void zglf_rect(int32_t *x0, int32_t *y0, int32_t *x1, int32_t *y1);
/* the code a frame runs for each vertex and triangle, kept together so
 * that it does not evict itself from the direct-mapped instruction
 * cache (8 KB on sergei_ml2): the linker scripts put .text.hot first */
#define ZGL_HOT __attribute__((section(".text.hot.zgl")))

#define ZGL_REC(op, a, b, c, d) \
	do { if (zgl_s.listing && zglr_rec && !zglr_rec((op), (a), (b), (c), (d))) return; } while (0)

/* the recorded calls' own way in, for zgl_lists.c */
void zgl_vertex_pos(int32_t x, int32_t y, int32_t z);   /* 24.8 */
void zgl_translate_pos(int32_t x, int32_t y, int32_t z);
void zgl_material(int32_t a, int32_t d);                /* 8.8 */

/* ZGL_FRAMES (zgl_frames.c, linked only by a program that uses it:
 * weak here) */
bool zglf_open(const zgl_desc_t *d, zgl_info_t *info) __attribute__((weak));
void zglf_close(void) __attribute__((weak));
void zglf_frame(void) __attribute__((weak));
void zglf_tri(const zgl_svtx_t *a, const zgl_svtx_t *b, const zgl_svtx_t *c) __attribute__((weak));
/* the same from projected vertices (sv set), every corner's index ci,
 * or with flat the one given: no zgl_svtx_t in between */
void zglf_tri_v(const zgl_vtx_t *a, const zgl_vtx_t *b, const zgl_vtx_t *c,
	bool flat, int32_t ci) __attribute__((weak));
void zglf_line(const zgl_svtx_t *a, const zgl_svtx_t *b) __attribute__((weak));
/* ZGL_CHUNKY (zgl_chunky.c, target only: weak here). zglc_open()
 * says whether T2 will draw; zglc_put() hands it a command word;
 * zglc_strip() writes a software strip's rows; zglc_finish() waits
 * for T2. */
/* 0: none (the software writes strips), 2: T2 */
int zglc_open(uint8_t flags) __attribute__((weak));
void zglc_put(uint32_t w) __attribute__((weak));
/* T2's command register, for zgl_t2.c to store to */
volatile uint32_t *zglc_cmd(void) __attribute__((weak));
void zglc_strip(int32_t y0, int rows, int32_t cx0, int32_t cx1, const uint8_t *col) __attribute__((weak));
void zglc_finish(void) __attribute__((weak));
/* T2's VBASE for the frame starting, whose rect is x0..x1, y0..y1:
 * chunky's picture (z_chunky_phys(): VRAM, or with `VRAM_SDRAM the
 * picture in main memory, allocated when chunky first comes on, which
 * may move with z_chunky_set_pic()) -- or, with `VRAM_SDRAM and a frame
 * that is the whole picture, one of three pages, flipped in at
 * zglc_finish() (zgl_chunky.c, "pages") */
void zglc_picture(int32_t x0, int32_t y0, int32_t x1, int32_t y1) __attribute__((weak));
/* a paged frame (above), finished, shown from the next video frame:
 * ar_frame() calls it (common/games/zarcade_sys.c) */
void zglc_present(void) __attribute__((weak));
/* the frame handed over, shown if T2 has it out; never waits (zgl_chunky.c) */
void zglc_poll(void) __attribute__((weak));
/* T2's texture memory (zgl_chunky.c): its size in texels (0: none),
 * and a word of four texels written at word `word` once the engine is
 * idle */
uint32_t zglc_tex_room(void) __attribute__((weak));
void zglc_tex_put(uint32_t word, uint32_t v) __attribute__((weak));
/* zgl_t2.c: a bound texture's word for a compiled chunk (0: none) */
uint32_t zgl_t2_tex_word(uint32_t name) __attribute__((weak));

#endif
