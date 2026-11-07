/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zgl.c -- zgl's front end: GL state, matrices, vertices, primitives,
 * clipping and projection. docs/zgl.md. The back ends turn what comes
 * out (lines and points in picture coordinates) into pixels:
 * zgl_lines.c for ZGL_LINES.
 *
 * -- Numbers --
 *
 * Matrices are 16.16 (GLfixed). Positions are 24.8: GLfixed's 16
 * integer bits are not enough for a game world (space3d's galaxies
 * are 40,000 units away), so a vertex is kept with 24 integer bits and
 * 8 of fraction, which a unit cube still resolves to 1/256. A matrix
 * times a position is a 40.24 product summed in 64 bits and shifted
 * back to 24.8: on RV32IM a mul and a mulh each, no library call.
 *
 * -- The pipeline --
 *
 * glVertex: object -> clip coordinates through the combined
 * projection x modelview (made once when either changes), with the
 * colour index of the moment. Primitives are assembled as GL says
 * (strips, fans, loops, quads, polygons), lines are clipped in clip
 * space against the six planes of the view volume (so a line passing
 * behind the eye never folds onto the screen), divided by w and
 * mapped to the viewport, then handed to the back end in picture
 * coordinates (0,0 the top-left of the 320x240 picture, y down).
 * Polygons in ZGL_LINES are their outlines, culled first if asked.
 */

/* zgl is arithmetic in loops: -O2 whatever the program is built with
 * (apps are -Os); about a third fewer instructions a line */
#pragma GCC optimize ("O2")

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "zgl.h"
#include "zgl_int.h"

zgl_state_t zgl_s;

#define FIX_ONE   65536
#define POS_ONE   256
#define MAT_ONE   (1 << 20)      /* matrices' ratios are 12.20 */
#define MAT_SH    20

/* -- fixed point ----------------------------------------------------------- */

static inline int32_t fmul(int32_t a, int32_t b) {
	return (int32_t)(((int64_t)a * b) >> 16);
}

static inline int32_t fdiv(int32_t a, int32_t b) {
	if (b == 0) return a >= 0 ? INT32_MAX : INT32_MIN;
	return (int32_t)(((int64_t)a * 65536) / b);
}

/* a quarter wave of sine, 64 steps, 16.16 */
static const int32_t sin_q[65] = {
	0, 1608, 3216, 4821, 6424, 8022, 9616, 11204,
	12785, 14359, 15924, 17479, 19024, 20557, 22078, 23586,
	25080, 26558, 28020, 29466, 30893, 32303, 33692, 35062,
	36410, 37736, 39040, 40320, 41576, 42806, 44011, 45190,
	46341, 47464, 48559, 49624, 50660, 51665, 52639, 53581,
	54491, 55368, 56212, 57022, 57798, 58538, 59244, 59914,
	60547, 61145, 61705, 62228, 62714, 63162, 63572, 63944,
	64277, 64571, 64827, 65043, 65220, 65358, 65457, 65516,
	65536
};

GLfixed zglSin(uint32_t t) {
	uint32_t q = (t >> 14) & 3, i = t & 0x3fff;   /* quadrant, position in it */
	uint32_t k, f;
	int32_t v;
	if (q & 1) i = 0x4000 - i;
	k = i >> 8;                                   /* 0..64 */
	f = i & 0xff;
	v = (k >= 64) ? sin_q[64] :
		sin_q[k] + (int32_t)(((int64_t)(sin_q[k + 1] - sin_q[k]) * f) >> 8);
	return (q & 2) ? -v : v;
}

GLfixed zglCos(uint32_t t) {
	return zglSin(t + 0x4000);
}

uint32_t zgl_isqrt64(uint64_t v);
#define isqrt64 zgl_isqrt64
uint32_t zgl_isqrt64(uint64_t v) {
	uint64_t r = 0, b = (uint64_t)1 << 62;
	while (b > v) b >>= 2;
	while (b) {
		if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
		else r >>= 1;
		b >>= 2;
	}
	return (uint32_t)r;
}

static inline int32_t from_float(float f) {
	return (int32_t)(f * 65536.0f);
}

/* -- errors ------------------------------------------------------------------- */

static void set_error(GLenum e) {
	if (zgl_s.error == GL_NO_ERROR) zgl_s.error = e;
}

GLenum glGetError(void) {
	GLenum e = zgl_s.error;
	zgl_s.error = GL_NO_ERROR;
	return e;
}

const GLubyte *glGetString(GLenum name) {
	switch (name) {
	case GL_VENDOR: return (const GLubyte *)"Zeitlos";
	case GL_RENDERER: return (const GLubyte *)"zgl";
	case GL_VERSION: return (const GLubyte *)"1.0 zgl";
	case GL_EXTENSIONS: return (const GLubyte *)"";
	default: set_error(GL_INVALID_ENUM); return NULL;
	}
}

/* -- matrices -------------------------------------------------------------------
 *
 * Column-major, as GL: m[col * 4 + row]. Columns 0-2 are 12.20
 * ratios (16.16 left glOrtho(0, 320, ...)'s 2/320 a pixel out at the
 * far side: 409.6 in 16.16); column 3 -- what multiplies a point's w of 1: the
 * translation, and the w row's constant -- is 24.8, the scale of a
 * position, so a translation reaches +-8,388,607 units. Every product
 * keeps that: a term through column 3 of the left matrix is scaled up
 * by 2^12 to meet the others (a position times a ratio, against a ratio
 * times a ratio). A vertex times a matrix is then 24.8 throughout. */

static void mat_identity(int32_t *m) {
	memset(m, 0, 16 * sizeof(int32_t));
	m[0] = m[5] = m[10] = MAT_ONE;
	m[15] = POS_ONE;
}

/* r = a * b (r may be a or b) */
static void mat_mul(int32_t *r, const int32_t *a, const int32_t *b) {
	int32_t t[16];
	int c, rr, k;
	for (c = 0; c < 4; c++)
		for (rr = 0; rr < 4; rr++) {
			int64_t s = 0;
			for (k = 0; k < 3; k++)
				s += (int64_t)a[k * 4 + rr] * b[c * 4 + k];
			s += ((int64_t)a[12 + rr] * b[c * 4 + 3]) * (MAT_ONE / POS_ONE);
			t[c * 4 + rr] = (int32_t)(s >> MAT_SH);
		}
	memcpy(r, t, sizeof(t));
}

/* GL's 16.16 matrix (every element 16.16) to ours */
static void mat_import(int32_t *o, const int32_t *m) {
	int i;
	for (i = 0; i < 12; i++) o[i] = m[i] * (MAT_ONE / FIX_ONE);
	for (i = 12; i < 16; i++) o[i] = m[i] >> 8;
}

static int32_t *cur_mat(void) {
	if (zgl_s.matrix_mode == GL_PROJECTION)
		return zgl_s.proj[zgl_s.proj_top];
	return zgl_s.mv[zgl_s.mv_top];
}

static void mat_changed(void) {
	zgl_s.mvp_dirty = true;
	zgl_s.gen++;
	zgl_s.lserial++;
}

static void mult_cur(const int32_t *m) {
	int32_t *c = cur_mat();
	mat_mul(c, c, m);
	if (zgl_s.matrix_mode == GL_PROJECTION) zgl_s.p_exact[zgl_s.proj_top] = false;
	mat_changed();
}

static bool is_identity(const int32_t *m) {
	int i;
	for (i = 0; i < 16; i++)
		if (m[i] != ((i == 15) ? POS_ONE : (i % 5 == 0) ? MAT_ONE : 0)) return false;
	return true;
}

void glMatrixMode(GLenum mode) {
	if (mode != GL_MODELVIEW && mode != GL_PROJECTION) { set_error(GL_INVALID_ENUM); return; }
	zgl_s.matrix_mode = mode;
}

void glLoadIdentity(void) {
	ZGL_REC(ZGLR_IDENTITY, 0, 0, 0, 0);
	mat_identity(cur_mat());
	if (zgl_s.matrix_mode == GL_PROJECTION) zgl_s.p_exact[zgl_s.proj_top] = false;
	mat_changed();
}

void glPushMatrix(void) {
	ZGL_REC(ZGLR_PUSH, 0, 0, 0, 0);
	if (zgl_s.matrix_mode == GL_PROJECTION) {
		if (zgl_s.proj_top + 1 >= ZGL_PROJ_STACK) { set_error(GL_STACK_OVERFLOW); return; }
		memcpy(zgl_s.proj[zgl_s.proj_top + 1], zgl_s.proj[zgl_s.proj_top], 16 * sizeof(int32_t));
		zgl_s.p_exact[zgl_s.proj_top + 1] = zgl_s.p_exact[zgl_s.proj_top];
		zgl_s.p_near[zgl_s.proj_top + 1] = zgl_s.p_near[zgl_s.proj_top];
		zgl_s.p_far[zgl_s.proj_top + 1] = zgl_s.p_far[zgl_s.proj_top];
		zgl_s.proj_top++;
	} else {
		if (zgl_s.mv_top + 1 >= ZGL_MV_STACK) { set_error(GL_STACK_OVERFLOW); return; }
		memcpy(zgl_s.mv[zgl_s.mv_top + 1], zgl_s.mv[zgl_s.mv_top], 16 * sizeof(int32_t));
		zgl_s.mv_top++;
	}
}

