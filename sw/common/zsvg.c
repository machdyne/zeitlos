/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * See zsvg.h.
 */

#include <string.h>
#include <stdlib.h>
#include "zsvg.h"

/* -- float maths, without libm --------------------------------------
 *
 * sqrt, sin, cos, tan and atan2, implemented here rather than linked.
 *
 * This file lives in sw/common and is meant for several apps -- view,
 * web, and a drawing app later -- and a library that only links if
 * every caller remembers -lm is a library that breaks the next time
 * someone uses it. sw/apps/gpu3d/stl.c already sets the precedent for
 * avoiding libm on this target.
 *
 * It also hid: the host compiler folds these into builtins, so the
 * file linked cleanly on the build machine and failed only on the
 * target, where they become calls into a library nothing was linking.
 *
 * Accuracy needed is modest. The result is a 1bpp raster a few hundred
 * pixels across, so an error of 1e-5 is orders of magnitude below one
 * pixel. tests/test_svg.c checks these against libm.
 */

#define ZS_PI   3.14159265358979f
#define ZS_PI2  1.57079632679490f

float zs_fabs(float x) { return x < 0.0f ? -x : x; }

float zs_floor(float x) {
	float t = (float)(long)x;
	return (x < 0.0f && t != x) ? t - 1.0f : t;
}

float zs_ceil(float x) {
	float t = (float)(long)x;
	return (x > 0.0f && t != x) ? t + 1.0f : t;
}

// Newton-Raphson. Twelve iterations from a rough guess is more than
// this needs and still cheaper than the branchy exact versions; it
// runs a few times per arc, not per pixel.
float zs_sqrt(float x) {

	float r;
	int i;

	if (x <= 0.0f) return 0.0f;

	r = (x < 1.0f) ? (x + 0.5f) : (x * 0.5f + 0.5f);
	for (i = 0; i < 12; i++) r = 0.5f * (r + x / r);

	return r;

}

// Range reduction to [-pi, pi], then the odd Taylor series to x^9.
// The angles here come from SVG rotate() and arc sweeps and are
// small, so a loop reduces faster than a division would.
float zs_sin(float x) {

	float x2;

	while (x >  ZS_PI) x -= 2.0f * ZS_PI;
	while (x < -ZS_PI) x += 2.0f * ZS_PI;

	// Fold into [-pi/2, pi/2], where the series is accurate.
	//
	// Without this the error at the ends of [-pi, pi] is 7e-3 -- a
	// hundredth of a radian, which at a 300-pixel radius is three
	// pixels of arc, and visible. sin(pi - x) == sin(x) makes it
	// free.
	if (x >  ZS_PI2) x =  ZS_PI - x;
	if (x < -ZS_PI2) x = -ZS_PI - x;

	x2 = x * x;

	// Odd Taylor terms to x^11. The next term is x^13/13!, which at
	// the worst case here (pi/2) is 2e-8 -- five orders of magnitude
	// below a pixel.
	return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f +
		x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f +
		x2 * (-1.0f / 39916800.0f))))));

}

float zs_cos(float x) { return zs_sin(x + ZS_PI2); }

float zs_tan(float x) {
	float c = zs_cos(x);
	if (zs_fabs(c) < 1e-6f) return (c < 0.0f) ? -1e6f : 1e6f;
	return zs_sin(x) / c;
}

static float zs_atan_unit(float x) {
	float x2 = x * x;
	return x * (0.99997726f + x2 * (-0.33262347f + x2 * (0.19354346f +
		x2 * (-0.11643287f + x2 * (0.05265332f + x2 * -0.01172120f)))));
}

float zs_atan2(float y, float x) {

	float a;

	if (x == 0.0f) {
		if (y > 0.0f) return ZS_PI2;
		if (y < 0.0f) return -ZS_PI2;
		return 0.0f;
	}

	if (zs_fabs(y) <= zs_fabs(x)) {
		a = zs_atan_unit(y / x);
		if (x < 0.0f) a += (y >= 0.0f) ? ZS_PI : -ZS_PI;
	} else {
		a = ZS_PI2 - zs_atan_unit(x / y);
		if (y < 0.0f) a -= ZS_PI;
	}

	return a;

}

// The rest of this file is written in the usual names.
#define fabsf   zs_fabs
#define floorf  zs_floor
#define ceilf   zs_ceil
#define sqrtf   zs_sqrt
#define sinf    zs_sin
#define cosf    zs_cos
#define tanf    zs_tan
#define atan2f  zs_atan2



// How finely curves are flattened, in device pixels. Below about half
// a pixel the extra segments are invisible on a 1bpp display and cost
// edges, which are the scarce resource here.
#define FLAT_TOL   0.4f

// Bezier subdivision depth. A hard stop, because the flatness test
// alone can recurse a long way on a degenerate curve -- and a
// malformed file should not be able to exhaust the stack.
#define MAX_DEPTH  10

#define MAX_XF     16			// nesting depth of <g transform=...>

const char *z_svg_strerror(int rv) {
	switch (rv) {
	case Z_SVG_OK:             return "ok";
	case Z_SVG_E_FORMAT:       return "not an SVG document";
	case Z_SVG_E_TOOCOMPLEX:   return "the drawing is too complex to render";
	case Z_SVG_E_UNSUPPORTED:  return "this SVG uses features that are not supported";
	default:                   return "the file could not be read";
	}
}

bool z_svg_sniff(const char *buf, uint32_t n) {

	uint32_t i;

	// "<svg" within the first stretch of the file. An XML declaration
	// and any number of comments or a DOCTYPE may come first, so the
	// tag is searched for rather than expected at offset zero.
	if (n > 1024) n = 1024;

	for (i = 0; i + 4 <= n; i++)
		if (buf[i] == '<' && (buf[i+1] == 's' || buf[i+1] == 'S') &&
			(buf[i+2] == 'v' || buf[i+2] == 'V') &&
			(buf[i+3] == 'g' || buf[i+3] == 'G'))
			return true;

	return false;

}

// -- a very small number parser -------------------------------------
//
// strtof() is not available in every build of this tree's libc, and
// SVG path data is a dialect anyway: "1-2" is two numbers, ".5.5" is
// two numbers, and a comma is whitespace.

static bool is_ws(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',';
}

static const char *skip_ws(const char *p, const char *end) {
	while (p < end && is_ws(*p)) p++;
	return p;
}

