/*
 * mausrun3d -- the models (models.h).
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Built from a few shapes -- boxes, wedges, pyramids, prisms -- each
 * face with its own normal (flat shading, lit along its colour's ramp).
 * A few dozen triangles a model: T2 transforms what it is sent whether
 * it is seen or not (docs/mausrun3d.md, "Frame budget").
 */

#include <stdint.h>
#include <stdbool.h>

#include "zgl.h"
#include "world.h"
#include "models.h"

uint32_t md_list;
uint16_t md_tris[MD_COUNT];

static int ntri;

static void material(int ramp) {
	GLint m[3];
	m[0] = ramp + 3;
	m[1] = m[2] = ramp + 15;
	glMaterialiv(GL_FRONT_AND_BACK, GL_COLOR_INDEXES, m);
}

/* one colour whatever the light */
static void flat_colour(int c) {
	GLint m[3];
	m[0] = m[1] = m[2] = c;
	glMaterialiv(GL_FRONT_AND_BACK, GL_COLOR_INDEXES, m);
}

/* a triangle, its normal from its corners (counter-clockwise: the front) */
static void tri3(int ax, int ay, int az, int bx, int by, int bz, int cx, int cy, int cz) {
	int32_t ux = bx - ax, uy = by - ay, uz = bz - az;
	int32_t wx = cx - ax, wy = cy - ay, wz = cz - az;
	glNormal3i(uy * wz - uz * wy, uz * wx - ux * wz, ux * wy - uy * wx);
	glVertex3i(ax, ay, az);
	glVertex3i(bx, by, bz);
	glVertex3i(cx, cy, cz);
	ntri++;
}

static void quad(const int *a, const int *b, const int *c, const int *d) {
	tri3(a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]);
	tri3(a[0], a[1], a[2], c[0], c[1], c[2], d[0], d[1], d[2]);
}

/* a box x0..x1, y0..y1, z0..z1: six faces */
static void box(int x0, int y0, int z0, int x1, int y1, int z1) {
	int p[8][3];
	int i;
	for (i = 0; i < 8; i++) {
		p[i][0] = (i & 1) ? x1 : x0;
		p[i][1] = (i & 2) ? y1 : y0;
		p[i][2] = (i & 4) ? z1 : z0;
	}
	/* corners: bit 0 x, bit 1 y, bit 2 z */
	quad(p[2], p[6], p[7], p[3]);     /* top (+y) */
	quad(p[0], p[1], p[5], p[4]);     /* bottom */
	quad(p[1], p[3], p[7], p[5]);     /* +x */
	quad(p[0], p[4], p[6], p[2]);     /* -x */
	quad(p[4], p[5], p[7], p[6]);     /* +z */
	quad(p[0], p[2], p[3], p[1]);     /* -z */
}

/* a box tapering to its top (a frustum of a pyramid): top half-size t */
static void taper(int cx, int y0, int cz, int hx, int hz, int y1, int tx, int tz) {
	int b[4][3] = { { cx - hx, y0, cz - hz }, { cx + hx, y0, cz - hz }, { cx + hx, y0, cz + hz }, { cx - hx, y0, cz + hz } };
	int t[4][3] = { { cx - tx, y1, cz - tz }, { cx + tx, y1, cz - tz }, { cx + tx, y1, cz + tz }, { cx - tx, y1, cz + tz } };
	int i;
	quad(t[0], t[3], t[2], t[1]);
	quad(b[0], b[1], b[2], b[3]);
	for (i = 0; i < 4; i++) {
		int j = (i + 1) & 3;
		quad(b[i], t[i], t[j], b[j]);
	}
}

/* a pyramid: a square base round (cx, y0, cz), its point at (px, py, pz) */
static void pyramid(int cx, int y0, int cz, int h, int px, int py, int pz) {
	int b[4][3] = { { cx - h, y0, cz - h }, { cx + h, y0, cz - h }, { cx + h, y0, cz + h }, { cx - h, y0, cz + h } };
	int i;
	for (i = 0; i < 4; i++) {
		int j = (i + 1) & 3;
		tri3(b[i][0], b[i][1], b[i][2], px, py, pz, b[j][0], b[j][1], b[j][2]);
	}
	quad(b[0], b[1], b[2], b[3]);
}

