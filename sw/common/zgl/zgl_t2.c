/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zgl_t2.c -- zgl's side of T2, the triangle engine that runs the
 * whole frame (claude/zgl-t2-proposal.md; the stream is zgl_t2.h).
 *
 * In T2's frames (ZGL_CHUNKY on a bitstream whose engine says "ZTR2"),
 * the CPU streams the frame into the engine's FIFO as the program makes
 * its calls:
 *
 *   - a display list is compiled once, the first time it is called,
 *     into the stream's own chunks (indexed triangles, 16-bit
 *     positions); each call then sends the state it needs (matrix,
 *     light, shading) and copies the chunks' words, with no work a
 *     vertex;
 *   - anything else (immediate mode, lines, a list that can't be
 *     compiled) goes through zgl's front end as before, and each
 *     triangle or line it makes is sent set up (TRI, LINE);
 *   - zgl_frame() ends the frame (END).
 *
 * Phase A: the engine is its C model (tests/t2_model.c), in the
 * simulator (ZSIM_GTRI=2) or in the host tests. A frame with more
 * triangles than the engine holds is not sent again yet (the model has
 * no limit); that comes with the RTL.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "zgl.h"
#include "zgl_int.h"
#include "zgl_t2.h"

#define MAT_SH 20

static bool t2_on;
static bool t2_framed;               /* FRAME sent for this frame */
static volatile uint32_t *t2_reg;    /* the FIFO; else zglc_put() */
static zgl_info_t *t2_info;
static uint32_t t2_prims;            /* this frame's, so far */

/* what the engine was last sent, so only changes are sent again */
static uint32_t sent_view[2], sent_xform[19], sent_light[11], sent_state, sent_pl[2][8];
static bool sent_view_ok, sent_xform_ok, sent_light_ok, sent_state_ok, sent_pl_ok[2];

/* -- textures: T2's texture memory, filled from the bottom ----------------- */

#define TEX_NAMES 64
static uint32_t tex_wd[TEX_NAMES];   /* each name's texture word; 0 none */
static uint32_t tex_size[TEX_NAMES]; /* ...and its texels */
static uint32_t tex_top;             /* texels used */
static uint32_t tex_n;               /* textures made */

uint32_t zglTexRoom(void) {
	return zglc_tex_room ? zglc_tex_room() : 0;
}

bool zglTexImage(unsigned name, int lw, int lh, const uint8_t *texels, int mode, bool key) {
	uint32_t room = zglTexRoom(), n, base, i;
	if (!room || !zglc_tex_put || name == 0 || name >= TEX_NAMES) return false;
	if (lw < 0 || lw > 7 || lh < 0 || lh > 7 || mode < 0 || mode > 3) return false;
	n = 1u << (lw + lh);
	if (tex_wd[name] && tex_size[name] == n)
		base = T2_TEX_BASE(tex_wd[name]);    /* where it was */
	else {
		/* a word's four texels together: each texture whole words */
		base = (tex_top + 3u) & ~3u;
		if (base + n > room) return false;
		tex_top = base + n;
	}
	for (i = 0; i < n; i += 4) {
		uint32_t w = (uint32_t)texels[i];
		if (i + 1 < n) w |= (uint32_t)texels[i + 1] << 8;
		if (i + 2 < n) w |= (uint32_t)texels[i + 2] << 16;
		if (i + 3 < n) w |= (uint32_t)texels[i + 3] << 24;
		zglc_tex_put((base + i) / 4u, w);
	}
	if (!tex_wd[name]) tex_n++;
	tex_wd[name] = T2_TEX(base, lw, lh, mode, key);
	tex_size[name] = n;
	return true;
}

uint32_t zgl_t2_tex_word(uint32_t name) {
	return name < TEX_NAMES ? tex_wd[name] : 0;
}

static inline void put(uint32_t w) {
	if (t2_reg) *t2_reg = w;
	else zglc_put(w);
}

bool zgl_t2_open(zgl_info_t *info) {
	t2_on = true;
	t2_framed = false;
	t2_info = info;
	t2_reg = zglc_cmd ? zglc_cmd() : NULL;
	sent_view_ok = sent_xform_ok = sent_light_ok = sent_state_ok = false;
	sent_pl_ok[0] = sent_pl_ok[1] = false;
	return true;
}