static const char *num(const char *p, const char *end, float *out) {

	float v = 0.0f, frac = 0.0f, scale = 0.1f;
	int sign = 1, esign = 1, ex = 0;
	bool any = false;

	p = skip_ws(p, end);
	if (p >= end) return NULL;

	if (*p == '+' || *p == '-') { sign = (*p == '-') ? -1 : 1; p++; }

	while (p < end && *p >= '0' && *p <= '9') {
		v = v * 10.0f + (float)(*p - '0');
		p++; any = true;
	}

	if (p < end && *p == '.') {
		p++;
		while (p < end && *p >= '0' && *p <= '9') {
			frac += (float)(*p - '0') * scale;
			scale *= 0.1f;
			p++; any = true;
		}
	}

	if (!any) return NULL;

	v += frac;

	if (p < end && (*p == 'e' || *p == 'E')) {
		const char *q = p + 1;
		if (q < end && (*q == '+' || *q == '-')) {
			esign = (*q == '-') ? -1 : 1; q++;
		}
		if (q < end && *q >= '0' && *q <= '9') {
			while (q < end && *q >= '0' && *q <= '9') {
				ex = ex * 10 + (*q - '0');
				q++;
			}
			while (ex--) v = (esign > 0) ? v * 10.0f : v * 0.1f;
			p = q;
		}
	}

	*out = (float)sign * v;
	return p;

}

// -- transforms -----------------------------------------------------

static void xf_identity(z_svg_xf_t *m) {
	m->a = 1; m->b = 0; m->c = 0; m->d = 1; m->e = 0; m->f = 0;
}

// r = m applied AFTER n, i.e. r(p) = m(n(p)).
static void xf_mul(z_svg_xf_t *r, const z_svg_xf_t *m, const z_svg_xf_t *n) {
	z_svg_xf_t t;
	t.a = m->a * n->a + m->c * n->b;
	t.b = m->b * n->a + m->d * n->b;
	t.c = m->a * n->c + m->c * n->d;
	t.d = m->b * n->c + m->d * n->d;
	t.e = m->a * n->e + m->c * n->f + m->e;
	t.f = m->b * n->e + m->d * n->f + m->f;
	*r = t;
}

static void xf_apply(const z_svg_xf_t *m, float x, float y,
	float *ox, float *oy) {
	*ox = m->a * x + m->c * y + m->e;
	*oy = m->b * x + m->d * y + m->f;
}

// Parses an SVG transform attribute: any sequence of translate,
// scale, rotate, matrix, skewX, skewY.
static void parse_transform(const char *p, const char *end, z_svg_xf_t *out) {

	xf_identity(out);

	while (p < end) {

		const char *name;
		int nlen;
		float v[6];
		int nv = 0;
		z_svg_xf_t t;

		p = skip_ws(p, end);
		if (p >= end) break;

		name = p;
		while (p < end && ((*p >= 'a' && *p <= 'z') ||
			(*p >= 'A' && *p <= 'Z'))) p++;
		nlen = (int)(p - name);
		if (nlen == 0) { p++; continue; }

		p = skip_ws(p, end);
		if (p >= end || *p != '(') continue;
		p++;

		while (nv < 6) {
			const char *q = num(p, end, &v[nv]);
			if (!q) break;
			p = q; nv++;
		}

		p = skip_ws(p, end);
		if (p < end && *p == ')') p++;

		xf_identity(&t);

		if (nlen == 9 && !memcmp(name, "translate", 9)) {
			t.e = nv > 0 ? v[0] : 0;
			t.f = nv > 1 ? v[1] : 0;
		} else if (nlen == 5 && !memcmp(name, "scale", 5)) {
			t.a = nv > 0 ? v[0] : 1;
			t.d = nv > 1 ? v[1] : t.a;
		} else if (nlen == 6 && !memcmp(name, "rotate", 6) && nv > 0) {
			float r = v[0] * 3.14159265f / 180.0f;
			float cs = cosf(r), sn = sinf(r);
			t.a = cs; t.b = sn; t.c = -sn; t.d = cs;
			if (nv >= 3) {
				// rotate(a, cx, cy) is translate(c) rotate(a) translate(-c)
				z_svg_xf_t pre, post;
				xf_identity(&pre);  pre.e  =  v[1]; pre.f  =  v[2];
				xf_identity(&post); post.e = -v[1]; post.f = -v[2];
				xf_mul(&t, &t, &post);
				xf_mul(&t, &pre, &t);
			}
		} else if (nlen == 6 && !memcmp(name, "matrix", 6) && nv >= 6) {
			t.a = v[0]; t.b = v[1]; t.c = v[2];
			t.d = v[3]; t.e = v[4]; t.f = v[5];
		} else if (nlen == 5 && !memcmp(name, "skewX", 5) && nv > 0) {
			t.c = tanf(v[0] * 3.14159265f / 180.0f);
		} else if (nlen == 5 && !memcmp(name, "skewY", 5) && nv > 0) {
			t.b = tanf(v[0] * 3.14159265f / 180.0f);
		}

		xf_mul(out, out, &t);

	}

}

// -- edge accumulation ----------------------------------------------

typedef struct {

	z_svg_t			*s;
	z_svg_xf_t		xf;				// current user -> device transform

	int				n;				// edges used
	bool			overflow;

	float			sx, sy;			// subpath start, device space
	float			cx, cy;			// current point, device space
	bool			open;

} ctx_t;

static void add_edge(ctx_t *c, float x0, float y0, float x1, float y1) {

	z_svg_edge_t *e;

	if (y0 == y1) return;			// horizontal edges contribute nothing

	if (c->n >= c->s->max_edges) { c->overflow = true; return; }

	e = &c->s->edges[c->n++];

	if (y0 < y1) {
		e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1; e->dir = 1;
	} else {
		e->x0 = x1; e->y0 = y1; e->x1 = x0; e->y1 = y0; e->dir = -1;
	}

}

static void line_to(ctx_t *c, float x, float y) {
	add_edge(c, c->cx, c->cy, x, y);
	c->cx = x; c->cy = y;
}