void glPopMatrix(void) {
	ZGL_REC(ZGLR_POP, 0, 0, 0, 0);
	if (zgl_s.matrix_mode == GL_PROJECTION) {
		if (zgl_s.proj_top == 0) { set_error(GL_STACK_UNDERFLOW); return; }
		zgl_s.proj_top--;
	} else {
		if (zgl_s.mv_top == 0) { set_error(GL_STACK_UNDERFLOW); return; }
		zgl_s.mv_top--;
	}
	mat_changed();
}

void glLoadMatrixx(const GLfixed *m) {
	mat_import(cur_mat(), m);
	if (zgl_s.matrix_mode == GL_PROJECTION) zgl_s.p_exact[zgl_s.proj_top] = false;
	mat_changed();
}

void glMultMatrixx(const GLfixed *m) {
	int32_t t[16];
	mat_import(t, m);
	mult_cur(t);
}

static void mat_from_float(int32_t *t, const GLfloat *m) {
	int i;
	for (i = 0; i < 12; i++) t[i] = (int32_t)(m[i] * (float)MAT_ONE);
	for (i = 12; i < 16; i++) t[i] = (int32_t)(m[i] * (float)POS_ONE);
}

void glLoadMatrixf(const GLfloat *m) {
	mat_from_float(cur_mat(), m);
	if (zgl_s.matrix_mode == GL_PROJECTION) zgl_s.p_exact[zgl_s.proj_top] = false;
	mat_changed();
}

void glMultMatrixf(const GLfloat *m) {
	int32_t t[16];
	mat_from_float(t, m);
	mult_cur(t);
}

/* Translation by x, y, z in 24.8: M = M * T, so column 3 gains M's
 * first three columns times (x, y, z) -- ratios times positions,
 * positions, as column 3 is kept. */
static void translate_pos(int32_t x, int32_t y, int32_t z) {
	int32_t *c = cur_mat();
	int r;
	ZGL_REC(ZGLR_TRANSLATE, x, y, z, 0);
	for (r = 0; r < 4; r++) {
		int64_t s = (int64_t)c[0 * 4 + r] * x + (int64_t)c[1 * 4 + r] * y +
		            (int64_t)c[2 * 4 + r] * z;
		c[12 + r] += (int32_t)(s >> MAT_SH);
	}
	mat_changed();
}

void glTranslatex(GLfixed x, GLfixed y, GLfixed z) {
	translate_pos(x >> 8, y >> 8, z >> 8);
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z) {
	translate_pos((int32_t)(x * 256.0f), (int32_t)(y * 256.0f), (int32_t)(z * 256.0f));
}

void zglTranslatei(GLint x, GLint y, GLint z) {
	translate_pos(x * POS_ONE, y * POS_ONE, z * POS_ONE);
}

/* M = M * S: columns 0-2 scaled, in place (12 multiplies, not 64) */
void glScalex(GLfixed x, GLfixed y, GLfixed z) {
	int32_t *c = cur_mat();
	int r;
	ZGL_REC(ZGLR_SCALE, x, y, z, 0);
	for (r = 0; r < 4; r++) {
		c[r] = fmul(c[r], x);
		c[4 + r] = fmul(c[4 + r], y);
		c[8 + r] = fmul(c[8 + r], z);
	}
	if (zgl_s.matrix_mode == GL_PROJECTION) zgl_s.p_exact[zgl_s.proj_top] = false;
	mat_changed();
}

/* M = M * R for a rotation about one axis: two columns combined in
 * place (8 multiplies) -- the usual case, a model turned about x, y
 * or z. i and j are the columns the rotation mixes. */
static void rotate_axis(int i, int j, int32_t s, int32_t c) {
	int32_t *m = cur_mat();
	int r;
	for (r = 0; r < 4; r++) {
		int32_t a = m[i * 4 + r], b = m[j * 4 + r];
		m[i * 4 + r] = fmul(a, c) + fmul(b, s);
		m[j * 4 + r] = fmul(b, c) - fmul(a, s);
	}
	if (zgl_s.matrix_mode == GL_PROJECTION) zgl_s.p_exact[zgl_s.proj_top] = false;
	mat_changed();
}

void glScalef(GLfloat x, GLfloat y, GLfloat z) {
	glScalex(from_float(x), from_float(y), from_float(z));
}

/* degrees in 16.16; the axis need not be unit length */
void glRotatex(GLfixed deg, GLfixed x, GLfixed y, GLfixed z) {
	int32_t m[16];
	uint32_t t = (uint32_t)(((int64_t)deg * 65536 / 360) >> 16);
	int32_t s = zglSin(t), c = zglCos(t), ic = FIX_ONE - c;
	uint32_t len;
	ZGL_REC(ZGLR_ROTATE, deg, x, y, z);
	/* about one axis: column pairs (y,z) for x, (z,x) for y, (x,y) for z */
	if (y == 0 && z == 0 && x != 0) { rotate_axis(1, 2, x > 0 ? s : -s, c); return; }
	if (x == 0 && z == 0 && y != 0) { rotate_axis(2, 0, y > 0 ? s : -s, c); return; }
	if (x == 0 && y == 0 && z != 0) { rotate_axis(0, 1, z > 0 ? s : -s, c); return; }
	len = isqrt64((uint64_t)((int64_t)x * x + (int64_t)y * y + (int64_t)z * z));
	if (len == 0) return;
	x = fdiv(x, (int32_t)len);
	y = fdiv(y, (int32_t)len);
	z = fdiv(z, (int32_t)len);
	mat_identity(m);
	/* 16.16, then to the matrices' 12.20 */
	m[0] = fmul(fmul(x, x), ic) + c;
	m[1] = fmul(fmul(y, x), ic) + fmul(z, s);
	m[2] = fmul(fmul(x, z), ic) - fmul(y, s);
	m[4] = fmul(fmul(x, y), ic) - fmul(z, s);
	m[5] = fmul(fmul(y, y), ic) + c;
	m[6] = fmul(fmul(y, z), ic) + fmul(x, s);
	m[8] = fmul(fmul(x, z), ic) + fmul(y, s);
	m[9] = fmul(fmul(y, z), ic) - fmul(x, s);
	m[10] = fmul(fmul(z, z), ic) + c;
	{
		int i;
		for (i = 0; i < 11; i++) if (i != 3 && i != 7) m[i] *= MAT_ONE / FIX_ONE;
	}
	mult_cur(m);
}

void glRotatef(GLfloat deg, GLfloat x, GLfloat y, GLfloat z) {
	glRotatex(from_float(deg), from_float(x), from_float(y), from_float(z));
}

/* The projections, from 32.32 arguments so that a far plane past
 * GLfixed's range (space3d's 40,000) is fine: the matrix entries
 * themselves are ratios and fit 16.16. */
static int32_t ratio(int64_t num, int64_t den) {
	if (den == 0) return 0;
	/* the matrices' 12.20 from num and den in 16.16, rounded */
	int64_t q = num * MAT_ONE;
	if ((q < 0) != (den < 0)) return (int32_t)((q - den / 2) / den);
	return (int32_t)((q + den / 2) / den);
}

static void frustum64(int64_t l, int64_t r, int64_t b, int64_t t, int64_t n, int64_t f) {
	int32_t m[16];
	int64_t rl = (r - l) >> 16, tb = (t - b) >> 16, fn = (f - n) >> 16;
	int64_t n16 = n >> 16, f16 = f >> 16;
	bool exact;
	if (rl == 0 || tb == 0 || fn == 0 || n <= 0 || f <= 0) { set_error(GL_INVALID_VALUE); return; }
	/* on an identity projection (the usual case) near and far are kept
	 * as given, for clipping against w (make_mvp()) */
	exact = zgl_s.matrix_mode == GL_PROJECTION && is_identity(cur_mat());
	memset(m, 0, sizeof(m));
	m[0] = ratio(2 * n16, rl);
	m[5] = ratio(2 * n16, tb);
	m[8] = ratio((r + l) >> 16, rl);
	m[9] = ratio((t + b) >> 16, tb);
	m[10] = -ratio((f + n) >> 16, fn);
	m[11] = -MAT_ONE;
	/* -2fn/(f-n): a length, in column 3 as 24.8. f16, n16 and fn are
	 * 16.16, so 2fn is 32.32 and over fn 16.16 */
	m[14] = (int32_t)(-((2 * f16 * n16) / fn) >> 8);
	mult_cur(m);
	if (exact) {
		zgl_s.p_exact[zgl_s.proj_top] = true;
		zgl_s.p_near[zgl_s.proj_top] = (int32_t)(n >> 20);   /* 32.32 -> 20.12 */
		zgl_s.p_far[zgl_s.proj_top] = (f >> 20) > INT32_MAX ? INT32_MAX : (int32_t)(f >> 20);
	}
}

