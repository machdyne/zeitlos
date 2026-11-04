/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * framebench -- instructions a frame through ZGL_FRAMES, in the
 * simulator (whose instret counts): a lit smooth sphere of 384
 * triangles filling a third of the picture, a flat-shaded cube and a
 * ring of 48 lines, as sw/apps/gldemo draws them. The arcade runtime's
 * calls are stubs here, so what is counted is zgl's own work: the
 * front end, rasterising, and the strips' conversion to planes or to
 * a 1 bpp dither; the blits themselves are the GPU's (ar_plane).
 * make -C sw/common/zgl bench. docs/zgl.md, "Cost".
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "zgl.h"
#include "zgl_int.h"

static bool colour = true;
static int colours = 256;
static uint16_t pal[256];
static uint32_t n_blit;

bool ar_color(void) { return colour; }
int ar_colours(void) { return colours; }
uint16_t ar_pal_get(int i) { return pal[i & 255]; }
void ar_pal(int i, uint16_t c) { pal[i & 255] = c; }
void ar_plane_line(int k, int x0, int y0, int x1, int y1, int op) { (void)k; (void)x0; (void)y0; (void)x1; (void)y1; (void)op; }
void ar_plane(int k, const void *bits, int stride, int sx, int sy, int x, int y, int w, int h) {
	(void)k; (void)bits; (void)stride; (void)sx; (void)sy; (void)x; (void)y; (void)w; (void)h; n_blit++;
}
void ar_bitmap_opaque(const void *bits, int stride, int sx, int sy, int x, int y, int w, int h) {
	(void)bits; (void)stride; (void)sx; (void)sy; (void)x; (void)y; (void)w; (void)h; n_blit++;
}

static inline uint32_t ir(void) { uint32_t v; __asm__ volatile ("rdinstret %0" : "=r"(v)); return v; }
static inline uint32_t rc(void) { uint32_t v; __asm__ volatile ("rdcycle %0" : "=r"(v)); return v; }

#define SL 16
#define ST 12

static int32_t sv[ST + 1][SL + 1][3];   /* unit normals, 16.16 */

static void build(void) {
	int i, j;
	for (i = 0; i <= ST; i++)
		for (j = 0; j <= SL; j++) {
			/* lat 0..32768 (pole to pole), lon 0..65536 */
			uint32_t lat = (uint32_t)i * 32768 / ST, lon = (uint32_t)j * 65536 / SL;
			int32_t s = zglSin(lat);
			sv[i][j][0] = (int32_t)(((int64_t)s * zglCos(lon)) >> 16);
			sv[i][j][1] = zglCos(lat);
			sv[i][j][2] = (int32_t)(((int64_t)s * zglSin(lon)) >> 16);
		}
}

static inline void vert(int i, int j) {
	const int32_t *n = sv[i][j];
	glNormal3x(n[0], n[1], n[2]);
	glVertex3x(n[0] * 100, n[1] * 100, n[2] * 100);
}

static void sphere_geom(void) {
	int i, j;
	glBegin(GL_TRIANGLES);
	for (i = 0; i < ST; i++)
		for (j = 0; j < SL; j++) {
			vert(i, j); vert(i + 1, j); vert(i + 1, j + 1);
			vert(i, j); vert(i + 1, j + 1); vert(i, j + 1);
		}
	glEnd();
}

static void cube_geom(void) {
	glBegin(GL_QUADS);
	{
		static const int8_t n[6][3] = { {0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0} };
		static const int8_t q[6][4][3] = {
			{{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}}, {{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}},
			{{1,-1,1},{1,-1,-1},{1,1,-1},{1,1,1}}, {{-1,-1,-1},{-1,-1,1},{-1,1,1},{-1,1,-1}},
			{{-1,1,1},{1,1,1},{1,1,-1},{-1,1,-1}}, {{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1}} };
		int f, k;
		for (f = 0; f < 6; f++) {
			glNormal3i(n[f][0], n[f][1], n[f][2]);
			for (k = 0; k < 4; k++) glVertex3i(q[f][k][0] * 30, q[f][k][1] * 30, q[f][k][2] * 30);
		}
	}
	glEnd();
}

static void ring_geom(void) {
	int i;
	glBegin(GL_LINE_LOOP);
	for (i = 0; i < 48; i++) {
		uint32_t a = (uint32_t)i * 65536u / 48;
		glVertex3x(zglCos(a) * 170, zglSin(a * 3) * 12, zglSin(a) * 170);
	}
	glEnd();
}

/* the sphere, cube and ring as display lists (0: drawn as calls) */
static GLuint l_sph, l_cub, l_ring;
static bool no_ring;     /* without the ring of lines (immediate mode) */