static void move_to(ctx_t *c, float x, float y) {
	// An open subpath is implicitly closed for FILLING, which is what
	// the spec says and what every renderer does. Stroking does not
	// close it, but strokes are emitted separately.
	if (c->open && (c->cx != c->sx || c->cy != c->sy))
		add_edge(c, c->cx, c->cy, c->sx, c->sy);
	c->sx = c->cx = x;
	c->sy = c->cy = y;
	c->open = true;
}

static void close_path(ctx_t *c) {
	if (c->open) add_edge(c, c->cx, c->cy, c->sx, c->sy);
	c->cx = c->sx; c->cy = c->sy;
}

// Recursive subdivision, stopping when the control points are within
// FLAT_TOL of the chord. Cheaper than computing an arc length and
// good enough at this resolution.
static void cubic(ctx_t *c, float x0, float y0, float x1, float y1,
	float x2, float y2, float x3, float y3, int depth) {

	float dx = x3 - x0, dy = y3 - y0;
	float d1 = fabsf((x1 - x3) * dy - (y1 - y3) * dx);
	float d2 = fabsf((x2 - x3) * dy - (y2 - y3) * dx);
	float dd = (d1 + d2) * (d1 + d2);

	if (depth >= MAX_DEPTH || dd < FLAT_TOL * (dx * dx + dy * dy)) {
		line_to(c, x3, y3);
		return;
	}

	{
		float x01 = (x0 + x1) * 0.5f, y01 = (y0 + y1) * 0.5f;
		float x12 = (x1 + x2) * 0.5f, y12 = (y1 + y2) * 0.5f;
		float x23 = (x2 + x3) * 0.5f, y23 = (y2 + y3) * 0.5f;
		float xa  = (x01 + x12) * 0.5f, ya = (y01 + y12) * 0.5f;
		float xb  = (x12 + x23) * 0.5f, yb = (y12 + y23) * 0.5f;
		float xm  = (xa + xb) * 0.5f, ym = (ya + yb) * 0.5f;

		cubic(c, x0, y0, x01, y01, xa, ya, xm, ym, depth + 1);
		cubic(c, xm, ym, xb, yb, x23, y23, x3, y3, depth + 1);
	}

}

// A quadratic raised to a cubic, so there is one curve routine.
static void quad(ctx_t *c, float x0, float y0, float x1, float y1,
	float x2, float y2) {
	cubic(c,
		x0, y0,
		x0 + 2.0f / 3.0f * (x1 - x0), y0 + 2.0f / 3.0f * (y1 - y0),
		x2 + 2.0f / 3.0f * (x1 - x2), y2 + 2.0f / 3.0f * (y1 - y2),
		x2, y2, 0);
}

// -- elliptical arcs ------------------------------------------------
//
// The A command, converted to centre form and then to cubics. It is
// the fiddliest part of SVG path data and it is not optional: circles
// and rounded rectangles in real files are written as arcs.
static void arc_to(ctx_t *c, float x0, float y0, float rx, float ry,
	float phi, int large, int sweep, float x, float y) {

	float dx2, dy2, x1p, y1p, cxp, cyp, cx, cy;
	float rxs, rys, num_, den, sq, sign;
	float th1, dth, ux, uy, vx, vy;
	int i, nseg;

	if (rx == 0.0f || ry == 0.0f) { line_to(c, x, y); return; }

	rx = fabsf(rx); ry = fabsf(ry);
	phi = phi * 3.14159265f / 180.0f;

	dx2 = (x0 - x) * 0.5f;
	dy2 = (y0 - y) * 0.5f;

	x1p =  cosf(phi) * dx2 + sinf(phi) * dy2;
	y1p = -sinf(phi) * dx2 + cosf(phi) * dy2;

	rxs = rx * rx; rys = ry * ry;

	// Radii too small to span the endpoints are scaled up, per spec,
	// rather than rejected.
	{
		float lam = (x1p * x1p) / rxs + (y1p * y1p) / rys;
		if (lam > 1.0f) {
			float k = sqrtf(lam);
			rx *= k; ry *= k;
			rxs = rx * rx; rys = ry * ry;
		}
	}

	num_ = rxs * rys - rxs * y1p * y1p - rys * x1p * x1p;
	den  = rxs * y1p * y1p + rys * x1p * x1p;
	if (den == 0.0f) { line_to(c, x, y); return; }
	sq = num_ / den;
	if (sq < 0.0f) sq = 0.0f;

	sign = (large == sweep) ? -1.0f : 1.0f;
	{
		float co = sign * sqrtf(sq);
		cxp =  co * rx * y1p / ry;
		cyp = -co * ry * x1p / rx;
	}

	cx = cosf(phi) * cxp - sinf(phi) * cyp + (x0 + x) * 0.5f;
	cy = sinf(phi) * cxp + cosf(phi) * cyp + (y0 + y) * 0.5f;

	ux = (x1p - cxp) / rx; uy = (y1p - cyp) / ry;
	vx = (-x1p - cxp) / rx; vy = (-y1p - cyp) / ry;

	th1 = atan2f(uy, ux);
	dth = atan2f(vy, vx) - th1;

	if (!sweep && dth > 0.0f) dth -= 2.0f * 3.14159265f;
	else if (sweep && dth < 0.0f) dth += 2.0f * 3.14159265f;

	// One cubic per quarter turn or less: the error of a cubic
	// approximation to a circular arc grows sharply past 90 degrees.
	nseg = (int)(fabsf(dth) / (3.14159265f * 0.5f)) + 1;
	if (nseg > 8) nseg = 8;

	for (i = 0; i < nseg; i++) {

		float a0 = th1 + dth * (float)i / (float)nseg;
		float a1 = th1 + dth * (float)(i + 1) / (float)nseg;
		float t = 4.0f / 3.0f * tanf((a1 - a0) * 0.25f);

		float px0 = cx + rx * cosf(a0) * cosf(phi) - ry * sinf(a0) * sinf(phi);
		float py0 = cy + rx * cosf(a0) * sinf(phi) + ry * sinf(a0) * cosf(phi);
		float px1 = cx + rx * cosf(a1) * cosf(phi) - ry * sinf(a1) * sinf(phi);
		float py1 = cy + rx * cosf(a1) * sinf(phi) + ry * sinf(a1) * cosf(phi);

		float d0x = -rx * sinf(a0) * cosf(phi) - ry * cosf(a0) * sinf(phi);
		float d0y = -rx * sinf(a0) * sinf(phi) + ry * cosf(a0) * cosf(phi);
		float d1x = -rx * sinf(a1) * cosf(phi) - ry * cosf(a1) * sinf(phi);
		float d1y = -rx * sinf(a1) * sinf(phi) + ry * cosf(a1) * cosf(phi);

		cubic(c, px0, py0, px0 + t * d0x, py0 + t * d0y,
			px1 - t * d1x, py1 - t * d1y, px1, py1, 0);

	}

}