static void ortho64(int64_t l, int64_t r, int64_t b, int64_t t, int64_t n, int64_t f) {
	int32_t m[16];
	int64_t rl = (r - l) >> 16, tb = (t - b) >> 16, fn = (f - n) >> 16;
	if (rl == 0 || tb == 0 || fn == 0) { set_error(GL_INVALID_VALUE); return; }
	mat_identity(m);
	m[0] = ratio(2 << 16, rl);
	m[5] = ratio(2 << 16, tb);
	m[10] = -ratio(2 << 16, fn);
	/* -(r+l)/(r-l) and the rest are ratios too, but in the translation
	 * column, which xform() reads as 24.8: scale by 256 */
	m[12] = (int32_t)(-((int64_t)ratio((r + l) >> 16, rl) >> (MAT_SH - 8)));
	m[13] = (int32_t)(-((int64_t)ratio((t + b) >> 16, tb) >> (MAT_SH - 8)));
	m[14] = (int32_t)(-((int64_t)ratio((f + n) >> 16, fn) >> (MAT_SH - 8)));
	mult_cur(m);
}

#define D32(v) ((int64_t)((v) * 4294967296.0))

void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
	frustum64(D32(l), D32(r), D32(b), D32(t), D32(n), D32(f));
}

void glFrustumx(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f) {
	frustum64((int64_t)l * 65536, (int64_t)r * 65536, (int64_t)b * 65536,
		(int64_t)t * 65536, (int64_t)n * 65536, (int64_t)f * 65536);
}

void zglFrustumi(GLint l, GLint r, GLint b, GLint t, GLint n, GLint f) {
	frustum64((int64_t)l * 4294967296LL, (int64_t)r * 4294967296LL,
		(int64_t)b * 4294967296LL, (int64_t)t * 4294967296LL,
		(int64_t)n * 4294967296LL, (int64_t)f * 4294967296LL);
}

void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f) {
	ortho64(D32(l), D32(r), D32(b), D32(t), D32(n), D32(f));
}

void glOrthox(GLfixed l, GLfixed r, GLfixed b, GLfixed t, GLfixed n, GLfixed f) {
	ortho64((int64_t)l * 65536, (int64_t)r * 65536, (int64_t)b * 65536,
		(int64_t)t * 65536, (int64_t)n * 65536, (int64_t)f * 65536);
}

/* -- the viewport and state ------------------------------------------------------ */

void glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
	if (w < 0 || h < 0) { set_error(GL_INVALID_VALUE); return; }
	zgl_s.vp_x = x;
	zgl_s.vp_y = ZGL_H - (y + h);      /* GL counts from the bottom */
	zgl_s.vp_w = w;
	zgl_s.vp_h = h;
	zgl_s.gen++;
}

void glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
	if (w < 0 || h < 0) { set_error(GL_INVALID_VALUE); return; }
	zgl_s.sc_x0 = x;
	zgl_s.sc_y0 = ZGL_H - (y + h);
	zgl_s.sc_x1 = x + w - 1;
	zgl_s.sc_y1 = ZGL_H - y - 1;
}

static bool *cap(GLenum c) {
	switch (c) {
	case GL_CULL_FACE: return &zgl_s.cull;
	case GL_DEPTH_TEST: return &zgl_s.depth_test;
	case GL_SCISSOR_TEST: return &zgl_s.scissor;
	case GL_LIGHTING: return &zgl_s.lighting;
	case GL_NORMALIZE: return &zgl_s.normalize;
	case GL_LIGHT0: return &zgl_s.light_on[0];
	case GL_LIGHT1: return &zgl_s.light_on[1];
	case GL_TEXTURE_2D: return &zgl_s.texture;
	default: return NULL;
	}
}

void glEnable(GLenum c) {
	bool *p = cap(c);
	ZGL_REC(ZGLR_ENABLE, (int32_t)c, 0, 0, 0);
	if (!p) { set_error(GL_INVALID_ENUM); return; }
	*p = true;
	zgl_s.lserial++;
	zgl_s.gen++;
}

void glDisable(GLenum c) {
	bool *p = cap(c);
	ZGL_REC(ZGLR_DISABLE, (int32_t)c, 0, 0, 0);
	if (!p) { set_error(GL_INVALID_ENUM); return; }
	*p = false;
	zgl_s.lserial++;
	zgl_s.gen++;
}

GLboolean glIsEnabled(GLenum c) {
	bool *p = cap(c);
	if (!p) { set_error(GL_INVALID_ENUM); return GL_FALSE; }
	return *p ? GL_TRUE : GL_FALSE;
}

void glShadeModel(GLenum m) {
	ZGL_REC(ZGLR_SHADE, (int32_t)m, 0, 0, 0);
	if (m != GL_FLAT && m != GL_SMOOTH) { set_error(GL_INVALID_ENUM); return; }
	zgl_s.smooth = (m == GL_SMOOTH);
}

void glPolygonMode(GLenum face, GLenum mode) {
	if (mode != GL_POINT && mode != GL_LINE && mode != GL_FILL) { set_error(GL_INVALID_ENUM); return; }
	if (face == GL_FRONT || face == GL_FRONT_AND_BACK) zgl_s.poly_front = mode;
	if (face == GL_BACK || face == GL_FRONT_AND_BACK) zgl_s.poly_back = mode;
	if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) set_error(GL_INVALID_ENUM);
}

void glCullFace(GLenum m) {
	if (m != GL_FRONT && m != GL_BACK && m != GL_FRONT_AND_BACK) { set_error(GL_INVALID_ENUM); return; }
	zgl_s.cull_face = m;
}

void glFrontFace(GLenum m) {
	if (m != GL_CW && m != GL_CCW) { set_error(GL_INVALID_ENUM); return; }
	zgl_s.front_ccw = (m == GL_CCW);
}

void glPointSize(GLfloat s) { (void)s; }
void glLineWidth(GLfloat w) { (void)w; }

void glClearIndex(GLfloat c) { zgl_s.clear_index = (uint8_t)(int)c; }
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a) {
	(void)r; (void)g; (void)b; (void)a;
}

/* ZGL_LINES: every frame is drawn from nothing (zgl_frame() erases
 * what went), so a clear is implied and asks for nothing more */
void glClear(GLbitfield mask) {
	(void)mask;
}

void glFlush(void) {}
void glFinish(void) {
	if (zgl_s.output == ZGL_FRAMES && zglc_finish) zglc_finish();
}

/* -- colour ------------------------------------------------------------------------- */

ZGL_HOT void glIndexi(GLint c) {
	ZGL_REC(ZGLR_INDEX, c, 0, 0, 0);
	zgl_s.index = (uint8_t)c;
}

/* RGB: to the nearest of the eight corners of the colour cube, as
 * index r | g << 1 | b << 2 (the runtime's palette is the program's;
 * a program that wants exact colours uses glIndexi) */
void glColor3ub(GLubyte r, GLubyte g, GLubyte b) {
	glIndexi((r >= 128 ? 1 : 0) | (g >= 128 ? 2 : 0) | (b >= 128 ? 4 : 0));
}

void glColor3x(GLfixed r, GLfixed g, GLfixed b) {
	glColor3ub(r >= 32768 ? 255 : 0, g >= 32768 ? 255 : 0, b >= 32768 ? 255 : 0);
}

void glColor3f(GLfloat r, GLfloat g, GLfloat b) {
	glColor3x(from_float(r), from_float(g), from_float(b));
}

/* -- textures (T2 only: zgl_t2.c, docs/zgl.md "Textures") ---------------- */

/* texels, 12.4: a texture coordinate of up to +-1,024 texels */
void glTexCoord2i(GLint s, GLint t) {
	ZGL_REC(ZGLR_TEXCOORD, s * 16, t * 16, 0, 0);
	zgl_s.tex_s = s * 16;
	zgl_s.tex_t = t * 16;
}

/* texels, 16.16 */
void glTexCoord2x(GLfixed s, GLfixed t) {
	int32_t a = (s + 2048) >> 12, b = (t + 2048) >> 12;
	ZGL_REC(ZGLR_TEXCOORD, a, b, 0, 0);
	zgl_s.tex_s = a;
	zgl_s.tex_t = b;
}

void glBindTexture(GLenum target, GLuint name) {
	if (target != GL_TEXTURE_2D) { set_error(GL_INVALID_ENUM); return; }
	ZGL_REC(ZGLR_BIND, (int32_t)name, 0, 0, 0);
	zgl_s.tex_bound = name;
}