void zgl_t2_close(void) {
	t2_on = false;
}

bool zgl_t2_active(void) {
	return t2_on;
}

static void frame_start(void) {
	int32_t x0, y0, x1, y1;
	if (t2_framed) return;
	zglf_rect(&x0, &y0, &x1, &y1);
	if (zglc_picture) zglc_picture(x0, y0, x1, y1);
	put(T2_FRAME << 24 | zgl_s.clear_index);
	put((uint32_t)x0 | (uint32_t)x1 << 16);
	put((uint32_t)y0 | (uint32_t)y1 << 16);
	t2_framed = true;
	/* a new frame: the engine's state is sent again */
	sent_view_ok = sent_xform_ok = sent_light_ok = sent_state_ok = false;
}

/* -- triangles and lines set up by the CPU ---------------------------------- */

static void put_s(const zgl_svtx_t *v) {
	put((uint32_t)v->x); put((uint32_t)v->y); put((uint32_t)v->z); put((uint32_t)v->ci);
}

void zgl_t2_tri(const zgl_svtx_t *a, const zgl_svtx_t *b, const zgl_svtx_t *c) {
	frame_start();
	put(T2_TRI << 24 | (zgl_s.depth_test ? 1u : 0u));
	put_s(a); put_s(b); put_s(c);
	t2_prims++;
}

void zgl_t2_line(const zgl_svtx_t *a, const zgl_svtx_t *b) {
	frame_start();
	put(T2_LINE << 24 | (zgl_s.depth_test ? 1u : 0u));
	put_s(a); put_s(b);
	t2_prims++;
}

void zgl_t2_frame(void) {
	frame_start();
	put(T2_END << 24);
	t2_framed = false;
	t2_info->frames++;
	t2_info->prims = t2_prims;
	t2_prims = 0;
}

/* -- the state a list's chunks need ---------------------------------------- */

static void send_words(uint32_t op, const uint32_t *w, int n, uint32_t *sent, bool *ok) {
	int i;
	if (*ok && sent[0] == op && !memcmp(&sent[1], w, (size_t)n * 4)) return;
	put(op);
	for (i = 0; i < n; i++) put(w[i]);
	sent[0] = op;
	memcpy(&sent[1], w, (size_t)n * 4);
	*ok = true;
}

/* The light in the model's space costs a dozen 64-bit products a call,
 * and a frame of a world in cells calls a hundred lists under the same
 * matrix: kept, and used again until a matrix, a light or an enable
 * changes (zgl_s.lserial). */
static uint32_t lc_serial;
static uint32_t lc_words[8];
static bool lc_ok;

static bool light_cached(uint32_t *w) {
	int k;
	if (!lc_ok || lc_serial != zgl_s.lserial) return false;
	for (k = 0; k < 8; k++) w[k] = lc_words[k];
	return true;
}

static void light_keep(const uint32_t *w) {
	int k;
	for (k = 0; k < 8; k++) lc_words[k] = w[k];
	lc_serial = zgl_s.lserial;
	lc_ok = true;
}

/* A positional light in the model's space: the modelview M is s R plus
 * t (zgl_t2_call() checks), so P_model = M3^T (P_eye - t) / s^2; and s,
 * and the attenuation's factors (zgl_t2.h, PLIGHT) */
static void point_words(int l, uint32_t *w) {
	const int32_t *m = zgl_s.mv[zgl_s.mv_top];
	int64_t s2 = 0, d[3], q;
	int c, r;
	for (r = 0; r < 3; r++) s2 += (int64_t)m[r] * m[r];          /* 24.40 */
	q = s2 >> 20;                                                  /* s^2, 12.20 */
	if (q < 1) q = 1;
	for (r = 0; r < 3; r++) d[r] = (int64_t)zgl_s.light_pos[l][r] - m[12 + r];
	for (c = 0; c < 3; c++) {
		int64_t v = (m[c * 4] * d[0] + m[c * 4 + 1] * d[1] + m[c * 4 + 2] * d[2]) / q;
		w[c] = (uint32_t)(int32_t)(v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : v);
	}
	w[3] = zgl_isqrt64((uint64_t)s2) >> 4;                        /* s, 16.16 */
	w[4] = (uint32_t)zgl_s.light_kc[l];
	w[5] = (uint32_t)zgl_s.light_kl[l];
	w[6] = zgl_s.light_kq[l];
}