// -- path data ------------------------------------------------------
//
// Everything is transformed to device space as it is parsed, so the
// edge list is device-space and the fill needs no matrix at all.

static void path_data(ctx_t *c, const char *p, const char *end) {

	float ux = 0, uy = 0;			// current point, USER space
	float sx = 0, sy = 0;			// subpath start, user space
	float rcx = 0, rcy = 0;			// reflection point for S/T
	char cmd = 0, prev = 0;

	while (p < end) {

		float v[7];
		int nv = 0, want = 0;
		float dx, dy;

		p = skip_ws(p, end);
		if (p >= end) break;

		if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) {
			cmd = *p++;
		} else if (cmd == 0) {
			break;					// data before any command
		} else if (cmd == 'M') {
			cmd = 'L';				// repeated M coordinates are L
		} else if (cmd == 'm') {
			cmd = 'l';
		}

		switch (cmd | 0x20) {
		case 'm': case 'l': case 't': want = 2; break;
		case 'h': case 'v':          want = 1; break;
		case 'c':                    want = 6; break;
		case 's': case 'q':          want = 4; break;
		case 'a':                    want = 7; break;
		case 'z':                    want = 0; break;
		default:                     return;	// unknown command
		}

		while (nv < want) {
			const char *q = num(p, end, &v[nv]);
			if (!q) return;			// short command: stop, keep what we have
			p = q; nv++;
		}

		{
			bool rel = (cmd >= 'a');
			float ox = rel ? ux : 0.0f, oy = rel ? uy : 0.0f;
			float px, py;

			switch (cmd | 0x20) {

			case 'm':
				ux = ox + v[0]; uy = oy + v[1];
				sx = ux; sy = uy;
				xf_apply(&c->xf, ux, uy, &px, &py);
				move_to(c, px, py);
				break;

			case 'l':
				ux = ox + v[0]; uy = oy + v[1];
				xf_apply(&c->xf, ux, uy, &px, &py);
				line_to(c, px, py);
				break;

			case 'h':
				ux = ox + v[0];
				xf_apply(&c->xf, ux, uy, &px, &py);
				line_to(c, px, py);
				break;

			case 'v':
				uy = oy + v[0];
				xf_apply(&c->xf, ux, uy, &px, &py);
				line_to(c, px, py);
				break;

			case 'c': {
				float c1x = ox + v[0], c1y = oy + v[1];
				float c2x = ox + v[2], c2y = oy + v[3];
				float ex  = ox + v[4], ey  = oy + v[5];
				float a1x, a1y, a2x, a2y, aex, aey;
				xf_apply(&c->xf, c1x, c1y, &a1x, &a1y);
				xf_apply(&c->xf, c2x, c2y, &a2x, &a2y);
				xf_apply(&c->xf, ex, ey, &aex, &aey);
				cubic(c, c->cx, c->cy, a1x, a1y, a2x, a2y, aex, aey, 0);
				rcx = c2x; rcy = c2y;
				ux = ex; uy = ey;
				break;
			}

			case 's': {
				// The first control point mirrors the previous curve's
				// second. After anything that is not a cubic, it is
				// the current point -- which is the rule people get
				// wrong and which shows as a kink.
				float c1x = ux, c1y = uy;
				float c2x = ox + v[0], c2y = oy + v[1];
				float ex  = ox + v[2], ey  = oy + v[3];
				float a1x, a1y, a2x, a2y, aex, aey;
				if ((prev | 0x20) == 'c' || (prev | 0x20) == 's') {
					c1x = 2.0f * ux - rcx;
					c1y = 2.0f * uy - rcy;
				}
				xf_apply(&c->xf, c1x, c1y, &a1x, &a1y);
				xf_apply(&c->xf, c2x, c2y, &a2x, &a2y);
				xf_apply(&c->xf, ex, ey, &aex, &aey);
				cubic(c, c->cx, c->cy, a1x, a1y, a2x, a2y, aex, aey, 0);
				rcx = c2x; rcy = c2y;
				ux = ex; uy = ey;
				break;
			}

			case 'q': {
				float c1x = ox + v[0], c1y = oy + v[1];
				float ex  = ox + v[2], ey  = oy + v[3];
				float a1x, a1y, aex, aey;
				xf_apply(&c->xf, c1x, c1y, &a1x, &a1y);
				xf_apply(&c->xf, ex, ey, &aex, &aey);
				quad(c, c->cx, c->cy, a1x, a1y, aex, aey);
				rcx = c1x; rcy = c1y;
				ux = ex; uy = ey;
				break;
			}

			case 't': {
				float c1x = ux, c1y = uy;
				float ex = ox + v[0], ey = oy + v[1];
				float a1x, a1y, aex, aey;
				if ((prev | 0x20) == 'q' || (prev | 0x20) == 't') {
					c1x = 2.0f * ux - rcx;
					c1y = 2.0f * uy - rcy;
				}
				xf_apply(&c->xf, c1x, c1y, &a1x, &a1y);
				xf_apply(&c->xf, ex, ey, &aex, &aey);
				quad(c, c->cx, c->cy, a1x, a1y, aex, aey);
				rcx = c1x; rcy = c1y;
				ux = ex; uy = ey;
				break;
			}

			case 'a': {
				// Radii are in USER space, so a non-uniform transform
				// would need the arc decomposed differently. The
				// common case -- uniform scale and translate -- is
				// handled by scaling the radii by the transform's
				// magnitude.
				float ex = ox + v[5], ey = oy + v[6];
				float aex, aey;
				float mx = sqrtf(c->xf.a * c->xf.a + c->xf.b * c->xf.b);
				float my = sqrtf(c->xf.c * c->xf.c + c->xf.d * c->xf.d);
				xf_apply(&c->xf, ex, ey, &aex, &aey);
				arc_to(c, c->cx, c->cy, v[0] * mx, v[1] * my, v[2],
					v[3] != 0.0f, v[4] != 0.0f, aex, aey);
				ux = ex; uy = ey;
				break;
			}

			case 'z':
				close_path(c);
				ux = sx; uy = sy;
				break;

			}

			(void)dx; (void)dy;

		}

		prev = cmd;

	}

	// A path left open is closed for filling.
	if (c->open && (c->cx != c->sx || c->cy != c->sy))
		add_edge(c, c->cx, c->cy, c->sx, c->sy);

}