static void scene(uint32_t angle) {
	GLint sph[3] = { 16, 47, 47 }, cub[3] = { 48, 79, 79 };
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatex(0, 0, -640 * 65536);
	glRotatex(25 << 16, 1 << 16, 0, 0);
	glPushMatrix();
	glRotatex((int32_t)angle << 16, 0, 1 << 16, 0);
	glMaterialiv(GL_FRONT_AND_BACK, GL_COLOR_INDEXES, sph);
	if (l_sph) glCallList(l_sph); else sphere_geom();
	glPopMatrix();
	glPushMatrix();
	glRotatex((int32_t)angle * 2 << 16, 0, 1 << 16, 0);
	glTranslatex(170 << 16, 0, 0);
	glRotatex((int32_t)angle * 5 << 16, 1 << 16, 1 << 16, 0);
	glMaterialiv(GL_FRONT_AND_BACK, GL_COLOR_INDEXES, cub);
	glShadeModel(GL_FLAT);
	if (l_cub) glCallList(l_cub); else cube_geom();
	glShadeModel(GL_SMOOTH);
	glPopMatrix();
	glDisable(GL_LIGHTING);
	glIndexi(100);
	if (no_ring) { glEnable(GL_LIGHTING); return; }
	if (l_ring) glCallList(l_ring);
	else ring_geom();
	glEnable(GL_LIGHTING);
}

static void run(const char *name, bool col, int ncol, bool depth) {
	uint32_t t0, t1, t2, fe = 0, be = 0, k, c0, c1, c2, cfe = 0, cbe = 0;
	colour = col; colours = ncol;
	if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	/* a frame first, not counted: lists are compiled at their first call */
	scene(3);
	zgl_frame();
	n_blit = 0;
	for (k = 0; k < 4; k++) {
		c0 = rc(); t0 = ir();
		scene(k * 7);
		c1 = rc(); t1 = ir();
		zgl_frame();
		c2 = rc(); t2 = ir();
		fe += t1 - t0; be += t2 - t1;
		cfe += c1 - c0; cbe += c2 - c1;
	}
	printf("framebench: %-22s front end %7lu, frame %8lu, total %8lu instructions a frame (%lu prims, %lu blits)\n",
		name, (unsigned long)(fe / 4), (unsigned long)(be / 4), (unsigned long)((fe + be) / 4),
		(unsigned long)zgl_info()->prims, (unsigned long)(n_blit / 4));
	if (cfe != fe)       /* the simulator's cycle model is on (ZSIM_COST) */
		printf("framebench: %-22s front end %7lu, frame %8lu cycles a frame (modelled)\n", "",
			(unsigned long)(cfe / 4), (unsigned long)(cbe / 4));
	n_blit = 0;
}

int main(void) {
	zgl_desc_t zd = { ZGL_FRAMES, 0, 0, 1024 };
	GLfixed light[4] = { -40000, 50000, 40000, 0 };
	GLfixed white[4] = { 65536, 65536, 65536, 65536 };
	uint32_t t0, t1;
	int i;
	build();
	for (i = 0; i < 256; i++) pal[i] = (uint16_t)(i * 0x111 & 0xfff);
	if (!zgl_open(&zd)) { printf("framebench: no memory\n"); return 1; }
	glMatrixMode(GL_PROJECTION); glLoadIdentity();
	zglFrustumi(-40, 40, -30, 30, 100, 2000);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
	glLightxv(GL_LIGHT0, GL_POSITION, light);
	glLightxv(GL_LIGHT0, GL_DIFFUSE, white);
	glEnable(GL_LIGHT0); glEnable(GL_LIGHTING); glEnable(GL_CULL_FACE);

	t0 = ir(); zgl_frame(); t1 = ir();
	printf("framebench: %-22s %lu instructions\n", "empty frame, 256", (unsigned long)(t1 - t0));
	run("256 colours, depth", true, 256, true);
	run("256 colours, no depth", true, 256, false);
	run("16 colours, depth", true, 16, true);
	run("mono, depth", false, 16, true);
	zgl_close();
	/* chunky: the triangle engine where there is one (the simulator's
	 * with ZSIM_GTRI set), else the software strips to VRAM */
	zd.flags = ZGL_CHUNKY;
	if (!zgl_open(&zd)) { printf("framebench: no memory\n"); return 1; }
	glMatrixMode(GL_PROJECTION); glLoadIdentity();
	zglFrustumi(-40, 40, -30, 30, 100, 2000);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
	glLightxv(GL_LIGHT0, GL_POSITION, light);
	glLightxv(GL_LIGHT0, GL_DIFFUSE, white);
	glEnable(GL_LIGHT0); glEnable(GL_LIGHTING); glEnable(GL_CULL_FACE);
	run(zgl_info()->backend == ZGL_BACKEND_T2 ? "chunky, T2" :
	    zgl_info()->backend == ZGL_BACKEND_ENGINE ? "chunky, engine" : "chunky, software", true, 256, true);
	/* the models as display lists: T2's fast path */
	l_sph = glGenLists(3);
	l_cub = l_sph + 1;
	l_ring = l_sph + 2;
	glNewList(l_ring, GL_COMPILE); ring_geom(); glEndList();
	glNewList(l_sph, GL_COMPILE); sphere_geom(); glEndList();
	glNewList(l_cub, GL_COMPILE); cube_geom(); glEndList();
	run(zgl_info()->backend == ZGL_BACKEND_T2 ? "chunky, T2, lists" : "chunky, lists", true, 256, true);

	zgl_close();
	return 0;
}