static void send_state(void) {
	uint32_t w[18], op;
	int k, l;
	int cull = 0;
	/* the viewport */
	w[0] = (uint32_t)(uint16_t)zgl_s.vp_x | (uint32_t)(uint16_t)zgl_s.vp_y << 16;
	w[1] = (uint32_t)zgl_s.vp_w | (uint32_t)zgl_s.vp_h << 16;
	if (!sent_view_ok || sent_view[0] != w[0] || sent_view[1] != w[1]) {
		put(T2_VIEW << 24); put(w[0]); put(w[1]);
		sent_view[0] = w[0]; sent_view[1] = w[1];
		sent_view_ok = true;
	}
	/* the matrix: object to clip */
	zgl_sync_mvp();
	for (k = 0; k < 16; k++) w[k] = (uint32_t)zgl_s.mvp[k];
	w[16] = (uint32_t)zgl_s.near_w;
	w[17] = (uint32_t)zgl_s.far_w;
	send_words(T2_XFORM << 24 | (zgl_s.persp ? 1u : 0u), w, 18, sent_xform, &sent_xform_ok);
	/* the light, in the model's space: n . (M^T L) is (M n) . L */
	op = T2_LIGHT << 24 | (zgl_s.lighting ? 1u : 0u) |
	     (zgl_s.light_on[0] ? 2u : 0u) | (zgl_s.light_on[1] ? 4u : 0u);
	/* the lights' 8 words: zero but for those on (no memset: a store a
	 * word is what it costs) */
	for (k = 0; k < 8; k++) w[k] = 0;
	if (zgl_s.lighting && light_cached(w)) {
		/* the same matrix and lights as the last call: the same words */
	} else if (zgl_s.lighting) {
		const int32_t *m = zgl_s.mv[zgl_s.mv_top];
		for (l = 0; l < 2; l++) {
			if (!zgl_s.light_on[l]) continue;
			for (k = 0; k < 3; k++)
				w[l * 4 + k] = (uint32_t)(int32_t)(((int64_t)m[k * 4 + 0] * zgl_s.light_dir[l][0] +
				                                    (int64_t)m[k * 4 + 1] * zgl_s.light_dir[l][1] +
				                                    (int64_t)m[k * 4 + 2] * zgl_s.light_dir[l][2]) >> MAT_SH);
			/* GL_NORMALIZE: the modelview is a rotation (or a mirror)
			 * times one scale (zgl_t2_call() checks), and the unit
			 * normal's light is the unit M^T L, the scale divided out */
			if (zgl_s.normalize) {
				int32_t u[3];
				u[0] = (int32_t)w[l * 4]; u[1] = (int32_t)w[l * 4 + 1]; u[2] = (int32_t)w[l * 4 + 2];
				zgl_unit3(u);
				w[l * 4] = (uint32_t)u[0]; w[l * 4 + 1] = (uint32_t)u[1]; w[l * 4 + 2] = (uint32_t)u[2];
			}
			w[l * 4 + 3] = (uint32_t)zgl_s.light_d[l];
		}
		light_keep(w);
	}
	w[8] = (uint32_t)zgl_s.mat_a;
	w[9] = (uint32_t)zgl_s.mat_d;
	send_words(op, w, 10, sent_light, &sent_light_ok);
	/* positional lights: where each is in the model's space */
	for (l = 0; l < 2; l++) {
		bool on = zgl_s.lighting && zgl_s.light_on[l] && zgl_s.light_pt[l];
		op = T2_PLIGHT << 24 | (uint32_t)l | (on ? 2u : 0u) | (on && zgl_s.normalize ? 4u : 0u);
		for (k = 0; k < 7; k++) w[k] = 0;
		if (on) point_words(l, w);
		send_words(op, w, 7, sent_pl[l], &sent_pl_ok[l]);
	}
	/* depth, shading, culling */
	if (zgl_s.cull)
		cull = zgl_s.cull_face == GL_BACK ? 1 : zgl_s.cull_face == GL_FRONT ? 2 : 3;
	op = T2_STATE << 24 | (zgl_s.depth_test ? 1u : 0u) | (zgl_s.smooth ? 2u : 0u) |
	     (uint32_t)cull << 2 | (zgl_s.front_ccw ? 16u : 0u) |
	     (zgl_s.texture && tex_n ? 32u : 0u);
	if (!sent_state_ok || sent_state != op) {
		put(op);
		sent_state = op;
		sent_state_ok = true;
	}
}