// -- scanline fill --------------------------------------------------
//
// One sample per scanline, at its centre. No anti-aliasing: the
// target has two levels, so a coverage value has nowhere to go.
//
// Crossings are gathered per line, sorted, then paired according to
// the fill rule. Nonzero counts edge directions; even-odd toggles.
// Real files need both -- nonzero is the default, and even-odd is how
// a "hole" is usually drawn.

typedef struct { float x; int8_t dir; } cross_t;

static void sort_cross(cross_t *a, int n) {
	// Insertion sort: n is the number of edges crossing ONE scanline,
	// which is single digits for most drawings and rarely more than a
	// few dozen. A heap would cost more in code than it saves.
	int i, j;
	for (i = 1; i < n; i++) {
		cross_t t = a[i];
		for (j = i - 1; j >= 0 && a[j].x > t.x; j--) a[j + 1] = a[j];
		a[j + 1] = t;
	}
}

#define MAX_CROSS 128

static void fill_edges(z_svg_t *s, int n, bool evenodd, int level) {

	float ymin = 1e30f, ymax = -1e30f;
	int y, iy0, iy1;

	if (n <= 0) return;

	for (y = 0; y < n; y++) {
		if (s->edges[y].y0 < ymin) ymin = s->edges[y].y0;
		if (s->edges[y].y1 > ymax) ymax = s->edges[y].y1;
	}

	iy0 = (int)floorf(ymin);
	iy1 = (int)ceilf(ymax);

	if (iy0 < s->y) iy0 = s->y;
	if (iy1 > s->y + s->h - 1) iy1 = s->y + s->h - 1;

	for (y = iy0; y <= iy1; y++) {

		cross_t xs[MAX_CROSS];
		int nc = 0, i;
		float sy = (float)y + 0.5f;

		for (i = 0; i < n && nc < MAX_CROSS; i++) {

			const z_svg_edge_t *e = &s->edges[i];

			// Half-open in y: a vertex shared by two edges is counted
			// once, not twice. Closing this interval instead is the
			// classic cause of single-pixel holes at shape joins.
			if (sy < e->y0 || sy >= e->y1) continue;

			xs[nc].x = e->x0 + (e->x1 - e->x0) * (sy - e->y0) /
				(e->y1 - e->y0);
			xs[nc].dir = e->dir;
			nc++;

		}

		if (nc < 2) continue;

		sort_cross(xs, nc);

		{
			int wind = 0;

			for (i = 0; i < nc - 1; i++) {

				int inside;

				wind += evenodd ? 1 : xs[i].dir;
				inside = evenodd ? (wind & 1) : (wind != 0);

				if (!inside) continue;

				{
					int x0 = (int)ceilf(xs[i].x - 0.5f);
					int x1 = (int)ceilf(xs[i + 1].x - 0.5f) - 1;

					if (x0 < s->x) x0 = s->x;
					if (x1 > s->x + s->w - 1) x1 = s->x + s->w - 1;

					if (x1 >= x0 && s->sink.span)
						s->sink.span(s->sink.user, y, x0, x1, level);
				}

			}
		}

	}

}

// Strokes: the flattened outline, drawn as segments.
//
// Width is ignored -- every stroke is one pixel. On a 640x480
// monochrome display a two-pixel line is not twice as visible, it is
// twice as heavy, and the drawings this renderer is for (diagrams,
// logos) read better thin.
static void stroke_edges(z_svg_t *s, int from, int n) {

	int i;

	if (!s->sink.line) return;

	for (i = from; i < n; i++) {
		const z_svg_edge_t *e = &s->edges[i];
		s->sink.line(s->sink.user, (int)(e->x0 + 0.5f), (int)(e->y0 + 0.5f),
			(int)(e->x1 + 0.5f), (int)(e->y1 + 0.5f));
	}

}

// -- attributes -----------------------------------------------------

// Finds attribute `name` within one tag's text. Returns its value
// span, or false.
static bool attr(const char *p, const char *end, const char *name,
	const char **vs, const char **ve) {

	int nl = (int)strlen(name);

	while (p < end) {

		const char *k;
		char q;

		p = skip_ws(p, end);
		while (p < end && is_ws(*p)) p++;
		if (p >= end) return false;

		k = p;
		while (p < end && *p != '=' && !is_ws(*p) && *p != '>') p++;

		if (p >= end || *p != '=') {
			if (p < end) p++;
			continue;
		}

		p++;
		if (p >= end) return false;
		q = *p;
		if (q != '"' && q != '\'') { p++; continue; }
		p++;

		*vs = p;
		while (p < end && *p != q) p++;
		*ve = p;
		if (p < end) p++;

		if ((int)(*ve - *vs) >= 0 && (p - k) > nl &&
			!memcmp(k, name, (size_t)nl) &&
			(k[nl] == '=' || is_ws(k[nl])))
			return true;

	}

	return false;

}

static bool attr_num(const char *p, const char *end, const char *name,
	float *out) {
	const char *vs, *ve;
	if (!attr(p, end, name, &vs, &ve)) return false;
	return num(vs, ve, out) != NULL;
}

// "none" means do not paint. Anything else is painted, because this
// display has one ink and the distinction between colours is not one
// it can make.
// A property from a style="..." attribute.
//
// Not a CSS parser: a semicolon-separated list of `name:value`, which
// is all SVG presentation style is in practice.
//
// This is not optional. Inkscape -- and most real-world SVG -- writes
// EVERY paint into style= and none into presentation attributes. A
// renderer reading only `fill="..."` sees a document with no colours
// at all and paints everything solid, which is exactly how an
// illustration came out as one black silhouette.
static bool style_prop(const char *p, const char *end, const char *name,
	const char **vs, const char **ve) {

	const char *ss, *se, *q;
	int nl = (int)strlen(name);

	if (!attr(p, end, "style", &ss, &se)) return false;

	q = ss;
	while (q < se) {

		const char *k, *kend, *v;

		while (q < se && (is_ws(*q) || *q == ';')) q++;
		k = q;
		while (q < se && *q != ':' && *q != ';') q++;
		if (q >= se || *q != ':') break;
		kend = q;
		q++;

		v = q;
		while (q < se && *q != ';') q++;

		while (kend > k && is_ws(kend[-1])) kend--;
		while (v < q && is_ws(*v)) v++;

		if (kend - k == nl && !memcmp(k, name, (size_t)nl)) {
			*vs = v; *ve = q;
			return true;
		}

	}

	return false;

}