ZGL_HOT void glNormal3x(GLfixed nx, GLfixed ny, GLfixed nz) {
	ZGL_REC(ZGLR_NORMAL, nx, ny, nz, 0);
	zgl_s.normal[0] = nx; zgl_s.normal[1] = ny; zgl_s.normal[2] = nz;
}

void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz) {
	glNormal3x(from_float(nx), from_float(ny), from_float(nz));
}

/* integers of any length (a model's int8 normals): made unit length */
ZGL_HOT void glNormal3i(GLint nx, GLint ny, GLint nz) {
	uint32_t len = isqrt64((uint64_t)((int64_t)nx * nx + (int64_t)ny * ny + (int64_t)nz * nz));
	if (!len) { glNormal3x(0, 0, FIX_ONE); return; }
	glNormal3x((int32_t)(((int64_t)nx * FIX_ONE) / len),
	           (int32_t)(((int64_t)ny * FIX_ONE) / len),
	           (int32_t)(((int64_t)nz * FIX_ONE) / len));
}

/* the modelview's upper 3x3 times a direction (16.16) */
ZGL_HOT static void mv_dir(const int32_t *in, int32_t *out) {
	const int32_t *m = zgl_s.mv[zgl_s.mv_top];
	int r;
	for (r = 0; r < 3; r++)
		out[r] = (int32_t)(((int64_t)m[r] * in[0] + (int64_t)m[4 + r] * in[1] +
		                    (int64_t)m[8 + r] * in[2]) >> MAT_SH);
}

void zgl_unit3(int32_t *v) {
	uint32_t len = isqrt64((uint64_t)((int64_t)v[0] * v[0] + (int64_t)v[1] * v[1] +
	                                  (int64_t)v[2] * v[2]));
	if (!len) return;
	v[0] = fdiv(v[0], (int32_t)len);
	v[1] = fdiv(v[1], (int32_t)len);
	v[2] = fdiv(v[2], (int32_t)len);
}

void glLightxv(GLenum light, GLenum pname, const GLfixed *p) {
	int l = (int)light - GL_LIGHT0;
	if (l < 0 || l > 1) { set_error(GL_INVALID_ENUM); return; }
	zgl_s.lserial++;
	switch (pname) {
	case GL_POSITION:
		/* as GL: through the modelview of the moment. A direction (w
		 * 0); or a position, where the light is, which each vertex's
		 * light then comes from */
		mv_dir(p, zgl_s.light_dir[l]);
		zgl_s.light_pt[l] = p[3] != 0;
		if (zgl_s.light_pt[l]) {
			const int32_t *m = zgl_s.mv[zgl_s.mv_top];
			int32_t q[3];
			int r;
			/* x / w, as 24.8 */
			for (r = 0; r < 3; r++) q[r] = (int32_t)(((int64_t)p[r] * POS_ONE) / p[3]);
			for (r = 0; r < 3; r++)
				zgl_s.light_pos[l][r] = (int32_t)(((int64_t)m[r] * q[0] + (int64_t)m[4 + r] * q[1] +
				                                   (int64_t)m[8 + r] * q[2]) >> MAT_SH) + m[12 + r];
		}
		zgl_unit3(zgl_s.light_dir[l]);
		zgl_s.gen++;
		break;
	case GL_CONSTANT_ATTENUATION:
		zgl_s.light_kc[l] = p[0];
		zgl_s.gen++;
		break;
	case GL_LINEAR_ATTENUATION:
		/* 16.16 in, 8.24 kept */
		zgl_s.light_kl[l] = p[0] > 0x7fffff ? INT32_MAX : p[0] < 0 ? 0 : p[0] << 8;
		zgl_s.gen++;
		break;
	case GL_QUADRATIC_ATTENUATION:
		/* 16.16 in, 0.32 kept (at most one per unit squared) */
		zgl_s.light_kq[l] = p[0] >= 0x10000 ? 0xffffffffu : p[0] < 0 ? 0 : (uint32_t)p[0] << 16;
		zgl_s.gen++;
		break;
	case GL_DIFFUSE:
		zgl_s.light_d[l] = (int32_t)(((int64_t)p[0] * 77 + (int64_t)p[1] * 151 +
		                              (int64_t)p[2] * 28) >> 8);
		zgl_s.gen++;
		break;
	case GL_AMBIENT:
	case GL_SPECULAR:
		break;
	default:
		set_error(GL_INVALID_ENUM);
	}
}

void glLightfv(GLenum light, GLenum pname, const GLfloat *p) {
	int32_t x[4];
	int i;
	int l = (int)light - GL_LIGHT0;
	/* the attenuation's small factors, to more bits than 16.16 holds */
	if (l >= 0 && l <= 1 && pname == GL_LINEAR_ATTENUATION) {
		float v = p[0] * 16777216.0f;
		zgl_s.light_kl[l] = v >= 2147483647.0f ? INT32_MAX : v <= 0.0f ? 0 : (int32_t)v;
		zgl_s.gen++; zgl_s.lserial++;
		return;
	}
	if (l >= 0 && l <= 1 && pname == GL_QUADRATIC_ATTENUATION) {
		float v = p[0] * 4294967296.0f;
		zgl_s.light_kq[l] = v >= 4294967295.0f ? 0xffffffffu : v <= 0.0f ? 0 : (uint32_t)v;
		zgl_s.gen++; zgl_s.lserial++;
		return;
	}
	for (i = 0; i < 4; i++) x[i] = pname == GL_POSITION || pname == GL_DIFFUSE || i == 0 ? from_float(p[i]) : 0;
	glLightxv(light, pname, x);
}

/* the scalars: the three attenuation factors */
void glLightx(GLenum light, GLenum pname, GLfixed v) {
	GLfixed p[4];
	p[0] = v; p[1] = p[2] = p[3] = 0;
	glLightxv(light, pname, p);
}

void glLightf(GLenum light, GLenum pname, GLfloat v) {
	GLfloat p[4];
	p[0] = v; p[1] = p[2] = p[3] = 0;
	glLightfv(light, pname, p);
}

/* 1 / (kc + kl d + kq d^2), 16.16, at most one; d 16.16 world units */
int32_t zgl_atten(int l, int64_t d) {
	int64_t den = (int64_t)zgl_s.light_kc[l];
	if (d < 0) d = 0;
	if (d > INT32_MAX) d = INT32_MAX;
	den += ((int64_t)zgl_s.light_kl[l] * d) >> 24;
	den += ((((uint64_t)d * (uint64_t)d) >> 32) * zgl_s.light_kq[l]) >> 16;
	if (den <= FIX_ONE) return FIX_ONE;
	return (int32_t)(((int64_t)1 << 32) / den);
}

void zgl_material(int32_t a, int32_t d) {
	ZGL_REC(ZGLR_MATERIAL, a, d, 0, 0);
	zgl_s.mat_a = a;
	zgl_s.mat_d = d;
	zgl_s.gen++;
}

void glMaterialiv(GLenum face, GLenum pname, const GLint *p) {
	(void)face;
	if (pname != GL_COLOR_INDEXES) { set_error(GL_INVALID_ENUM); return; }
	zgl_material(p[0] * 256, p[1] * 256);
}

void glMaterialfv(GLenum face, GLenum pname, const GLfloat *p) {
	(void)face;
	if (pname != GL_COLOR_INDEXES) { set_error(GL_INVALID_ENUM); return; }
	zgl_material((int32_t)(p[0] * 256.0f), (int32_t)(p[1] * 256.0f));
}

/* A positional light's 16.16 dot at the vertex at object (x, y, z),
 * 24.8, whose eye normal is n: n . the unit vector to the light, times
 * the attenuation. Kept apart from vertex_ci() (it is the rare case). */
static int32_t point_dot(int l, const int32_t *n, int32_t x, int32_t y, int32_t z) {
	const int32_t *m = zgl_s.mv[zgl_s.mv_top];
	int64_t L[3], d2, nl;
	uint32_t d;
	int r;
	for (r = 0; r < 3; r++)
		L[r] = (int64_t)zgl_s.light_pos[l][r] -
		       ((((int64_t)m[r] * x + (int64_t)m[4 + r] * y + (int64_t)m[8 + r] * z) >> MAT_SH) + m[12 + r]);
	d2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2];
	d = isqrt64((uint64_t)d2);                  /* 24.8 */
	if (!d) return FIX_ONE;
	nl = (int64_t)n[0] * L[0] + (int64_t)n[1] * L[1] + (int64_t)n[2] * L[2];   /* 16.16 x 24.8 */
	return (int32_t)(((nl / (int64_t)d) * zgl_atten(l, (int64_t)d << 8)) >> 16);
}

/* the colour index of the vertex being made, 8.8: lit, or as set; at
 * object (x, y, z), for a positional light */