/* -- display lists: compiled once, copied every call ------------------------- */

typedef struct {
	uint32_t *w;               /* the chunks, back to back */
	uint32_t nw;
	uint16_t nchunks;
	/* what the list leaves set, as GL does after it is called */
	bool set_normal, set_index, set_mat;
	int32_t normal[3], mat_a, mat_d;
	uint8_t index;
	bool unit;                 /* compiled with its normals made unit vectors */
} t2_list_t;

/* a vertex as the list made it */
typedef struct { int32_t x, y, z, nx, ny, nz, ci, s, t; } cv_t;

/* the compiler's state */
#define CH_T 1024              /* triangles, lines a chunk at most */
#define CH_ROOM (5 + T2_CHUNK_V * 5 + CH_T * 2 + CH_T)

typedef struct {
	t2_list_t *l;
	uint32_t cap;
	uint32_t c0;               /* the open chunk's first word */
	uint32_t nv, nt, nl;
	int shift;
	uint8_t flags;
	uint32_t tex;              /* the open chunk's texture word; 0 none */
	cv_t cv[T2_CHUNK_V];
	int16_t hash[512];
	bool fail;
} comp_t;

static bool grow(comp_t *c, uint32_t n) {
	if (c->l->nw + n <= c->cap) return true;
	{
		uint32_t cap = c->cap ? c->cap * 2 : 1024;
		uint32_t *nw;
		while (cap < c->l->nw + n) cap *= 2;
		nw = realloc(c->l->w, cap * 4);
		if (!nw) { c->fail = true; return false; }
		c->l->w = nw;
		c->cap = cap;
	}
	return true;
}

/* chunk layout while open: 4 header words (5 textured), then room for
 * T2_CHUNK_V vertices (4 words, 5 textured), CH_T triangles (2) and
 * CH_T lines (1); closed, the triangles and lines are moved down to
 * follow what was used */
#define T_ROOM (5 + T2_CHUNK_V * 5)
#define L_ROOM (T_ROOM + CH_T * 2)
#define HW(c)  ((c)->tex ? 5u : 4u)        /* header words */
#define VW(c)  ((c)->tex ? 5u : 4u)        /* words a vertex */

static void chunk_finish(comp_t *c) {
	uint32_t *h, vend;
	if (c->fail || c->nv == 0) { if (!c->fail) c->l->nw = c->c0; return; }
	h = &c->l->w[c->c0];
	vend = HW(c) + c->nv * VW(c);
	memmove(&h[vend], &h[T_ROOM], c->nt * 8);
	memmove(&h[vend + c->nt * 2], &h[L_ROOM], c->nl * 4);
	h[0] = c->nv | c->nt << 16;
	h[1] |= c->nl << 16;
	c->l->nw = c->c0 + vend + c->nt * 2 + c->nl;
	c->l->nchunks++;
}

static void chunk_open(comp_t *c, bool own_mat, int32_t ma, int32_t md) {
	uint32_t *h;
	if (!grow(c, CH_ROOM)) return;
	c->c0 = c->l->nw;
	h = &c->l->w[c->c0];
	h[0] = 0;
	h[1] = (uint32_t)c->shift | (uint32_t)(c->flags | (own_mat ? T2_C_MAT : 0) |
	       (c->tex ? T2_C_TEX : 0)) << 8;
	h[2] = (uint32_t)ma;
	h[3] = (uint32_t)md;
	h[4] = c->tex;
	c->l->nw = c->c0 + CH_ROOM;
	c->nv = c->nt = c->nl = 0;
	memset(c->hash, -1, sizeof(c->hash));
}