// style= first, presentation attribute second -- the CSS cascade's
// order, and the one that matters when a file has both.
static bool paint_get(const char *p, const char *end, const char *name,
	const char **vs, const char **ve) {
	if (style_prop(p, end, name, vs, ve)) return true;
	return attr(p, end, name, vs, ve);
}

// The fill colour, as a shading level.
//
// Luminance of #rgb / #rrggbb, or a few names. Anything unrecognised
// is solid: an unknown colour drawn as ink is visible, drawn as white
// is missing.
static int paint_level(const char *p, const char *end, const char *name) {

	const char *vs, *ve;
	int r, g, b, y, nd = 0, d[6];
	const char *q;

	if (!paint_get(p, end, name, &vs, &ve)) return Z_SVG_LEVELS - 1;

	if (ve - vs == 4 && !memcmp(vs, "none", 4)) return 0;
	if (ve - vs == 5 && !memcmp(vs, "white", 5)) return 0;
	if (ve - vs == 5 && !memcmp(vs, "black", 5)) return Z_SVG_LEVELS - 1;

	if (ve <= vs || *vs != '#') return Z_SVG_LEVELS - 1;

	for (q = vs + 1; q < ve && nd < 6; q++) {
		int v;
		if (*q >= '0' && *q <= '9') v = *q - '0';
		else if (*q >= 'a' && *q <= 'f') v = *q - 'a' + 10;
		else if (*q >= 'A' && *q <= 'F') v = *q - 'A' + 10;
		else break;
		d[nd++] = v;
	}

	if (nd == 3) { r = d[0] * 17; g = d[1] * 17; b = d[2] * 17; }
	else if (nd == 6) {
		r = d[0] * 16 + d[1]; g = d[2] * 16 + d[3]; b = d[4] * 16 + d[5];
	} else return Z_SVG_LEVELS - 1;

	// Rec.601 luma, as zimg.c uses. Inverted: dark paint is more ink.
	y = (77 * r + 150 * g + 29 * b) >> 8;

	return (255 - y) * (Z_SVG_LEVELS - 1) / 255;

}

static bool paint_is_none(const char *p, const char *end, const char *name,
	bool dflt) {
	const char *vs, *ve;
	if (!paint_get(p, end, name, &vs, &ve)) return dflt;
	if (ve - vs == 4 && !memcmp(vs, "none", 4)) return false;
	return true;
}

static bool rule_is_evenodd(const char *p, const char *end) {
	const char *vs, *ve;
	if (!paint_get(p, end, "fill-rule", &vs, &ve)) return false;
	return (ve - vs == 7) && !memcmp(vs, "evenodd", 7);
}

// -- shapes ---------------------------------------------------------

static void shape_rect(ctx_t *c, const char *a, const char *ae) {

	float x = 0, y = 0, w = 0, h = 0, rx = 0, ry = 0;
	float px, py;

	attr_num(a, ae, "x", &x);
	attr_num(a, ae, "y", &y);
	if (!attr_num(a, ae, "width", &w)) return;
	if (!attr_num(a, ae, "height", &h)) return;
	if (w <= 0 || h <= 0) return;

	if (!attr_num(a, ae, "rx", &rx)) rx = 0;
	if (!attr_num(a, ae, "ry", &ry)) ry = rx;
	if (rx > w * 0.5f) rx = w * 0.5f;
	if (ry > h * 0.5f) ry = h * 0.5f;

	if (rx <= 0 || ry <= 0) {
		xf_apply(&c->xf, x, y, &px, &py);         move_to(c, px, py);
		xf_apply(&c->xf, x + w, y, &px, &py);     line_to(c, px, py);
		xf_apply(&c->xf, x + w, y + h, &px, &py); line_to(c, px, py);
		xf_apply(&c->xf, x, y + h, &px, &py);     line_to(c, px, py);
		close_path(c);
		return;
	}

	// Rounded: four sides and four quarter-arcs.
	{
		float mx = sqrtf(c->xf.a * c->xf.a + c->xf.b * c->xf.b);
		float my = sqrtf(c->xf.c * c->xf.c + c->xf.d * c->xf.d);
		float arx = rx * mx, ary = ry * my;
		float ex, ey;

		xf_apply(&c->xf, x + rx, y, &px, &py); move_to(c, px, py);
		xf_apply(&c->xf, x + w - rx, y, &px, &py); line_to(c, px, py);
		xf_apply(&c->xf, x + w, y + ry, &ex, &ey);
		arc_to(c, c->cx, c->cy, arx, ary, 0, 0, 1, ex, ey);
		xf_apply(&c->xf, x + w, y + h - ry, &px, &py); line_to(c, px, py);
		xf_apply(&c->xf, x + w - rx, y + h, &ex, &ey);
		arc_to(c, c->cx, c->cy, arx, ary, 0, 0, 1, ex, ey);
		xf_apply(&c->xf, x + rx, y + h, &px, &py); line_to(c, px, py);
		xf_apply(&c->xf, x, y + h - ry, &ex, &ey);
		arc_to(c, c->cx, c->cy, arx, ary, 0, 0, 1, ex, ey);
		xf_apply(&c->xf, x, y + ry, &px, &py); line_to(c, px, py);
		xf_apply(&c->xf, x + rx, y, &ex, &ey);
		arc_to(c, c->cx, c->cy, arx, ary, 0, 0, 1, ex, ey);
		close_path(c);
	}

}

