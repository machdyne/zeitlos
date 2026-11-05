/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zgl_t2.h -- T2, the triangle engine that runs the whole frame
 * (claude/zgl-t2-proposal.md, docs/zgl_t2.md): the frame program and
 * the compiled display lists, as zgl (zgl_t2.c) writes them and the
 * engine (its C model, tests/t2_model.c) reads them.
 *
 * The engine has no read port: the CPU streams the frame into its
 * command FIFO, word by word. A compiled list is kept in RAM already in
 * the stream's format, so a frame copies it with no work a vertex.
 * zgl keeps the frame's stream in RAM too, to send again for each group
 * of strips when a frame has more triangles than the engine holds.
 *
 * -- The frame program: commands, each a header word and its arguments --
 *
 *   FRAME  op | clear (bits 7:0)              begins the frame
 *          x0 | x1 << 16                      its rectangle (the
 *          y0 | y1 << 16                      viewport and scissor)
 *   VIEW   op                                 the viewport
 *          vp_x | vp_y << 16, vp_w | vp_h << 16
 *   XFORM  op | persp (bit 0)                 object to clip
 *          16 words: the matrix, column-major: columns 0-2 12.20,
 *          column 3 24.8 (zgl's mvp)
 *          near_w, far_w (20.12; far INT32_MAX: none)
 *   LIGHT  op | lighting (0) | light 0 on (1) | light 1 on (2)
 *          light 0: direction in the model's space x, y, z (16.16:
 *          n . L >> 16 is GL's n_eye . L_eye), brightness (16.16)
 *          light 1: the same
 *          material ambient, diffuse (colour indices 8.8)
 *   PLIGHT op | light (0) | on (1) | unit (2)    light 0 or 1 positional
 *          (on), or not: where it is in the model's space x, y, z
 *          (24.8), the model's scale s (16.16: eye units a model unit),
 *          the attenuation's kc (16.16), kl (8.24), kq (0.32), for eye
 *          distance. A vertex's light: n . (P - v) / |P - v|, times s
 *          unless the normals are unit (unit), times the attenuation
 *          1 / (kc + kl d + kq d^2) at d = s |P - v|. Takes the place of
 *          LIGHT's direction for that light until off.
 *   STATE  op | depth (0) | smooth (1) | cull (3:2: 0 none, 1 back,
 *          2 front, 3 both) | front_ccw (4) | textures (5: a T2_C_TEX
 *          chunk's triangles are textured; else they are drawn as if
 *          it had no texture)
 *   DRAW   op                                 a compiled list's chunk
 *          index (8.8) for vertices without one
 *          normal x, y, z (16.16) for vertices without one
 *          then the chunk itself, inline (below)
 *   TRI    op | depth (0)                     a triangle the CPU set up
 *          3 corners: x, y (28.4), z (16.15), ci (8.8)
 *   LINE   op | depth (0)                     a line the CPU set up
 *          2 ends: x, y, z, ci
 *   END    op                                 the frame is done
 *
 * -- A compiled list: chunks of up to T2_CHUNK_V vertices --
 *
 *   word 0  nv | nt << 16
 *   word 1  shift | flags << 8 | nl << 16     position = x << shift (24.8)
 *   word 2  material ambient (8.8)            when T2_C_MAT
 *   word 3  material diffuse (8.8)
 *   word 4  the texture (T2_TEX_*)             when T2_C_TEX
 *   nv vertices, 4 words each (5 with T2_C_TEX):
 *           x | y << 16, z | ci << 16 (ci 8.8, unsigned),
 *           nx | ny << 16, nz (normals 2.14),
 *           s | t << 16 (T2_C_TEX: texels 12.4, signed)
 *   nt triangles, 2 words each:
 *           a | b << 16, c | provoking << 16  (vertex numbers; the
 *           provoking one's colour is a flat triangle's)
 *   nl lines, a word each: a | b << 16       (flat: b's colour)
 */

#ifndef ZGL_T2_H
#define ZGL_T2_H

#include <stdint.h>

#define T2_FRAME  0x01u
#define T2_VIEW   0x02u
#define T2_XFORM  0x03u
#define T2_LIGHT  0x04u
#define T2_STATE  0x05u
#define T2_DRAW   0x06u
#define T2_TRI    0x07u
#define T2_LINE   0x08u
#define T2_PLIGHT 0x09u
#define T2_END    0x0fu
#define T2_OP(w)  ((w) >> 24)

#define T2_CHUNK_V 64       /* the engine's vertex buffer */

/* chunk flags */
#define T2_C_MAT     0x01u    /* its own material (else the LIGHT's) */
#define T2_C_NORMAL  0x02u    /* vertices have normals (else the DRAW's) */
#define T2_C_INDEX   0x04u    /* vertices have indices (else the DRAW's) */
#define T2_C_TEX     0x08u    /* a texture word, and s | t a vertex */

/* A texture word: where the texture is in the engine's texture memory
 * (texels, a byte each), its size (2^lw x 2^lh, each 1 to 128, repeated
 * both ways), how its texel and the colour index make the pixel, and
 * whether texel 0 is not drawn at all (a cut-out) */
#define T2_TEX(base, lw, lh, mode, key) \
	((uint32_t)(base) | (uint32_t)(lw) << 16 | (uint32_t)(lh) << 19 | \
	 (uint32_t)(mode) << 22 | ((key) ? 1u << 24 : 0u))
#define T2_TEX_BASE(w)  ((w) & 0xffffu)
#define T2_TEX_LW(w)    (((w) >> 16) & 7u)
#define T2_TEX_LH(w)    (((w) >> 19) & 7u)
#define T2_TEX_MODE(w)  (((w) >> 22) & 3u)
#define T2_TEX_KEY(w)   (((w) >> 24) & 1u)
/* the modes: c the pixel's colour index as untextured, x the texel */
#define T2_TM_DECAL  0u       /* x */
#define T2_TM_ADD    1u       /* x + c */
#define T2_TM_TONE   2u       /* c's ramp, x's tone lit: (c & 0xf0) | (x & 15) (c & 15 + 1) / 16 */
#define T2_TM_RAMP   3u       /* x's ramp and tone, lit: (x & 0xf0) | (x & 15) (c & 15 + 1) / 16 */

/* the engine's ID word (zsoc.h's Z_GTRI_W_ID): "ZTR2"; commands go to
 * Z_GTRI_W_CMD as today's engine's do */
#define T2_ID        0x5A545232u

/* -- Inside the engine: the geometry unit's records for the raster --
 *
 * The geometry unit (t2fw.c, on the engine's own small RISC-V core)
 * carries out the stream and writes a record for each triangle and line
 * that is drawn; at END the raster draws the frame strip by strip from
 * the records, each strip from every record that touches it.
 *
 * TRI (11 words); rows ylo..yhi and columns bx0..bx1 already inside the
 * frame's rectangle; the short edge is v0-v1 above row ymid, v1-v2 from
 * it on:
 *   0  1 | depth << 2 | short_right << 3 | ylo << 8 | yhi << 16 | ymid << 24
 *   1  bx0 | bx1 << 16
 *   2  v0.x | v0.y << 16   (28.4, sorted by y)
 *   3  v1.x | v1.y << 16
 *   4  v2.x | v2.y << 16
 *   5  z at (bx0, ylo)      6 dz/dx  7 dz/dy     (16.15, mod 2^32)
 *   8  ci at (bx0, ylo)     9 dci/dx 10 dci/dy   (8.8, mod 2^32)
 *
 * LINE (10 words): steps 0..n, each its pixel ((x + 8 << 16) >> 20):
 *   0  2 | depth << 2 | ylo << 8 | yhi << 16
 *   1  n
 *   2  x  3 y  4 dx  5 dy   (28.4 << 16)
 *   6  z  7 dz  8 ci  9 dci
 *
 * TTRI (21 words), a textured triangle: TRI's 11 (but 3 in bits 1:0),
 * then
 *   11 the texture word
 *   12 A at (bx0, ylo)  13 dA/dx  14 dA/dy    s q >> 8   (mod 2^32)
 *   15 B                16 dB/dx  17 dB/dy    t q >> 8
 *   18 Q                19 dQ/dx  20 dQ/dy    q = 2^24 wmin / w
 * where w is the corner's clip w and wmin the least of the three: the
 * planes are s/w, t/w and 1/w (linear on the screen), so a pixel's
 * texel is perspective-correct, s = A 256 / Q (t2_uv(), below). The
 * pixel's index is then the texture mode's of the texel and the index
 * TRI would draw, and with the key a texel 0 is not drawn.
 */
#define T2_REC_TRI   1u
#define T2_REC_LINE  2u
#define T2_REC_TTRI  3u
#define T2_REC_WORDS 11
#define T2_REC_TWORDS 21
#define T2_RECS      512      /* the record list's room */
/* the texture memory: 2^T2_TEX_LOG2 texels (rtl/gpu/gpu_t2.v's TEX_LOG2;
 * the engine says its own in CAPS, zsoc.h) */
#define T2_TEX_LOG2  13

/* -- a textured pixel, as the raster works it out (rtl/gpu/gpu_t2.v) --
 *
 * s = A 256 / Q in 12.4, by a reciprocal: Q (at least 32) is 2^e (1 + f),
 * 1/Q from a 1,024-entry table of f's first ten bits, and A brought to 18
 * bits for the multiplier first. Bit for bit what the RTL does. */
static inline int t2_msb(uint32_t v) { int n = 0; while (v >>= 1) n++; return n; }

static inline uint32_t t2_recip(uint32_t i) {        /* 2^28 / (2049 + 2i), rounded */
	uint32_t d = 2049u + 2u * i;
	return ((1u << 28) + d / 2u) / d;
}

static inline uint32_t t2_div(int32_t n, int e, uint32_t r) {
	uint32_t a = n < 0 ? ~(uint32_t)n : (uint32_t)n;
	int bl = a ? t2_msb(a) + 1 : 0, sa = bl > 17 ? bl - 17 : 0;
	int64_t p = (int64_t)(n >> sa) * (int64_t)r;
	return (uint32_t)(p >> (9 + e - sa));
}

static inline void t2_uv(int32_t A, int32_t B, int32_t Q, uint32_t *s, uint32_t *t) {
	uint32_t q = Q < 32 ? 32u : (uint32_t)Q, r;
	int e = t2_msb(q);
	r = t2_recip(((q << (31 - e)) >> 21) & 1023u);
	*s = t2_div(A, e, r);
	*t = t2_div(B, e, r);
}

/* the pixel's index from the texel x and the untextured index c */
static inline uint8_t t2_combine(uint32_t mode, uint8_t x, uint8_t c) {
	uint8_t tone = (uint8_t)(((x & 15u) * ((c & 15u) + 1u)) >> 4);
	switch (mode) {
	case T2_TM_DECAL: return x;
	case T2_TM_ADD:   return (uint8_t)(x + c);
	case T2_TM_TONE:  return (uint8_t)((c & 0xf0u) | tone);
	default:          return (uint8_t)((x & 0xf0u) | tone);
	}
}

/* the texel's place in texture memory (2^log2 bytes) for s, t (12.4) */
static inline uint32_t t2_texel_at(uint32_t tw, uint32_t s, uint32_t t, int log2) {
	uint32_t lw = T2_TEX_LW(tw), lh = T2_TEX_LH(tw);
	uint32_t u = (s >> 4) & ((1u << lw) - 1u), v = (t >> 4) & ((1u << lh) - 1u);
	return (T2_TEX_BASE(tw) + (v << lw) + u) & ((1u << log2) - 1u);
}

/* the geometry unit's way out (t2fw.c calls these; the core's are
 * registers, the host's are the model's) */
#ifdef T2_CORE
/* on zeitlos32g: the stream's register, read in place (sw/t2fw/io.c) */
static inline uint32_t t2io_in(void) { return *(volatile uint32_t *)0x20000u; }
#else
uint32_t t2io_in(void);                                   /* the next stream word */
#endif
volatile uint32_t *t2io_rec(int n);                      /* room for a record; 0 if full */
void t2io_frame(int32_t x0, int32_t x1, int32_t y0, int32_t y1, uint8_t clear);
void t2io_end(void);                                      /* raster the frame */

/* the geometry unit: carries out one command; 0, or -1 for a bad one */
int t2fw_step(void);

/* what the model counts of a frame, and what it estimates the engine's
 * cycles to be (tests/t2_model.c) */
typedef struct {
	uint32_t draws, verts, tris, cpu_tris, lines;
	uint32_t culled, clipped, drawn, pixels, overflows;
	uint32_t outside;                    /* of culled: wholly outside a plane */
	uint32_t guard;                      /* outside the sides, drawn unclipped */
	uint32_t cyc_fetch, cyc_geom, cyc_raster, cyc_out;
	int32_t rx0, rx1, ry0, ry1;          /* the frame's rectangle */
} t2_stats_t;

/* the model: carries out a frame's stream of n words into pic
 * (chunky indices); 0, or -1 for a bad stream */
int t2_model_run(const uint32_t *w, uint32_t n, uint8_t pic[240][320], t2_stats_t *st);
/* the model's texture memory: a word (four texels, the first in bits
 * 7:0) at word `word`, as the CPU writes T2's TEXDATA (tests/t2_raster.c) */
void t2_model_tex(uint32_t word, uint32_t v);
uint32_t t2_model_tex_word(uint32_t word);
uint32_t t2_model_used(void);

#include <stdbool.h>

#endif