/* a vertex's number in the open chunk (added if new); -1 if full */
static int chunk_vertex(comp_t *c, const cv_t *v) {
	uint32_t hsh = ((uint32_t)v->x * 0x9E3779B1u ^ (uint32_t)v->y * 0x85EBCA77u ^
	                (uint32_t)v->z * 0xC2B2AE3Du ^ (uint32_t)v->nx * 31u ^
	                (uint32_t)v->ny * 131u ^ (uint32_t)v->nz * 1031u ^ (uint32_t)v->ci ^
	                (uint32_t)v->s * 7919u ^ (uint32_t)v->t * 104729u) >> 23;
	uint32_t *p;
	int k;
	for (k = 0; k < 512; k++) {
		int16_t e = c->hash[(hsh + (uint32_t)k) & 511];
		if (e < 0) break;
		if (!memcmp(&c->cv[e], v, sizeof(*v))) return e;
	}
	if (c->nv >= T2_CHUNK_V) return -1;
	c->hash[(hsh + (uint32_t)k) & 511] = (int16_t)c->nv;
	c->cv[c->nv] = *v;
	p = &c->l->w[c->c0 + HW(c) + c->nv * VW(c)];
	{
		int32_t r = c->shift ? 1 << (c->shift - 1) : 0;
		p[0] = (uint32_t)(uint16_t)((v->x + r) >> c->shift) | (uint32_t)(uint16_t)((v->y + r) >> c->shift) << 16;
		p[1] = (uint32_t)(uint16_t)((v->z + r) >> c->shift) | (uint32_t)(uint16_t)v->ci << 16;
		p[2] = (uint32_t)(uint16_t)((v->nx + 2) >> 2) | (uint32_t)(uint16_t)((v->ny + 2) >> 2) << 16;
		p[3] = (uint32_t)(uint16_t)((v->nz + 2) >> 2);
		if (c->tex) p[4] = (uint32_t)(uint16_t)v->s | (uint32_t)(uint16_t)v->t << 16;
	}
	return (int)c->nv++;
}

static bool cur_own_mat;
static int32_t cur_ma, cur_md;

/* a triangle of three corners, the provoking one's colour for flat */
static void comp_tri(comp_t *c, const cv_t *a, const cv_t *b, const cv_t *d, const cv_t *pv) {
	int ia, ib, id, ip;
	uint32_t *t;
	if (c->fail) return;
	if (c->nv + 4 > T2_CHUNK_V || c->nt >= CH_T) {
		chunk_finish(c);
		chunk_open(c, cur_own_mat, cur_ma, cur_md);
		if (c->fail) return;
	}
	ia = chunk_vertex(c, a); ib = chunk_vertex(c, b);
	id = chunk_vertex(c, d); ip = chunk_vertex(c, pv);
	if (ia < 0 || ib < 0 || id < 0 || ip < 0) { c->fail = true; return; }
	t = &c->l->w[c->c0 + T_ROOM + c->nt * 2];
	t[0] = (uint32_t)ia | (uint32_t)ib << 16;
	t[1] = (uint32_t)id | (uint32_t)ip << 16;
	c->nt++;
}

/* a line, from a to b */
static void comp_line(comp_t *c, const cv_t *a, const cv_t *b) {
	int ia, ib;
	if (c->fail) return;
	if (c->nv + 2 > T2_CHUNK_V || c->nl >= CH_T) {
		chunk_finish(c);
		chunk_open(c, cur_own_mat, cur_ma, cur_md);
		if (c->fail) return;
	}
	ia = chunk_vertex(c, a); ib = chunk_vertex(c, b);
	if (ia < 0 || ib < 0) { c->fail = true; return; }
	c->l->w[c->c0 + L_ROOM + c->nl] = (uint32_t)ia | (uint32_t)ib << 16;
	c->nl++;
}