ZGL_HOT static int32_t vertex_ci(int32_t x, int32_t y, int32_t z) {
	int32_t n[3], d = 0;
	int l;
	if (!zgl_s.lighting) return (int32_t)zgl_s.index << 8;
	mv_dir(zgl_s.normal, n);
	if (zgl_s.normalize) zgl_unit3(n);
	for (l = 0; l < 2; l++) {
		int32_t dot;
		if (!zgl_s.light_on[l]) continue;
		if (zgl_s.light_pt[l]) dot = point_dot(l, n, x, y, z);
		else dot = (int32_t)(((int64_t)n[0] * zgl_s.light_dir[l][0] +
		                      (int64_t)n[1] * zgl_s.light_dir[l][1] +
		                      (int64_t)n[2] * zgl_s.light_dir[l][2]) >> 16);
		if (dot > 0) d += fmul(dot, zgl_s.light_d[l]);
	}
	if (d > FIX_ONE) d = FIX_ONE;
	return zgl_s.mat_a + (int32_t)(((int64_t)(zgl_s.mat_d - zgl_s.mat_a) * d) >> 16);
}

/* -- vertices ------------------------------------------------------------------------- */

/* Near and far as eye depths, when the projection is a perspective one
 * (its w row (0, 0, -1, 0)): then the near and far planes are clipped
 * against w itself (w = -z_eye) rather than against clip z. With a far
 * plane thousands of times the near one, clip z changes by a thousandth
 * of a unit per unit of depth, which at 24.8 leaves the near crossing
 * of a long line pixels out; w is exact. */
static void make_mvp(void) {
	const int32_t *p = zgl_s.proj[zgl_s.proj_top];
	const int32_t *b = zgl_s.mv[zgl_s.mv_top];
	int32_t *o = zgl_s.mvp;
	int k, rr, c;
	/* mat_mul(mvp, p, mv), over p's non-zero entries only: a frustum
	 * has seven of sixteen, so this runs for every model a frame. Each
	 * entry summed in registers and stored once: on a board whose data
	 * cache writes through, a store is the cost, not a multiply. */
	for (rr = 0; rr < 4; rr++) {
		int64_t a0 = p[rr], a1 = p[4 + rr], a2 = p[8 + rr], a3 = (int64_t)p[12 + rr] * (MAT_ONE / POS_ONE);
		for (c = 0; c < 4; c++) {
			const int32_t *bc = &b[c * 4];
			int64_t acc = 0;
			if (a0) acc += a0 * bc[0];
			if (a1) acc += a1 * bc[1];
			if (a2) acc += a2 * bc[2];
			if (a3) acc += a3 * bc[3];
			o[c * 4 + rr] = (int32_t)(acc >> MAT_SH);
		}
	}
	(void)k;
	zgl_s.persp = p[3] == 0 && p[7] == 0 && p[11] == -MAT_ONE && p[15] == 0;
	if (zgl_s.persp && zgl_s.p_exact[zgl_s.proj_top]) {
		zgl_s.near_w = zgl_s.p_near[zgl_s.proj_top];
		zgl_s.far_w = zgl_s.p_far[zgl_s.proj_top];
	} else if (zgl_s.persp) {
		/* z_c = A z_e + B; near: z_c = -w = z_e; far: z_c = w = -z_e */
		int64_t A = p[10], B = p[14];
		int64_t dn = MAT_ONE - A, df = MAT_ONE + A;
		zgl_s.near_w = dn ? (int32_t)((-B * MAT_ONE * 16) / dn) : 0;
		zgl_s.far_w = df > 0 ? (int32_t)((B * MAT_ONE * 16) / df) : INT32_MAX;
		if (df <= 0 || zgl_s.far_w <= 0) zgl_s.far_w = INT32_MAX;
	}
	zgl_s.mvp_dirty = false;
	zgl_s.mvp_serial++;
}

void zgl_sync_mvp(void) {
	if (zgl_s.mvp_dirty) make_mvp();
}

/* object (24.8) -> clip (20.12: clip coordinates are eye-space sized,
 * and ortho ones within +-1, which 8 bits of fraction would leave a
 * pixel coarse). Columns 0-2 of the matrix are 16.16
 * ratios; column 3 is a translation already in 24.8 units (see
 * translate_pos()), which in a product of two matrices stays true:
 * and mat_mul() keeps that through every product. */
static void plane_d(const zgl_vtx_t *v, int32_t *d);

/* the planes of the view volume (plane_d()) a vertex is outside of, a
 * bit each: in registers, as it is worked out for every vertex */
static inline uint8_t out_code(const zgl_vtx_t *v) {
	int32_t w = v->w;
	uint8_t c = (uint8_t)((w + v->x < 0) | (w - v->x < 0) << 1 |
	                      (w + v->y < 0) << 2 | (w - v->y < 0) << 3);
	if (zgl_s.persp) {
		if (w - zgl_s.near_w < 0) c |= 16;
		if (zgl_s.far_w != INT32_MAX && zgl_s.far_w - w < 0) c |= 32;
	} else {
		if (w + v->z < 0) c |= 16;
		if (w - v->z < 0) c |= 32;
	}
	return c;
}

ZGL_HOT static void xform(zgl_vtx_t *o, int32_t x, int32_t y, int32_t z) {
	const int32_t *m = zgl_s.mvp;
#define ROW(r) (int32_t)((((int64_t)m[r] * x + (int64_t)m[4 + r] * y + \
	(int64_t)m[8 + r] * z) + (1 << 15)) >> 16) + m[12 + r] * 16
	o->x = ROW(0);
	o->y = ROW(1);
	/* z is only needed when it is clipped against (not perspective,
	 * where near and far are w's) or kept for depth */
	o->z = (zgl_s.persp && !zgl_s.depth_test) ? 0 : ROW(2);
	o->w = ROW(3);
#undef ROW
	o->c = zgl_s.index;
	o->out = out_code(o);
	o->pv = 0;
	o->sv = 0;
}

static void screen_of(zgl_vtx_t *v);

/* xform() through the cache: the cached vertex itself, whose pixel,
 * once found, stays found for every line that shares the corner */
static unsigned vc_slot(int32_t x, int32_t y, int32_t z) {
	/* multiplicative, from the top bits: positions are 24.8, so whole
	 * coordinates have their low bits all zero */
	uint32_t h = (uint32_t)x * 0x9E3779B1u + (uint32_t)y * 0x85EBCA77u +
	             (uint32_t)z * 0xC2B2AE3Du;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	return h >> (32 - ZGL_VCACHE_BITS);
}

/* a cache miss: transform, and (filled) project, into the entry */
ZGL_HOT __attribute__((noinline)) static void vc_fill(zgl_vcache_t *e, int32_t x, int32_t y, int32_t z) {
	xform(&e->v, x, y, z);
	e->ox = x; e->oy = y; e->oz = z;
	e->gen = zgl_s.gen;
	e->ci_ok = 0;
	/* filled, a corner is projected once for all its triangles */
	if (zgl_s.output == ZGL_FRAMES && !e->v.out) screen_of(&e->v);
}

/* the entry lit for the current normal and index */
ZGL_HOT __attribute__((noinline)) static void vc_light(zgl_vcache_t *e) {
	e->v.ci = vertex_ci(e->ox, e->oy, e->oz);
	e->v.c = (uint8_t)((e->v.ci + 128) >> 8);
	e->nx = zgl_s.normal[0]; e->ny = zgl_s.normal[1]; e->nz = zgl_s.normal[2];
	e->idx = zgl_s.index;
	e->ci_ok = 1;
}

ZGL_HOT static zgl_vtx_t *xform_cp(unsigned slot, int32_t x, int32_t y, int32_t z) {
	zgl_vcache_t *e = &zgl_s.vc[slot];
	if (e->gen != zgl_s.gen || e->ox != x || e->oy != y || e->oz != z) vc_fill(e, x, y, z);
	/* lit again only for another normal or index (a model's corners
	 * that share a position mostly share a normal too) */
	if (!zgl_s.lighting) {
		/* stored only if it changed: on a board whose stores go out to
		 * memory one by one, a store costs a dozen loads */
		if (e->v.c != zgl_s.index || e->ci_ok || e->v.ci != (int32_t)zgl_s.index << 8) {
			e->v.ci = (int32_t)zgl_s.index << 8;
			e->v.c = zgl_s.index;
			e->ci_ok = 0;
		}
	} else if (!e->ci_ok || e->idx != zgl_s.index || e->nx != zgl_s.normal[0] ||
	    e->ny != zgl_s.normal[1] || e->nz != zgl_s.normal[2])
		vc_light(e);
	return &e->v;
}

static void xform_c(zgl_vtx_t *o, int32_t x, int32_t y, int32_t z) {
	*o = *xform_cp(vc_slot(x, y, z), x, y, z);
}