/* an upright prism of n sides round the y axis, radius r, y0..y1 */
static void prism(int n, int r, int y0, int y1, bool caps) {
	int i;
	for (i = 0; i < n; i++) {
		uint32_t a0 = (uint32_t)(i * 65536 / n), a1 = (uint32_t)((i + 1) * 65536 / n);
		int x0 = (int)((zglCos(a0) * r) >> 16), z0 = (int)((zglSin(a0) * r) >> 16);
		int x1 = (int)((zglCos(a1) * r) >> 16), z1 = (int)((zglSin(a1) * r) >> 16);
		tri3(x0, y0, z0, x0, y1, z0, x1, y1, z1);
		tri3(x0, y0, z0, x1, y1, z1, x1, y0, z1);
		if (caps) {
			tri3(0, y1, 0, x1, y1, z1, x0, y1, z0);
			tri3(0, y0, 0, x0, y0, z0, x1, y0, z1);
		}
	}
}

/* a flat triangle seen from both sides (ears, fins, feathers) */
static void card(int ax, int ay, int az, int bx, int by, int bz, int cx, int cy, int cz) {
	tri3(ax, ay, az, bx, by, bz, cx, cy, cz);
	tri3(ax, ay, az, cx, cy, cz, bx, by, bz);
}

static void begin(int m) {
	glNewList(md_list + (GLuint)m, GL_COMPILE);
	ntri = 0;
}

static void end(int m) {
	glEndList();
	md_tris[m] = (uint16_t)ntri;
}

/* -- the mouse: grey, pink ears and nose, a tail; mausrun's run frames
 * are its feet, here two legs' worth of stride -- */
static void mouse(int stride) {
	material(W3_R_MOUSE);
	glBegin(GL_TRIANGLES);
	taper(0, 2, 1, 4, 6, 9, 3, 4);                  /* the body */
	taper(0, 5, -7, 3, 3, 10, 1, 1);                /* the head, looking ahead */
	tri3(-2, 6, -9, 0, 9, -9, 2, 6, -9);            /* its face, to the nose */
	tri3(-2, 6, -10, 0, 7, -14, 2, 6, -10);
	tri3(-2, 6, -10, 2, 6, -10, 0, 7, -14);
	/* feet: forward and back by the stride */
	box(-4, 0, -3 - stride, -2, 2, -1 - stride);
	box(2, 0, -3 + stride, 4, 2, -1 + stride);
	box(-4, 0, 4 + stride, -2, 2, 6 + stride);
	box(2, 0, 4 - stride, 4, 2, 6 - stride);
	glEnd();
	material(W3_R_PINK);
	glBegin(GL_TRIANGLES);
	card(-2, 9, -6, -6, 14, -5, -5, 9, -5);         /* ears */
	card(2, 9, -6, 5, 9, -5, 6, 14, -5);
	tri3(-1, 7, -14, 0, 8, -15, 1, 7, -14);         /* the nose */
	glEnd();
	glBegin(GL_LINES);                              /* the tail */
	glVertex3i(0, 4, 7); glVertex3i(0, 6, 12);
	glVertex3i(0, 6, 12); glVertex3i(2, 9, 16);
	glEnd();
}

/* -- a cat: orange, bigger than the mouse; napping, curled up -- */
static void cat(int stride, bool nap) {
	material(W3_R_CAT);
	glBegin(GL_TRIANGLES);
	if (nap) {
		taper(0, 0, 0, 8, 7, 7, 5, 4);              /* curled */
		taper(-5, 3, -5, 4, 4, 9, 3, 3);            /* the head, tucked in */
		card(-8, 8, -6, -6, 12, -5, -4, 8, -6);
		card(-2, 8, -6, -1, 12, -5, 1, 8, -6);
	} else {
		taper(0, 4, 2, 5, 9, 13, 4, 7);             /* the body */
		taper(0, 10, -8, 5, 4, 18, 4, 3);           /* the head */
		card(-5, 17, -8, -4, 22, -8, -1, 17, -8);   /* ears */
		card(1, 17, -8, 4, 22, -8, 5, 17, -8);
		box(-5, 0, -6 - stride, -2, 5, -3 - stride);    /* legs */
		box(2, 0, -6 + stride, 5, 5, -3 + stride);
		box(-5, 0, 7 + stride, -2, 5, 10 + stride);
		box(2, 0, 7 - stride, 5, 5, 10 - stride);
	}
	glEnd();
	material(W3_R_ROCK);                            /* eyes */
	glBegin(GL_TRIANGLES);
	if (!nap) {
		tri3(-3, 15, -12, -2, 13, -12, -1, 15, -12);
		tri3(1, 15, -12, 2, 13, -12, 3, 15, -12);
	}
	glEnd();
	glBegin(GL_LINES);                              /* the tail */
	if (nap) { glVertex3i(6, 3, 4); glVertex3i(2, 2, 9); glVertex3i(2, 2, 9); glVertex3i(-4, 2, 8); }
	else { glVertex3i(0, 11, 11); glVertex3i(0, 16, 15); glVertex3i(0, 16, 15); glVertex3i(2, 20, 15); }
	glEnd();
}