/* a convex polygon's fan, as zgl_polygon() and fill_polygon() make it */
static void comp_poly(comp_t *c, const cv_t *v, int n) {
	int i;
	for (i = 2; i < n; i++) comp_tri(c, &v[0], &v[i - 1], &v[i], &v[n - 1]);
}

static t2_list_t *compile(const zglr_op_t *ops, uint32_t n, bool unit) {
	static comp_t c;           /* large: not on the stack */
	t2_list_t *l;
	uint32_t i;
	int32_t mx = 0;
	bool has_n_op = false, has_i_op = false, seen_n = false, seen_i = false;
	bool in_begin = false;
	GLenum prim = 0;
	cv_t q[ZGL_POLY_MAX], cur;
	int nq = 0;
	unsigned k = 0;
	/* first: what the list does, and how far its positions reach */
	for (i = 0; i < n; i++) {
		const zglr_op_t *o = &ops[i];
		switch (o->op) {
		case ZGLR_BEGIN:
			if (o->a != GL_TRIANGLES && o->a != GL_TRIANGLE_STRIP && o->a != GL_TRIANGLE_FAN &&
			    o->a != GL_QUADS && o->a != GL_QUAD_STRIP && o->a != GL_POLYGON &&
			    o->a != GL_LINES && o->a != GL_LINE_STRIP && o->a != GL_LINE_LOOP)
				return NULL;
			break;
		case ZGLR_END: case ZGLR_MATERIAL: break;
		case ZGLR_NORMAL:
			has_n_op = true;
			if (!unit && (o->a > 131070 || o->a < -131070 || o->b > 131070 || o->b < -131070 ||
			    o->c > 131070 || o->c < -131070))
				return NULL;
			break;
		case ZGLR_INDEX: has_i_op = true; break;
		case ZGLR_TEXCOORD: break;
		case ZGLR_BIND:
			/* a texture coordinate past what a vertex holds (12.4 in 16
			 * bits) would wrap: played back, in colours */
			break;
		case ZGLR_VERTEX: {
			int32_t a = o->a < 0 ? -o->a : o->a, b = o->b < 0 ? -o->b : o->b, d = o->c < 0 ? -o->c : o->c;
			if (a > mx) mx = a;
			if (b > mx) mx = b;
			if (d > mx) mx = d;
			break;
		}
		default: return NULL;                      /* matrices, state, calls: played back */
		}
	}
	/* a vertex before the list's first normal (or index) would take the
	 * caller's, which a chunk can't mix with its own: played back */
	for (i = 0; i < n; i++) {
		if (ops[i].op == ZGLR_NORMAL) seen_n = true;
		if (ops[i].op == ZGLR_INDEX) seen_i = true;
		if (ops[i].op == ZGLR_VERTEX && ((has_n_op && !seen_n) || (has_i_op && !seen_i))) return NULL;
	}
	l = calloc(1, sizeof(*l));
	if (!l) return NULL;
	l->unit = unit;
	memset(&c, 0, sizeof(c));
	c.l = l;
	c.shift = 0;
	while ((mx + (c.shift ? 1 << (c.shift - 1) : 0)) >> c.shift > 32767) c.shift++;
	c.flags = (uint8_t)((has_n_op ? T2_C_NORMAL : 0) | (has_i_op ? T2_C_INDEX : 0));
	cur_own_mat = false; cur_ma = 0; cur_md = 0;
	memset(&cur, 0, sizeof(cur));
	/* the texture bound when the list is made */
	c.tex = zgl_s.tex_bound ? zgl_t2_tex_word(zgl_s.tex_bound) : 0;
	chunk_open(&c, false, 0, 0);
	for (i = 0; i < n && !c.fail; i++) {
		const zglr_op_t *o = &ops[i];
		switch (o->op) {
		case ZGLR_BEGIN: in_begin = true; prim = (GLenum)o->a; nq = 0; k = 0; break;
		case ZGLR_END:
			if (in_begin && prim == GL_POLYGON && nq >= 3) comp_poly(&c, q, nq);
			/* zgl.c's glEnd(): the loop closed from the last to the first */
			if (in_begin && prim == GL_LINE_LOOP && k >= 2) comp_line(&c, &q[1], &q[0]);
			in_begin = false;
			break;
		case ZGLR_NORMAL:
			cur.nx = o->a; cur.ny = o->b; cur.nz = o->c;
			if (unit) {
				int32_t u[3];
				u[0] = o->a; u[1] = o->b; u[2] = o->c;
				zgl_unit3(u);
				cur.nx = u[0]; cur.ny = u[1]; cur.nz = u[2];
			}
			l->set_normal = true;
			l->normal[0] = o->a; l->normal[1] = o->b; l->normal[2] = o->c;
			break;
		case ZGLR_INDEX:
			cur.ci = (o->a & 0xff) << 8;
			l->set_index = true;
			l->index = (uint8_t)o->a;
			break;
		case ZGLR_TEXCOORD:
			cur.s = o->a < -32768 ? -32768 : o->a > 32767 ? 32767 : o->a;
			cur.t = o->b < -32768 ? -32768 : o->b > 32767 ? 32767 : o->b;
			break;
		case ZGLR_BIND: {
			uint32_t tw = o->a ? zgl_t2_tex_word((uint32_t)o->a) : 0;
			if (in_begin) { c.fail = true; break; }
			if (tw == c.tex) break;
			chunk_finish(&c);
			c.tex = tw;
			chunk_open(&c, cur_own_mat, cur_ma, cur_md);
			break;
		}
		case ZGLR_MATERIAL:
			if (in_begin) { c.fail = true; break; }
			chunk_finish(&c);
			cur_own_mat = true; cur_ma = o->a; cur_md = o->b;
			chunk_open(&c, true, o->a, o->b);
			l->set_mat = true; l->mat_a = o->a; l->mat_d = o->b;
			break;
		case ZGLR_VERTEX: {
			cv_t v = cur;
			if (!in_begin) break;
			v.x = o->a; v.y = o->b; v.z = o->c;
			/* zgl.c's prim_corner(), with corners by value */
			switch (prim) {
			case GL_TRIANGLES:
				q[k % 3] = v;
				if (k % 3 == 2) comp_poly(&c, q, 3);
				break;
			case GL_TRIANGLE_STRIP:
				if (k >= 2) {
					if (k & 1) comp_tri(&c, &q[1], &q[0], &v, &v);
					else comp_tri(&c, &q[0], &q[1], &v, &v);
					q[0] = q[1]; q[1] = v;
				} else q[k] = v;
				break;
			case GL_TRIANGLE_FAN:
				if (k >= 2) { comp_tri(&c, &q[0], &q[1], &v, &v); q[1] = v; }
				else q[k] = v;
				break;
			case GL_QUADS:
				q[k & 3] = v;
				if ((k & 3) == 3) comp_poly(&c, q, 4);
				break;
			case GL_QUAD_STRIP:
				if (k >= 3 && (k & 1)) {
					cv_t t4[4];
					t4[0] = q[0]; t4[1] = q[1]; t4[2] = v; t4[3] = q[2];
					comp_poly(&c, t4, 4);
					q[0] = q[2]; q[1] = v;
				} else if (k < 3) q[k] = v;
				else q[2] = v;
				break;
			case GL_LINES:
				if (k & 1) comp_line(&c, &q[0], &v); else q[0] = v;
				break;
			case GL_LINE_STRIP:
			case GL_LINE_LOOP:
				if (k == 0) { q[0] = v; q[1] = v; }
				else { comp_line(&c, &q[1], &v); q[1] = v; }
				break;
			default:                               /* GL_POLYGON */
				if (nq < ZGL_POLY_MAX) q[nq++] = v;
				break;
			}
			k++;
			break;
		}
		default: break;
		}
	}
	chunk_finish(&c);
	if (c.fail) { free(l->w); free(l); return NULL; }
	return l;
}