/* A corner held by the primitive being assembled is in a cache entry a
 * newer corner is about to take: it moves to vs[], to a place no held
 * corner uses, and its holders follow it. */
static void stash(zgl_vtx_t *ev) {
	int s, i;
	for (s = 0; s < 4; s++) {
		zgl_vtx_t *c = &zgl_s.vs[s];
		if (zgl_s.vp[0] != c && zgl_s.vp[1] != c && zgl_s.vp[2] != c && zgl_s.vp[3] != c) break;
	}
	zgl_s.vs[s] = *ev;
	for (i = 0; i < 4; i++)
		if (zgl_s.vp[i] == ev) zgl_s.vp[i] = &zgl_s.vs[s];
}

/* a filled primitive's next corner (assembly by pointer) */
static void prim_corner(zgl_vtx_t *v) {
	zgl_vtx_t **q = zgl_s.vp;
	unsigned k = zgl_s.count++;
	switch (zgl_s.prim) {
	case GL_TRIANGLES:
		q[k % 3] = v;
		if (k % 3 == 2) { zgl_polygon(q, 3); q[0] = q[1] = q[2] = NULL; }
		break;
	case GL_TRIANGLE_STRIP:
		if (k >= 2) {
			zgl_vtx_t *t[3];
			/* every other one reversed, to keep one winding */
			if (k & 1) { t[0] = q[1]; t[1] = q[0]; } else { t[0] = q[0]; t[1] = q[1]; }
			t[2] = v;
			zgl_polygon(t, 3);
			q[0] = q[1];
			q[1] = v;
		} else q[k] = v;
		break;
	case GL_TRIANGLE_FAN:
		if (k >= 2) {
			zgl_vtx_t *t[3];
			t[0] = q[0]; t[1] = q[1]; t[2] = v;
			zgl_polygon(t, 3);
			q[1] = v;
		} else q[k] = v;
		break;
	case GL_QUADS:
		q[k & 3] = v;
		if ((k & 3) == 3) { zgl_polygon(q, 4); q[0] = q[1] = q[2] = q[3] = NULL; }
		break;
	default:                            /* GL_QUAD_STRIP */
		if (k >= 3 && (k & 1)) {
			zgl_vtx_t *t[4];
			t[0] = q[0]; t[1] = q[1]; t[2] = v; t[3] = q[2];
			zgl_polygon(t, 4);
			q[0] = q[2];
			q[1] = v;
			q[2] = NULL;
		} else if (k < 3) q[k] = v;
		else q[2] = v;
		break;
	}
}

/* emit_vertex()'s three ways, each its own function so that the one
 * taken saves only the registers it uses (on a board whose stores go
 * out to memory one by one, a function's prologue is a real cost) */
ZGL_HOT __attribute__((noinline)) static void emit_line(int32_t x, int32_t y, int32_t z) {
	/* GL_LINES, the common case: straight into its pair, no copies */
	unsigned slot = vc_slot(x, y, z);
	if (!(zgl_s.count++ & 1)) {
		zgl_s.l0 = xform_cp(slot, x, y, z);
		zgl_s.l0_slot = slot;
	} else {
		zgl_vtx_t *b;
		if (slot == zgl_s.l0_slot && zgl_s.l0 != &zgl_s.vq[0]) {
			/* the second would evict the first: keep a copy */
			zgl_s.vq[0] = *zgl_s.l0;
			zgl_s.l0 = &zgl_s.vq[0];
		}
		b = xform_cp(slot, x, y, z);
		zgl_line(zgl_s.l0, b);
	}
}

ZGL_HOT __attribute__((noinline)) static void emit_filled(int32_t x, int32_t y, int32_t z) {
	/* filled: the corner by pointer, no copy (stash(), prim_corner()) */
	unsigned slot = vc_slot(x, y, z);
	zgl_vtx_t *ev = &zgl_s.vc[slot].v;
	if (zgl_s.vp[0] == ev || zgl_s.vp[1] == ev || zgl_s.vp[2] == ev || zgl_s.vp[3] == ev)
		stash(ev);
	prim_corner(xform_cp(slot, x, y, z));
}

__attribute__((noinline)) static void emit_other(int32_t x, int32_t y, int32_t z) {
	zgl_vtx_t v;
	xform_c(&v, x, y, z);
	zgl_prim_vertex(&v);
}

/* the rare cases before a vertex: recording, outside glBegin, a new
 * matrix (false: not drawn now) */
__attribute__((noinline, cold)) static bool emit_pre(int32_t x, int32_t y, int32_t z) {
	/* ZGL_REC(), which returns: recorded and not drawn */
	if (zgl_s.listing && zglr_rec && !zglr_rec(ZGLR_VERTEX, x, y, z, 0)) return false;
	if (!zgl_s.in_begin) { set_error(GL_INVALID_OPERATION); return false; }
	if (zgl_s.mvp_dirty) make_mvp();
	return true;
}

ZGL_HOT static void emit_vertex(int32_t x, int32_t y, int32_t z) {
	if ((zgl_s.listing || !zgl_s.in_begin || zgl_s.mvp_dirty) && !emit_pre(x, y, z)) return;
	switch (zgl_s.prim) {
	case GL_LINES:
		emit_line(x, y, z);
		break;
	case GL_TRIANGLES: case GL_TRIANGLE_STRIP: case GL_TRIANGLE_FAN:
	case GL_QUADS: case GL_QUAD_STRIP:
		emit_filled(x, y, z);
		break;
	default:
		emit_other(x, y, z);
		break;
	}
}

void glVertex2i(GLint x, GLint y) { emit_vertex(x * POS_ONE, y * POS_ONE, 0); }
ZGL_HOT void glVertex3i(GLint x, GLint y, GLint z) { emit_vertex(x * POS_ONE, y * POS_ONE, z * POS_ONE); }
void glVertex3iv(const GLint *v) { glVertex3i(v[0], v[1], v[2]); }
void glVertex2x(GLfixed x, GLfixed y) { emit_vertex(x >> 8, y >> 8, 0); }
ZGL_HOT void glVertex3x(GLfixed x, GLfixed y, GLfixed z) { emit_vertex(x >> 8, y >> 8, z >> 8); }
void glVertex2f(GLfloat x, GLfloat y) {
	emit_vertex((int32_t)(x * 256.0f), (int32_t)(y * 256.0f), 0);
}
void glVertex3f(GLfloat x, GLfloat y, GLfloat z) {
	emit_vertex((int32_t)(x * 256.0f), (int32_t)(y * 256.0f), (int32_t)(z * 256.0f));
}
void glVertex3fv(const GLfloat *v) { glVertex3f(v[0], v[1], v[2]); }

/* -- clipping and projection ------------------------------------------------------------ */

/* signed distances to the six planes of the view volume */
static void plane_d(const zgl_vtx_t *v, int32_t *d) {
	d[0] = v->w + v->x; d[1] = v->w - v->x;
	d[2] = v->w + v->y; d[3] = v->w - v->y;
	if (zgl_s.persp) {
		d[4] = v->w - zgl_s.near_w;
		d[5] = zgl_s.far_w == INT32_MAX ? INT32_MAX / 2 : zgl_s.far_w - v->w;
	} else {
		d[4] = v->w + v->z; d[5] = v->w - v->z;
	}
}

/* t in 0..2^30: a line from far away to just in front of the eye
 * needs that much -- with 16 bits the near end of one 4,000 units long
 * lands 10 pixels out */
#define T_ONE ((int64_t)1 << 30)

static void lerp(zgl_vtx_t *o, const zgl_vtx_t *a, const zgl_vtx_t *b, int64_t t) {
	o->x = a->x + (int32_t)(((int64_t)(b->x - a->x) * t) >> 30);
	o->y = a->y + (int32_t)(((int64_t)(b->y - a->y) * t) >> 30);
	o->z = a->z + (int32_t)(((int64_t)(b->z - a->z) * t) >> 30);
	o->w = a->w + (int32_t)(((int64_t)(b->w - a->w) * t) >> 30);
	o->ci = a->ci + (int32_t)(((int64_t)(b->ci - a->ci) * t) >> 30);
	o->c = a->c;
	o->out = 0;
	o->pv = 0;
	o->sv = 0;
}

/* clip coordinates -> picture coordinates, 24.8. x/w is taken with
 * the 32-bit hardware divide, not a 64-bit library one (hundreds of
 * cycles, twice a vertex): w is brought below 2^19 by shifting both,
 * and x / w found to 10 bits of fraction -- a sixth of a pixel across
 * 320. Only a point far outside the view (|x| or |y| over 4w, which
 * clipped lines never are) takes the 64-bit way. */