/* -- a frog: green, wide, eyes on top; hopping, legs out -- */
static void frog(bool hop) {
	material(W3_R_GRASS);
	glBegin(GL_TRIANGLES);
	taper(0, 2, 0, 7, 6, 8, 5, 4);
	box(-5, 8, -5, -2, 11, -2);                     /* eyes */
	box(2, 8, -5, 5, 11, -2);
	if (hop) { box(-9, 0, 4, -6, 3, 10); box(6, 0, 4, 9, 3, 10); }
	else { box(-9, 0, -2, -6, 3, 4); box(6, 0, -2, 9, 3, 4); }
	glEnd();
}

/* -- an owl: brown, round, big eyes, wings out -- */
static void owl(void) {
	material(W3_R_EARTH);
	glBegin(GL_TRIANGLES);
	taper(0, 0, 0, 5, 5, 12, 4, 4);
	pyramid(0, 12, 0, 4, 0, 16, 0);
	card(-5, 10, 0, -14, 6, 2, -5, 4, 0);           /* wings */
	card(5, 10, 0, 5, 4, 0, 14, 6, 2);
	glEnd();
	material(W3_R_QBOX);
	glBegin(GL_TRIANGLES);
	box(-4, 9, -6, -1, 12, -5);
	box(1, 9, -6, 4, 12, -5);
	glEnd();
}

/* -- a mouse hole: an arch in a little bank of earth, dark inside -- */
static void hole(bool lit) {
	material(W3_R_EARTH);
	glBegin(GL_TRIANGLES);
	box(-8, 0, -2, -4, 12, 2);
	box(4, 0, -2, 8, 12, 2);
	box(-8, 12, -2, 8, 15, 2);
	glEnd();
	flat_colour(W3_C_SHADOW);                       /* the dark of the hole */
	glBegin(GL_QUADS);
	glVertex3i(-4, 0, 0); glVertex3i(4, 0, 0); glVertex3i(4, 12, 0); glVertex3i(-4, 12, 0);
	glVertex3i(-4, 0, 0); glVertex3i(-4, 12, 0); glVertex3i(4, 12, 0); glVertex3i(4, 0, 0);
	glEnd();
	ntri += 4;
	if (lit) {
		material(W3_R_CHEESE);
		glBegin(GL_TRIANGLES);
		taper(0, 0, -3, 3, 2, 4, 1, 1);
		glEnd();
	}
}

/* -- the cheese, a wedge; the wheel, a big round of it -- */
static void cheese(void) {
	static const int P[6][3] = {
		{ -5, 0, -4 }, { 5, 0, -4 }, { 0, 0, 6 }, { -5, 6, -4 }, { 5, 6, -4 }, { 0, 6, 6 }
	};
	material(W3_R_CHEESE);
	glBegin(GL_TRIANGLES);
	tri3(P[3][0], P[3][1], P[3][2], P[5][0], P[5][1], P[5][2], P[4][0], P[4][1], P[4][2]);
	tri3(P[0][0], P[0][1], P[0][2], P[1][0], P[1][1], P[1][2], P[2][0], P[2][1], P[2][2]);
	quad(P[0], P[3], P[4], P[1]);
	quad(P[1], P[4], P[5], P[2]);
	quad(P[2], P[5], P[3], P[0]);
	glEnd();
}

static void wheel(void) {
	material(W3_R_CHEESE);
	glBegin(GL_TRIANGLES);
	prism(10, 14, 0, 12, true);
	glEnd();
	material(W3_R_QBOX);                            /* a wedge cut out */
	glBegin(GL_TRIANGLES);
	box(-3, 12, -3, 3, 14, 3);
	glEnd();
}

/* -- a star: five points, thick -- */
static void star(void) {
	int i;
	material(W3_R_QBOX);
	glBegin(GL_TRIANGLES);
	for (i = 0; i < 5; i++) {
		uint32_t a = (uint32_t)(i * 65536 / 5), b = a + 6554, c = a - 6554;
		int px = (int)((zglSin(a) * 9) >> 16), py = 9 + (int)((zglCos(a) * 9) >> 16);
		int bx = (int)((zglSin(b) * 4) >> 16), by = 9 + (int)((zglCos(b) * 4) >> 16);
		int cx = (int)((zglSin(c) * 4) >> 16), cy = 9 + (int)((zglCos(c) * 4) >> 16);
		tri3(0, 9, -2, cx, cy, 0, px, py, 0);
		tri3(0, 9, -2, px, py, 0, bx, by, 0);
		tri3(0, 9, 2, px, py, 0, cx, cy, 0);
		tri3(0, 9, 2, bx, by, 0, px, py, 0);
	}
	glEnd();
}