static void shape_ellipse(ctx_t *c, const char *a, const char *ae,
	bool circle) {

	float cx = 0, cy = 0, rx = 0, ry = 0;
	float mx, my, arx, ary, px, py, ex, ey;

	attr_num(a, ae, "cx", &cx);
	attr_num(a, ae, "cy", &cy);

	if (circle) {
		if (!attr_num(a, ae, "r", &rx)) return;
		ry = rx;
	} else {
		if (!attr_num(a, ae, "rx", &rx)) return;
		if (!attr_num(a, ae, "ry", &ry)) return;
	}

	if (rx <= 0 || ry <= 0) return;

	mx = sqrtf(c->xf.a * c->xf.a + c->xf.b * c->xf.b);
	my = sqrtf(c->xf.c * c->xf.c + c->xf.d * c->xf.d);
	arx = rx * mx; ary = ry * my;

	// Two half-arcs: a single 360-degree arc is degenerate, because
	// its start and end points coincide and the centre is then
	// undefined.
	xf_apply(&c->xf, cx - rx, cy, &px, &py); move_to(c, px, py);
	xf_apply(&c->xf, cx + rx, cy, &ex, &ey);
	arc_to(c, c->cx, c->cy, arx, ary, 0, 1, 1, ex, ey);
	xf_apply(&c->xf, cx - rx, cy, &ex, &ey);
	arc_to(c, c->cx, c->cy, arx, ary, 0, 1, 1, ex, ey);
	close_path(c);

}

static void shape_points(ctx_t *c, const char *a, const char *ae,
	bool close_it) {

	const char *vs, *ve, *p;
	bool first = true;

	if (!attr(a, ae, "points", &vs, &ve)) return;

	p = vs;
	for (;;) {
		float ux, uy, px, py;
		const char *q = num(p, ve, &ux);
		if (!q) break;
		q = num(q, ve, &uy);
		if (!q) break;
		p = q;
		xf_apply(&c->xf, ux, uy, &px, &py);
		if (first) { move_to(c, px, py); first = false; }
		else line_to(c, px, py);
	}

	if (!first && close_it) close_path(c);

}

static void shape_line(ctx_t *c, const char *a, const char *ae) {
	float x1 = 0, y1 = 0, x2 = 0, y2 = 0, px, py;
	attr_num(a, ae, "x1", &x1); attr_num(a, ae, "y1", &y1);
	attr_num(a, ae, "x2", &x2); attr_num(a, ae, "y2", &y2);
	xf_apply(&c->xf, x1, y1, &px, &py); move_to(c, px, py);
	xf_apply(&c->xf, x2, y2, &px, &py); line_to(c, px, py);
}

// -- the document walk ----------------------------------------------

static bool tag_is(const char *n, int len, const char *name) {
	int l = (int)strlen(name);
	return len == l && !memcmp(n, name, (size_t)l);
}

int z_svg_render(z_svg_t *s) {

	const char *p, *end;
	ctx_t c;
	z_svg_xf_t stack[MAX_XF], base;
	int depth = 0;
	bool got_svg = false;
	float vb[4] = { 0, 0, 0, 0 };
	bool have_vb = false;

	if (!s || !s->doc || !s->edges || s->max_edges < 16)
		return Z_SVG_E_FORMAT;
	if (s->w <= 0 || s->h <= 0) return Z_SVG_E_FORMAT;

	p = s->doc; end = s->doc + s->len;

	s->n_edges = 0;
	s->n_shapes = 0;

	if (!z_svg_sniff(s->doc, s->len)) return Z_SVG_E_FORMAT;

	memset(&c, 0, sizeof(c));
	c.s = s;
	xf_identity(&base);
	xf_identity(&c.xf);

	while (p < end) {

		const char *tag, *tend, *aend;
		int tlen;
		bool selfclose = false, closing = false;

		while (p < end && *p != '<') p++;
		if (p >= end) break;
		p++;

		if (p < end && *p == '/') { closing = true; p++; }

		// Comments, declarations and CDATA: skipped wholesale.
		if (p < end && (*p == '!' || *p == '?')) {
			while (p < end && *p != '>') p++;
			if (p < end) p++;
			continue;
		}

		tag = p;
		while (p < end && !is_ws(*p) && *p != '>' && *p != '/') p++;
		tlen = (int)(p - tag);

		// The attribute region: from here to the closing angle.
		aend = p;
		{
			const char *q = p;
			char quote = 0;
			while (q < end) {
				if (quote) { if (*q == quote) quote = 0; }
				else if (*q == '"' || *q == '\'') quote = *q;
				else if (*q == '>') break;
				q++;
			}
			tend = q;
			if (tend > aend && tend[-1] == '/') selfclose = true;
		}

		if (closing) {
			if (tag_is(tag, tlen, "g") && depth > 0) {
				depth--;
				c.xf = depth > 0 ? stack[depth - 1] : base;
			}
			p = (tend < end) ? tend + 1 : end;
			continue;
		}

		if (tag_is(tag, tlen, "svg")) {

			const char *vs, *ve;
			float w = 0, h = 0;
			float sc, ox, oy;

			if (got_svg) { p = tend; continue; }
			got_svg = true;

			if (attr(aend, tend, "viewBox", &vs, &ve)) {
				const char *q = vs;
				int i;
				for (i = 0; i < 4; i++) {
					q = num(q, ve, &vb[i]);
					if (!q) break;
				}
				have_vb = (i == 4 && vb[2] > 0 && vb[3] > 0);
			}

			attr_num(aend, tend, "width", &w);
			attr_num(aend, tend, "height", &h);

			if (!have_vb) {
				// No viewBox: width and height define the coordinate
				// space directly.
				if (w <= 0 || h <= 0) return Z_SVG_E_FORMAT;
				vb[0] = 0; vb[1] = 0; vb[2] = w; vb[3] = h;
			}

			s->src_w = vb[2];
			s->src_h = vb[3];

			// Fit, preserving aspect ratio and centring -- the
			// default preserveAspectRatio ("xMidYMid meet"). Other
			// values are rare and a wrongly stretched diagram is
			// worse than a correctly letterboxed one.
			{
				float sx = (float)s->w / vb[2];
				float sy = (float)s->h / vb[3];
				sc = sx < sy ? sx : sy;
				ox = (float)s->x + ((float)s->w - vb[2] * sc) * 0.5f;
				oy = (float)s->y + ((float)s->h - vb[3] * sc) * 0.5f;
			}

			base.a = sc; base.b = 0; base.c = 0; base.d = sc;
			base.e = ox - vb[0] * sc;
			base.f = oy - vb[1] * sc;
			c.xf = base;

			p = tend;
			continue;

		}

		if (!got_svg) { p = tend; continue; }

		if (tag_is(tag, tlen, "g")) {

			const char *vs, *ve;
			z_svg_xf_t local, cur = c.xf;

			if (attr(aend, tend, "transform", &vs, &ve)) {
				parse_transform(vs, ve, &local);
				xf_mul(&cur, &cur, &local);
			}

			if (depth < MAX_XF) {
				stack[depth++] = cur;
				c.xf = cur;
			} else {
				// Deeper than the stack: the group's transform is
				// ignored rather than the file rejected, which
				// degrades a drawing instead of losing it.
				;
			}

			// A self-closing <g/> opens and closes at once.
			if (selfclose && depth > 0) {
				depth--;
				c.xf = depth > 0 ? stack[depth - 1] : base;
			}

			p = tend;
			continue;

		}

		// -- a shape --
		{
			z_svg_xf_t saved = c.xf, local;
			const char *vs, *ve;
			int start = c.n;
			bool filled, stroked, eo;
			int level;

			if (!tag_is(tag, tlen, "path") && !tag_is(tag, tlen, "rect") &&
				!tag_is(tag, tlen, "circle") &&
				!tag_is(tag, tlen, "ellipse") &&
				!tag_is(tag, tlen, "line") &&
				!tag_is(tag, tlen, "polyline") &&
				!tag_is(tag, tlen, "polygon")) {
				p = tend;
				continue;
			}

			if (attr(aend, tend, "transform", &vs, &ve)) {
				parse_transform(vs, ve, &local);
				xf_mul(&c.xf, &c.xf, &local);
			}

			c.n = 0;
			c.open = false;
			c.cx = c.cy = c.sx = c.sy = 0;
			(void)start;

			// A <line> or <polyline> has no interior; everything else
			// is filled unless it says otherwise.
			stroked = paint_is_none(aend, tend, "stroke", false);
			filled  = paint_is_none(aend, tend, "fill", true);
			eo      = rule_is_evenodd(aend, tend);
			level   = paint_level(aend, tend, "fill");

			// A fill that resolves to white is not drawn: on paper it
			// would cover what is beneath, and here there is nothing
			// to cover with.
			if (level == 0) filled = false;

			if (tag_is(tag, tlen, "path")) {
				if (attr(aend, tend, "d", &vs, &ve))
					path_data(&c, vs, ve);
			} else if (tag_is(tag, tlen, "rect")) {
				shape_rect(&c, aend, tend);
			} else if (tag_is(tag, tlen, "circle")) {
				shape_ellipse(&c, aend, tend, true);
			} else if (tag_is(tag, tlen, "ellipse")) {
				shape_ellipse(&c, aend, tend, false);
			} else if (tag_is(tag, tlen, "line")) {
				shape_line(&c, aend, tend);
				filled = false; stroked = true;
			} else if (tag_is(tag, tlen, "polyline")) {
				shape_points(&c, aend, tend, false);
				filled = paint_is_none(aend, tend, "fill", false);
				stroked = true;
			} else {
				shape_points(&c, aend, tend, true);
			}

			if (c.overflow) return Z_SVG_E_TOOCOMPLEX;

			if (c.n > s->n_edges) s->n_edges = c.n;

			if (filled) fill_edges(s, c.n, eo, level);
			if (stroked) stroke_edges(s, 0, c.n);

			s->n_shapes++;
			c.xf = saved;
			p = tend;
			continue;
		}

	}

	if (!got_svg) return Z_SVG_E_FORMAT;

	return Z_SVG_OK;

}