void zgl_t2_free(void *p) {
	t2_list_t *l = p;
	if (!l) return;
	free(l->w);
	free(l);
}

/* GL_NORMALIZE on T2: the light goes to the model's space as M^T L made
 * a unit vector, which is the software's unit (M n) . L only when M's
 * 3x3 is a rotation or mirror times one scale -- its columns as long as
 * each other and at right angles, to a 64th. Otherwise played back. */
static bool similarity(void) {
	const int32_t *m = zgl_s.mv[zgl_s.mv_top];
	int64_t d[3][3];
	int a, b, k;
	for (a = 0; a < 3; a++)
		for (b = a; b < 3; b++) {
			int64_t t = 0;
			for (k = 0; k < 3; k++) t += ((int64_t)m[a * 4 + k] >> 4) * ((int64_t)m[b * 4 + k] >> 4);
			d[a][b] = t;
		}
	for (a = 0; a < 3; a++) {
		int64_t q = d[0][0] >> 6;
		if (d[0][0] <= 0) return false;
		if (d[a][a] - d[0][0] > q || d[0][0] - d[a][a] > q) return false;
		for (b = a + 1; b < 3; b++)
			if (d[a][b] > q || -d[a][b] > q) return false;
	}
	return true;
}

/* glCallList()'s way: true if the list went to the engine as chunks
 * (*cache: its compiled form; *state: 0 not tried, 1 compiled, 2 can't) */