/* -- a heart: red, two lobes and a point -- */
static void heart(void) {
	material(W3_R_RED);
	glBegin(GL_TRIANGLES);
	pyramid(-3, 8, 0, 3, -3, 13, 0);
	pyramid(3, 8, 0, 3, 3, 13, 0);
	taper(0, 1, 0, 1, 1, 8, 6, 3);
	glEnd();
}

/* the power-ups: a little sign each, floating */
static void gem(int ramp) {
	material(ramp);
	glBegin(GL_TRIANGLES);
	pyramid(0, 7, 0, 4, 0, 13, 0);
	pyramid(0, 7, 0, 4, 0, 1, 0);
	glEnd();
}

static void feather(void) {
	material(W3_R_MOUSE);
	glBegin(GL_TRIANGLES);
	card(0, 1, 0, -4, 10, 0, 2, 15, 0);
	card(0, 1, 0, 2, 15, 0, 5, 9, 0);
	glEnd();
}

static void bell(void) {
	material(W3_R_QBOX);
	glBegin(GL_TRIANGLES);
	taper(0, 2, 0, 6, 6, 10, 2, 2);
	box(-1, 10, -1, 1, 13, 1);
	glEnd();
}

static void shoes(void) {
	material(W3_R_RED);
	glBegin(GL_TRIANGLES);
	box(-6, 2, -5, -1, 6, 4);
	box(1, 2, -5, 6, 6, 4);
	glEnd();
}

/* dizzy: three little stars round, as lines */
static void dizzy(void) {
	int i;
	flat_colour(W3_R_QBOX + 15);
	glNormal3i(0, 1, 0);
	glBegin(GL_LINES);
	for (i = 0; i < 3; i++) {
		uint32_t a = (uint32_t)(i * 65536 / 3);
		int x = (int)((zglSin(a) * 8) >> 16), z = (int)((zglCos(a) * 8) >> 16);
		glVertex3i(x - 2, 0, z); glVertex3i(x + 2, 0, z);
		glVertex3i(x, -2, z); glVertex3i(x, 2, z);
	}
	glEnd();
}

void md_make(void) {
	md_list = glGenLists(MD_COUNT);
	if (!md_list) return;
	begin(MD_MOUSE); mouse(0); end(MD_MOUSE);
	begin(MD_MOUSE_STEP); mouse(2); end(MD_MOUSE_STEP);
	begin(MD_SHADOW);
	{
		static const int8_t o[8][2] = {
			{ 0, -6 }, { 4, -4 }, { 6, 0 }, { 4, 4 }, { 0, 6 }, { -4, 4 }, { -6, 0 }, { -4, -4 }
		};
		int i;
		glIndexi(W3_C_SHADOW);
		glBegin(GL_TRIANGLE_FAN);
		glVertex3i(0, 0, 0);
		for (i = 8; i >= 0; i--) glVertex3i(o[i & 7][0], 0, o[i & 7][1]);
		glEnd();
		ntri = 8;
	}
	end(MD_SHADOW);
	begin(MD_CHEESE); cheese(); end(MD_CHEESE);
	begin(MD_CAT); cat(0, false); end(MD_CAT);
	begin(MD_CAT_STEP); cat(2, false); end(MD_CAT_STEP);
	begin(MD_CAT_NAP); cat(0, true); end(MD_CAT_NAP);
	begin(MD_FROG); frog(false); end(MD_FROG);
	begin(MD_FROG_HOP); frog(true); end(MD_FROG_HOP);
	begin(MD_OWL); owl(); end(MD_OWL);
	begin(MD_HOLE); hole(false); end(MD_HOLE);
	begin(MD_HOLE_LIT); hole(true); end(MD_HOLE_LIT);
	begin(MD_WHEEL); wheel(); end(MD_WHEEL);
	begin(MD_STAR); star(); end(MD_STAR);
	begin(MD_HEART); heart(); end(MD_HEART);
	begin(MD_FEATHER); feather(); end(MD_FEATHER);
	begin(MD_BUBBLE); gem(W3_R_WATER); end(MD_BUBBLE);
	begin(MD_MAGNET); gem(W3_R_RED); end(MD_MAGNET);
	begin(MD_BELL); bell(); end(MD_BELL);
	begin(MD_SHOES); shoes(); end(MD_SHOES);
	begin(MD_DIZZY); dizzy(); end(MD_DIZZY);
}