// -- rendering into a bitmap ----------------------------------------

typedef struct {
	uint32_t	*bits;
	int			wpl;
	int			w, h;
} bmp_t;

static void bmp_px(bmp_t *b, int x, int y) {
	if (x < 0 || y < 0 || x >= b->w || y >= b->h) return;
	b->bits[(long)y * b->wpl + (x >> 5)] |= 1u << (x & 31);
}

// An ordered (Bayer) dither, so a shaded region has a stable pattern
// that does not crawl when the drawing is redrawn or scrolled.
//
// Error diffusion would look better on a photograph and is wrong
// here: shapes are painted independently, so error from one would
// leak into the next and a flat region would come out streaked.
static const uint8_t bayer4[4][4] = {
	{  0,  8,  2, 10 },
	{ 12,  4, 14,  6 },
	{  3, 11,  1,  9 },
	{ 15,  7, 13,  5 },
};

static void bmp_span(void *user, int y, int x0, int x1, int level) {

	bmp_t *b = (bmp_t *)user;
	int x, thr;

	if (y < 0 || y >= b->h || level <= 0) return;
	if (x0 < 0) x0 = 0;
	if (x1 >= b->w) x1 = b->w - 1;

	// Solid is solid: the common case, and a dither at full ink would
	// punch holes in it.
	if (level >= Z_SVG_LEVELS - 1) {
		for (x = x0; x <= x1; x++) bmp_px(b, x, y);
		return;
	}

	thr = level * 16 / (Z_SVG_LEVELS - 1);

	for (x = x0; x <= x1; x++)
		if (bayer4[y & 3][x & 3] < thr) bmp_px(b, x, y);

}

static void bmp_line(void *user, int x0, int y0, int x1, int y1) {

	// Bresenham. Not the hardware rasterizer: this writes into an
	// off-screen bitmap, and the GPU draws to VRAM.
	bmp_t *b = (bmp_t *)user;
	int dx = x1 > x0 ? x1 - x0 : x0 - x1;
	int dy = y1 > y0 ? y1 - y0 : y0 - y1;
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx - dy;

	for (;;) {
		bmp_px(b, x0, y0);
		if (x0 == x1 && y0 == y1) break;
		{
			int e2 = 2 * err;
			if (e2 > -dy) { err -= dy; x0 += sx; }
			if (e2 <  dx) { err += dx; y0 += sy; }
		}
	}

}

int z_svg_render_bitmap(z_svg_t *s, uint32_t *bits, int wpl) {

	bmp_t b;

	if (!s || !bits || wpl <= 0) return Z_SVG_E_FORMAT;

	b.bits = bits;
	b.wpl = wpl;
	b.w = s->x + s->w;
	b.h = s->y + s->h;

	s->sink.span = bmp_span;
	s->sink.line = bmp_line;
	s->sink.user = &b;

	return z_svg_render(s);

}