bool zgl_t2_call(const zglr_op_t *ops, uint32_t n, void **cache, uint8_t *state) {
	t2_list_t *l;
	uint32_t i, ci;
	const uint32_t *w;
	int32_t dn[3];
	bool unit = zgl_s.lighting && zgl_s.normalize;
	if (!t2_on || zgl_s.in_begin || zgl_s.listing) return false;
	if (zgl_s.poly_front != GL_FILL || zgl_s.poly_back != GL_FILL) return false;
	if (unit && !similarity()) return false;
	/* a positional light goes to the model's space the same way */
	if (zgl_s.lighting && ((zgl_s.light_on[0] && zgl_s.light_pt[0]) ||
	    (zgl_s.light_on[1] && zgl_s.light_pt[1])) && !unit && !similarity()) return false;
	/* compiled the other way (normalizing then, not now): again */
	if (*state == 1 && ((t2_list_t *)*cache)->unit != unit) {
		zgl_t2_free(*cache);
		*cache = NULL;
		*state = 0;
	}
	if (*state == 0) {
		*cache = compile(ops, n, unit);
		*state = *cache ? 1 : 2;
	}
	if (*state != 1) return false;
	l = *cache;
	dn[0] = zgl_s.normal[0]; dn[1] = zgl_s.normal[1]; dn[2] = zgl_s.normal[2];
	if (unit) zgl_unit3(dn);
	frame_start();
	send_state();
	w = l->w;
	for (ci = 0; ci < l->nchunks; ci++) {
		uint32_t nv = w[0] & 0xffff, nt = w[0] >> 16, nl = w[1] >> 16;
		uint32_t tx = (w[1] >> 8) & T2_C_TEX ? 1u : 0u;
		uint32_t nw = 4 + tx + nv * (4 + tx) + nt * 2 + nl;
		put(T2_DRAW << 24);
		put((uint32_t)zgl_s.index << 8);
		put((uint32_t)dn[0]); put((uint32_t)dn[1]); put((uint32_t)dn[2]);
		/* the chunk, as it is: a load and a store a word */
		if (t2_reg) {
			volatile uint32_t *r = t2_reg;
			for (i = 0; i + 4 <= nw; i += 4) {
				uint32_t a = w[i], b = w[i + 1], c = w[i + 2], d = w[i + 3];
				*r = a; *r = b; *r = c; *r = d;
			}
			for (; i < nw; i++) *r = w[i];
		} else
			for (i = 0; i < nw; i++) zglc_put(w[i]);
		t2_prims += nt + nl;
		w += nw;
	}
	/* what the list leaves set */
	if (l->set_normal) {
		zgl_s.normal[0] = l->normal[0]; zgl_s.normal[1] = l->normal[1]; zgl_s.normal[2] = l->normal[2];
	}
	if (l->set_index) zgl_s.index = l->index;
	if (l->set_mat) zgl_material(l->mat_a, l->mat_d);
	zgl_s.gen++;
	return true;
}