ZGL_HOT static int32_t ndc_px(int32_t x, int32_t w, int32_t half) {
	int32_t q;
	int sh = 0;
	while (w >= (1 << 19)) { w >>= 1; sh++; }
	x >>= sh;
	if (w <= 0) w = 1;
	if (x > 4 * w || x < -4 * w)
		return (int32_t)(((int64_t)x * half) / w);
	q = (x * 1024) / w;
	return (q * half) / 1024;
}

ZGL_HOT static void project(const zgl_vtx_t *v, int32_t *sx, int32_t *sy) {
	int32_t hw = zgl_s.vp_w * (POS_ONE / 2);   /* half sizes, 24.8 */
	int32_t hh = zgl_s.vp_h * (POS_ONE / 2);
	int32_t w = v->w > 0 ? v->w : 1;
	*sx = zgl_s.vp_x * POS_ONE + hw + ndc_px(v->x, w, hw);
	*sy = zgl_s.vp_y * POS_ONE + hh - ndc_px(v->y, w, hh);
}

static int32_t to_px(int32_t p) {
	return (p + POS_ONE / 2) >> 8;     /* nearest pixel centre */
}

static void clamp_vp(int32_t *x, int32_t *y) {
	int32_t x0 = zgl_s.vp_x, y0 = zgl_s.vp_y;
	int32_t x1 = zgl_s.vp_x + zgl_s.vp_w - 1, y1 = zgl_s.vp_y + zgl_s.vp_h - 1;
	if (*x < x0) *x = x0;
	if (*x > x1) *x = x1;
	if (*y < y0) *y = y0;
	if (*y > y1) *y = y1;
}

/* the scissor, in picture coordinates (Cohen-Sutherland) */
static int sc_code(int32_t x, int32_t y) {
	int c = 0;
	if (x < zgl_s.sc_x0) c |= 1; else if (x > zgl_s.sc_x1) c |= 2;
	if (y < zgl_s.sc_y0) c |= 4; else if (y > zgl_s.sc_y1) c |= 8;
	return c;
}

static void scissor_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t c) {
	int c0 = sc_code(x0, y0), c1 = sc_code(x1, y1), i;
	for (i = 0; i < 6; i++) {
		int oc;
		int32_t x, y;
		if (!(c0 | c1)) { zgl_back_line(x0, y0, x1, y1, c); return; }
		if (c0 & c1) return;
		oc = c0 ? c0 : c1;
		if (oc & 8) { x = x0 + (int32_t)((int64_t)(x1 - x0) * (zgl_s.sc_y1 - y0) / (y1 - y0)); y = zgl_s.sc_y1; }
		else if (oc & 4) { x = x0 + (int32_t)((int64_t)(x1 - x0) * (zgl_s.sc_y0 - y0) / (y1 - y0)); y = zgl_s.sc_y0; }
		else if (oc & 2) { y = y0 + (int32_t)((int64_t)(y1 - y0) * (zgl_s.sc_x1 - x0) / (x1 - x0)); x = zgl_s.sc_x1; }
		else { y = y0 + (int32_t)((int64_t)(y1 - y0) * (zgl_s.sc_x0 - x0) / (x1 - x0)); x = zgl_s.sc_x0; }
		if (oc == c0) { x0 = x; y0 = y; c0 = sc_code(x0, y0); }
		else { x1 = x; y1 = y; c1 = sc_code(x1, y1); }
	}
}

/* the pixel a vertex (inside the view volume) lands on, found once */
static void vtx_px(zgl_vtx_t *v) {
	int32_t x, y;
	if (v->pv) return;
	project(v, &x, &y);
	x = to_px(x); y = to_px(y);
	clamp_vp(&x, &y);
	v->px = (int16_t)x; v->py = (int16_t)y;
	v->pv = 1;
}

static void to_svtx(const zgl_vtx_t *v, zgl_svtx_t *o);

static void out_line(zgl_vtx_t *a, zgl_vtx_t *b, uint8_t c) {
	if (zgl_s.output == ZGL_FRAMES) {
		zgl_svtx_t sa, sb;
		to_svtx(a, &sa);
		to_svtx(b, &sb);
		if (!zgl_s.smooth) sa.ci = sb.ci;
		if (zglf_line) zglf_line(&sa, &sb);
		return;
	}
	vtx_px(a);
	vtx_px(b);
	if (zgl_s.scissor) scissor_line(a->px, a->py, b->px, b->py, c);
	else zgl_back_line(a->px, a->py, b->px, b->py, c);
}

/* A line, clipped in clip space (Liang-Barsky on the six planes), the
 * colour of its first vertex (flat, as GL's provoking vertex is the
 * second: the same for a line of one colour) */
void zgl_line(zgl_vtx_t *a, zgl_vtx_t *b) {
	int32_t da[6], db[6];
	int64_t t0 = 0, t1 = T_ONE;
	int i;
	zgl_vtx_t p, q;
	/* wholly inside, the common case: no clipping, and each corner's
	 * pixel found once however many lines share it */
	if (!(a->out | b->out)) {
		out_line(a, b, b->c);
		return;
	}
	if (a->out & b->out) return;      /* both outside one plane */
	plane_d(a, da);
	plane_d(b, db);
	for (i = 0; i < 6; i++) {
		if (da[i] < 0 && db[i] < 0) return;
		if (da[i] < 0) {
			int64_t t = ((int64_t)da[i] * T_ONE) / ((int64_t)da[i] - db[i]);
			if (t > t0) t0 = t;
		} else if (db[i] < 0) {
			int64_t t = ((int64_t)da[i] * T_ONE) / ((int64_t)da[i] - db[i]);
			if (t < t1) t1 = t;
		}
	}
	if (t0 > t1) return;
	if (t0 > 0) lerp(&p, a, b, t0); else p = *a;
	if (t1 < T_ONE) lerp(&q, a, b, t1); else q = *b;
	p.pv = q.pv = 0;
	out_line(&p, &q, b->c);
}

void zgl_point(zgl_vtx_t *a) {
	if (a->out) return;
	out_line(a, a, a->c);
}

static void project(const zgl_vtx_t *v, int32_t *sx, int32_t *sy);

/* Facing: the sign of the polygon's area on the screen, positive for
 * counter-clockwise in GL's y-up sense. With every vertex in front of
 * the eye (w > 0, the usual case) from the projected positions; else
 * from det[x y w] in clip space, which has the same sign for the part
 * in front of the eye and stays valid for a triangle crossing the near
 * plane -- in whole units there (>> 12) so the triple products fit 64
 * bits, coarse but only for such triangles. */
ZGL_HOT static int facing(const zgl_vtx_t *a, const zgl_vtx_t *b, const zgl_vtx_t *c) {
	int64_t d;
	if (a->w > 0 && b->w > 0 && c->w > 0) {
		int32_t ax, ay, bx, by, cx, cy;
		if (a->sv) { ax = a->sx; ay = a->sy; } else project(a, &ax, &ay);
		if (b->sv) { bx = b->sx; by = b->sy; } else project(b, &bx, &by);
		if (c->sv) { cx = c->sx; cy = c->sy; } else project(c, &cx, &cy);
		/* picture y is down: negate for GL's sense */
		d = -((int64_t)(bx - ax) * (cy - ay) - (int64_t)(by - ay) * (cx - ax));
	} else {
		int64_t ax = a->x >> 12, ay = a->y >> 12, aw = a->w >> 12;
		int64_t bx = b->x >> 12, by = b->y >> 12, bw = b->w >> 12;
		int64_t cx = c->x >> 12, cy = c->y >> 12, cw = c->w >> 12;
		d = ax * (by * cw - bw * cy) - ay * (bx * cw - bw * cx) + aw * (bx * cy - by * cx);
	}
	return d > 0 ? 1 : (d < 0 ? -1 : 0);
}

/* -- ZGL_FRAMES: filled polygons ------------------------------------------------- */

/* clip coordinates -> the screen: picture 28.4, depth 0..65535 << 15
 * (z / w taken to 15 bits with the 32-bit divide, as x / w) */
ZGL_HOT static void screen_of(zgl_vtx_t *v) {
	int32_t z = v->z, w = v->w > 0 ? v->w : 1, q;
	project(v, &v->sx, &v->sy);
	while (w >= (1 << 15)) { w >>= 1; z >>= 1; }
	if (z > w) z = w;
	if (z < -w) z = -w;
	q = (z * 32768) / w;                       /* -32768..32768 */
	q += 32768;
	if (q > 65535) q = 65535;
	if (q < 0) q = 0;
	v->sz = q << 15;
	v->sv = 1;
}

static void to_svtx(const zgl_vtx_t *v, zgl_svtx_t *o) {
	zgl_vtx_t t;
	if (!v->sv) { t = *v; screen_of(&t); v = &t; }
	o->x = v->sx >> 4;
	o->y = v->sy >> 4;
	o->z = v->sz;
	o->ci = v->ci;
}

/* Sutherland-Hodgman against the planes the polygon's corners are
 * outside of, then a fan of triangles. Flat shading gives every
 * triangle the polygon's last corner's index (GL's provoking vertex). */
#define CLIP_MAX (ZGL_POLY_MAX + 8)

/* a polygon partly outside the view: clipped, then its fan */
__attribute__((noinline, cold)) static void clip_polygon(zgl_vtx_t **v, int n, uint8_t any, bool flat, int32_t flat_ci) {
	static zgl_vtx_t buf[2][CLIP_MAX];
	zgl_vtx_t *in, *out;
	int i, p, m, cur = 0;
	in = buf[1];
	for (i = 0; i < n; i++) in[i] = *v[i];
	for (p = 0; p < 6 && any; p++) {
		if (!(any & (1 << p))) continue;
		out = buf[cur];
		m = 0;
		for (i = 0; i < n; i++) {
			zgl_vtx_t *a = &in[i], *b = &in[(i + 1) % n];
			int32_t da[6], db[6];
			plane_d(a, da);
			plane_d(b, db);
			if (da[p] >= 0 && m < CLIP_MAX) out[m++] = *a;
			if ((da[p] >= 0) != (db[p] >= 0) && m < CLIP_MAX) {
				int64_t t = ((int64_t)da[p] * T_ONE) / ((int64_t)da[p] - db[p]);
				lerp(&out[m++], a, b, t);
			}
		}
		in = out;
		n = m;
		cur ^= 1;
		if (n < 3) return;
	}
	for (i = 0; i < n; i++) if (!in[i].sv) screen_of(&in[i]);
	for (i = 2; i < n; i++) zglf_tri_v(&in[0], &in[i - 1], &in[i], flat, flat_ci);
}

ZGL_HOT static void fill_polygon(zgl_vtx_t **v, int n) {
	uint8_t any = 0, all = 0xff;
	int i;
	int32_t flat_ci = v[n - 1]->ci;
	bool flat = !zgl_s.smooth;
	for (i = 0; i < n; i++) { any |= v[i]->out; all &= v[i]->out; }
	if (all) return;                          /* all outside one plane */
	if (!zglf_tri_v) return;
	if (any) { clip_polygon(v, n, any, flat, flat_ci); return; }
	/* inside: the corners as they are (projected, in the cache) */
	for (i = 0; i < n; i++) if (!v[i]->sv) screen_of(v[i]);
	for (i = 2; i < n; i++) zglf_tri_v(v[0], v[i - 1], v[i], flat, flat_ci);
}

/* a polygon (n >= 3, convex, in order) as GL_FILL/GL_LINE/GL_POINT
 * says; in ZGL_LINES a filled polygon is its outline */
/* GL_POINT, GL_LINE, and GL_FILL in ZGL_LINES: the corners, or the
 * outline, flat in the polygon's last vertex's colour (GL's provoking
 * vertex) */
__attribute__((noinline)) static void poly_outline(zgl_vtx_t **v, int n, GLenum mode) {
	int i;
	if (mode == GL_POINT) {
		for (i = 0; i < n; i++) zgl_point(v[i]);
		return;
	}
	for (i = 0; i < n; i++) {
		zgl_vtx_t a = *v[i], b = *v[(i + 1) % n];
		a.c = b.c = v[n - 1]->c;
		zgl_line(&a, &b);
	}
}

ZGL_HOT void zgl_polygon(zgl_vtx_t **v, int n) {
	int i, f;
	GLenum mode;
	bool front;
	if (n < 3) return;
	f = facing(v[0], v[1], v[2]);
	for (i = 3; f == 0 && i < n; i++) f = facing(v[0], v[i - 1], v[i]);
	front = zgl_s.front_ccw ? (f >= 0) : (f <= 0);
	if (zgl_s.cull) {
		if (zgl_s.cull_face == GL_FRONT_AND_BACK) return;
		if (zgl_s.cull_face == GL_BACK && !front) return;
		if (zgl_s.cull_face == GL_FRONT && front) return;
	}
	mode = front ? zgl_s.poly_front : zgl_s.poly_back;
	if (mode == GL_FILL && zgl_s.output == ZGL_FRAMES) {
		fill_polygon(v, n);
		return;
	}
	poly_outline(v, n, mode);
}

/* -- primitive assembly ------------------------------------------------------------------- */

void glBegin(GLenum mode) {
	ZGL_REC(ZGLR_BEGIN, (int32_t)mode, 0, 0, 0);
	if (zgl_s.in_begin) { set_error(GL_INVALID_OPERATION); return; }
	if (mode > GL_POLYGON) { set_error(GL_INVALID_ENUM); return; }
	zgl_s.in_begin = true;
	zgl_s.prim = mode;
	zgl_s.l0 = NULL;
	zgl_s.nv = 0;
	zgl_s.count = 0;
	zgl_s.vp[0] = zgl_s.vp[1] = zgl_s.vp[2] = zgl_s.vp[3] = NULL;
}

void zgl_prim_vertex(zgl_vtx_t *v) {
	zgl_vtx_t *q = zgl_s.vq;
	unsigned k = zgl_s.count++;
	switch (zgl_s.prim) {
	case GL_POINTS:
		zgl_point(v);
		break;
	case GL_LINES:
		if (k & 1) zgl_line(&q[0], v); else q[0] = *v;
		break;
	case GL_LINE_STRIP:
	case GL_LINE_LOOP:
		if (k == 0) { q[0] = *v; q[1] = *v; }
		else { zgl_line(&q[1], v); q[1] = *v; }
		break;
	case GL_POLYGON:
		if (zgl_s.nv < ZGL_POLY_MAX) zgl_s.poly[zgl_s.nv++] = *v;
		break;
	}
}

void glEnd(void) {
	ZGL_REC(ZGLR_END, 0, 0, 0, 0);
	if (!zgl_s.in_begin) { set_error(GL_INVALID_OPERATION); return; }
	if (zgl_s.prim == GL_LINE_LOOP && zgl_s.count >= 2)
		zgl_line(&zgl_s.vq[1], &zgl_s.vq[0]);
	if (zgl_s.prim == GL_POLYGON) {
		zgl_vtx_t *pv[ZGL_POLY_MAX];
		int i;
		for (i = 0; i < zgl_s.nv; i++) pv[i] = &zgl_s.poly[i];
		zgl_polygon(pv, zgl_s.nv);
	}
	zgl_s.in_begin = false;
}

/* -- the context ---------------------------------------------------------------------------- */

void zgl_reset_state(void) {
	int i;
	zgl_s.listing = false;
	zgl_s.matrix_mode = GL_MODELVIEW;
	zgl_s.mv_top = zgl_s.proj_top = 0;
	mat_identity(zgl_s.mv[0]);
	mat_identity(zgl_s.proj[0]);
	zgl_s.p_exact[0] = false;
	zgl_s.mvp_dirty = true;
	zgl_s.vp_x = 0; zgl_s.vp_y = 0; zgl_s.vp_w = ZGL_W; zgl_s.vp_h = ZGL_H;
	zgl_s.sc_x0 = 0; zgl_s.sc_y0 = 0; zgl_s.sc_x1 = ZGL_W - 1; zgl_s.sc_y1 = ZGL_H - 1;
	zgl_s.cull = zgl_s.depth_test = zgl_s.scissor = zgl_s.lighting = zgl_s.normalize = false;
	for (i = 0; i < 2; i++) zgl_s.light_on[i] = false;
	zgl_s.cull_face = GL_BACK;
	zgl_s.front_ccw = true;
	zgl_s.poly_front = zgl_s.poly_back = GL_FILL;
	zgl_s.smooth = true;
	zgl_s.index = 1;
	zgl_s.clear_index = 0;
	zgl_s.in_begin = false;
	zgl_s.error = GL_NO_ERROR;
	zgl_s.normal[0] = 0; zgl_s.normal[1] = 0; zgl_s.normal[2] = FIX_ONE;
	/* GL's defaults: light 0 white from the viewer, light 1 dark;
	 * colour indexes 0, 1 */
	for (i = 0; i < 2; i++) {
		zgl_s.light_dir[i][0] = 0; zgl_s.light_dir[i][1] = 0;
		zgl_s.light_dir[i][2] = FIX_ONE;
		zgl_s.light_d[i] = i == 0 ? FIX_ONE : 0;
		zgl_s.light_pt[i] = false;
		zgl_s.light_kc[i] = FIX_ONE;
		zgl_s.light_kl[i] = 0;
		zgl_s.light_kq[i] = 0;
	}
	zgl_s.mat_a = 0;
	zgl_s.mat_d = 256;
}

/* the recorded calls' way back in (zgl_lists.c) */
ZGL_HOT void zgl_vertex_pos(int32_t x, int32_t y, int32_t z) { emit_vertex(x, y, z); }
void zgl_translate_pos(int32_t x, int32_t y, int32_t z) { translate_pos(x, y, z); }
