/*
 * ZGL_FRAMES on a build host: zgl.c + zgl_frames.c with the arcade
 * runtime's drawing calls stood in for by a picture of indices. Checks
 * coverage against a first-principles test of every pixel centre (top-
 * left rule), shared edges, depth both ways round, smooth shading,
 * clipping, and the monochrome path. make -C sw/common/zgl test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../zgl.h"
#include "../zgl_int.h"

static uint8_t pic[240][320];      /* indices (colour) or bits (mono) */
static bool colour = true;
static uint16_t pal[256];

bool ar_color(void) { return colour; }
int ar_colours(void) { return 256; }
uint16_t ar_pal_get(int i) { return pal[i & 255]; }
void ar_pal(int i, uint16_t c) { pal[i & 255] = c; }
void ar_plane_line(int k, int x0, int y0, int x1, int y1, int op) { (void)k; (void)x0; (void)y0; (void)x1; (void)y1; (void)op; }
void ar_plane(int k, const void *bits, int stride, int sx, int sy, int x, int y, int w, int h) {
	const uint8_t *b = bits; int r, c;
	for (r = 0; r < h; r++) for (c = 0; c < w; c++) {
		int bit = (b[(sy + r) * stride + ((sx + c) >> 3)] >> ((sx + c) & 7)) & 1;
		uint8_t *p = &pic[y + r][x + c];
		*p = (uint8_t)((*p & ~(1 << k)) | (bit << k));
	}
}
void ar_bitmap_opaque(const void *bits, int stride, int sx, int sy, int x, int y, int w, int h) {
	const uint8_t *b = bits; int r, c;
	for (r = 0; r < h; r++) for (c = 0; c < w; c++)
		pic[y + r][x + c] = (b[(sy + r) * stride + ((sx + c) >> 3)] >> ((sx + c) & 7)) & 1;
}

static int bad;

#define CHECK(c, ...) do { if (!(c)) { bad++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void ortho(void) {
	glMatrixMode(GL_PROJECTION); glLoadIdentity();
	glOrthox(0, 320 << 16, 240 << 16, 0, -65536, 65536);    /* picture coordinates, y down */
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
}

/* pixel centre (px, py) inside triangle (screen 28.4 coordinates), top-left rule */
static bool inside(const double *x, const double *y, double px, double py) {
	int i; double area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
	for (i = 0; i < 3; i++) {
		int j = (i + 1) % 3;
		double ex = x[j] - x[i], ey = y[j] - y[i];
		double e = (px - x[i]) * ey - (py - y[i]) * ex;
		if (area > 0) e = -e;
		/* top-left: an edge whose pixels count when exactly on it */
		if (e < 0) return false;
		if (e == 0) {
			/* in y-down picture space with the triangle oriented, a top edge
			 * is horizontal going "right", a left edge goes "up" */
			double tx = area > 0 ? -ex : ex, ty = area > 0 ? -ey : ey;
			bool top = ty == 0 && tx < 0, left = ty > 0;
			if (!top && !left) return false;
		}
	}
	return true;
}

int main(void) {
	zgl_desc_t d; int i, x, y, n, miss = 0, extra = 0;
	memset(&d, 0, sizeof d);
	d.output = ZGL_FRAMES;
	if (!zgl_open(&d)) { printf("FAIL: open\n"); return 1; }

	/* 1. the whole picture, two triangles: every pixel once, no gaps */
	ortho(); memset(pic, 0, sizeof pic);
	glClearIndex(0); glShadeModel(GL_FLAT); glIndexi(7);
	glBegin(GL_QUADS); glVertex2i(0, 0); glVertex2i(320, 0); glVertex2i(320, 240); glVertex2i(0, 240); glEnd();
	zgl_frame();
	for (n = 0, y = 0; y < 240; y++) for (x = 0; x < 320; x++) n += pic[y][x] == 7;
	CHECK(n == 76800, "full quad: %d of 76800", n);

	/* 2. random triangles against the pixel-centre test */
	srand(3);
	for (i = 0; i < 300; i++) {
		double X[3], Y[3]; int k, m0 = miss, e0 = extra;
		memset(pic, 0, sizeof pic);
		for (k = 0; k < 3; k++) { X[k] = (rand() % (300 * 16)) / 16.0 + 5; Y[k] = (rand() % (220 * 16)) / 16.0 + 5; }
		/* straight to the rasterizer, in its own 28.4: this is the
		 * rasterizer's exactness, not the front end's arithmetic */
		{
			zgl_svtx_t sv[3];
			for (k = 0; k < 3; k++) { sv[k].x = (int32_t)(X[k] * 16); sv[k].y = (int32_t)(Y[k] * 16); sv[k].z = 0; sv[k].ci = 3 << 8; }
			zglf_tri(&sv[0], &sv[1], &sv[2]);
		}
		zgl_frame();
		for (y = 0; y < 240; y++) for (x = 0; x < 320; x++) {
			bool want = inside(X, Y, x, y), got = pic[y][x] == 3;
			if (want && !got) miss++;
			if (got && !want) extra++;
		}
		if (i < 3 && (miss != m0 || extra != e0)) {
			printf("  tri %d: (%.2f,%.2f) (%.2f,%.2f) (%.2f,%.2f): %d missing %d extra\n", i, X[0], Y[0], X[1], Y[1], X[2], Y[2], miss - m0, extra - e0);
			for (y = 0; y < 240; y++) for (x = 0; x < 320; x++) {
				bool want = inside(X, Y, x, y), got = pic[y][x] == 3;
				if (want != got) { printf("    first diff at %d,%d want %d\n", x, y, want); y = 240; break; }
			}
		}
	}
	CHECK(miss + extra == 0, "coverage: %d missing, %d extra over 300 triangles", miss, extra);
	printf("  coverage: %d missing, %d extra pixels over 300 random triangles\n", miss, extra);

	/* 3. depth, both orders: the near quad wins where they overlap */
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); zglFrustumi(-40, 40, -30, 30, 64, 4000);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
	glEnable(GL_DEPTH_TEST);
	for (i = 0; i < 2; i++) {
		int k;
		memset(pic, 0, sizeof pic);
		for (k = 0; k < 2; k++) {
			int near = (k == i);
			int z = near ? -300 : -600, s = near ? 60 : 200;
			glIndexi(near ? 9 : 5);
			glBegin(GL_QUADS); glVertex3i(-s, -s, z); glVertex3i(s, -s, z); glVertex3i(s, s, z); glVertex3i(-s, s, z); glEnd();
		}
		zgl_frame();
		CHECK(pic[120][160] == 9, "depth order %d: centre %d", i, pic[120][160]);
		CHECK(pic[120][100] == 5, "depth order %d: side %d", i, pic[120][10]);
	}
	glDisable(GL_DEPTH_TEST);

	/* 4. smooth shading: index 16 to 31 left to right, rising */
	ortho(); memset(pic, 0, sizeof pic); glShadeModel(GL_SMOOTH);
	glBegin(GL_QUADS);
	glIndexi(16); glVertex2i(0, 0); glIndexi(31); glVertex2i(320, 0); glVertex2i(320, 16); glIndexi(16); glVertex2i(0, 16);
	glEnd();
	zgl_frame();
	{
		int lo = 255, hi = 0, prev = 0, down = 0;
		for (x = 0; x < 320; x++) {
			int sum = 0;
			for (y = 0; y < 16; y++) { int c = pic[y][x]; if (c < lo) lo = c; if (c > hi) hi = c; sum += c; }
			if (x >= 8 && sum + 4 * 16 < prev) down++;
			if (x % 8 == 7) prev = sum;
		}
		CHECK(lo >= 16 && hi <= 31 && hi - lo >= 14, "smooth range %d..%d", lo, hi);
		CHECK(down == 0, "smooth not rising (%d)", down);
	}

	/* 5. clipping: a triangle through the near plane draws, no wild pixels */
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); zglFrustumi(-40, 40, -30, 30, 64, 4000);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glShadeModel(GL_FLAT);
	memset(pic, 0, sizeof pic); glIndexi(4);
	glBegin(GL_TRIANGLES); glVertex3i(-100, -50, 100); glVertex3i(100, -50, 100); glVertex3i(0, -50, -800); glEnd();
	zgl_frame();
	for (n = 0, y = 0; y < 120; y++) for (x = 0; x < 320; x++) n += pic[y][x] == 4;
	CHECK(n == 0, "near-plane triangle above the horizon: %d pixels", n);
	for (n = 0, y = 120; y < 240; y++) for (x = 0; x < 320; x++) n += pic[y][x] == 4;
	CHECK(n > 1000, "near-plane triangle: %d pixels", n);

	/* 6. lines: random ones straight to the rasterizer, against the
	 * same DDA walked whole (each strip walks only its own part) */
	{
		int lbad = 0;
		srand(5);
		for (i = 0; i < 200; i++) {
			zgl_svtx_t a, b;
			static uint8_t ref[240][320];
			int32_t dx, dy, adx, ady, k, nn;
			int64_t sx, sy, X, Y;
			memset(pic, 0, sizeof pic); memset(ref, 0, sizeof ref);
			a.x = rand() % (330 * 16) - 80; a.y = rand() % (250 * 16) - 80;
			b.x = rand() % (330 * 16) - 80; b.y = rand() % (250 * 16) - 80;
			if (i % 4 == 0) b.y = a.y + rand() % 64 - 32;        /* nearly flat */
			a.z = b.z = 0; a.ci = b.ci = 6 << 8;
			zglf_line(&a, &b);
			zgl_frame();
			dx = b.x - a.x; dy = b.y - a.y; adx = dx < 0 ? -dx : dx; ady = dy < 0 ? -dy : dy;
			nn = ((adx > ady ? adx : ady) + 15) >> 4; if (nn == 0) nn = 1;
			sx = ((int64_t)dx * 65536) / nn; sy = ((int64_t)dy * 65536) / nn;
			X = (int64_t)a.x * 65536; Y = (int64_t)a.y * 65536;
			for (k = 0; k <= nn; k++, X += sx, Y += sy) {
				int32_t px = (int32_t)((X + (8 << 16)) >> 20), py = (int32_t)((Y + (8 << 16)) >> 20);
				if (px >= 0 && px < 320 && py >= 0 && py < 240) ref[py][px] = 6;
			}
			for (y = 0; y < 240; y++) for (x = 0; x < 320; x++)
				if ((pic[y][x] == 6) != (ref[y][x] == 6)) lbad++;
		}
		CHECK(lbad == 0, "lines: %d pixels differ from the whole walk", lbad);
		printf("  lines: %d pixels differ over 200 random lines\n", lbad);
	}

	/* 7. depth per primitive: an untested quad drawn last covers a
	 * depth-tested near one (GL_DEPTH_TEST as it was when submitted) */
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); zglFrustumi(-40, 40, -30, 30, 64, 4000);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity(); memset(pic, 0, sizeof pic);
	glEnable(GL_DEPTH_TEST);
	glIndexi(9); glBegin(GL_QUADS); glVertex3i(-60, -60, -300); glVertex3i(60, -60, -300); glVertex3i(60, 60, -300); glVertex3i(-60, 60, -300); glEnd();
	glIndexi(5); glBegin(GL_QUADS); glVertex3i(-200, -200, -600); glVertex3i(200, -200, -600); glVertex3i(200, 200, -600); glVertex3i(-200, 200, -600); glEnd();
	glDisable(GL_DEPTH_TEST);
	glIndexi(11); glBegin(GL_QUADS); glVertex3i(-20, -20, -900); glVertex3i(20, -20, -900); glVertex3i(20, 20, -900); glVertex3i(-20, 20, -900); glEnd();
	zgl_frame();
	CHECK(pic[120][160] == 11, "untested last: centre %d", pic[120][160]);
	CHECK(pic[120][120] == 9, "tested near: %d", pic[120][120]);
	CHECK(pic[120][90] == 5, "tested far: %d", pic[120][90]);

	/* 8. display lists: a lit, rotated, nested scene called from lists
	 * draws exactly what the same calls draw directly */
	{
		static uint8_t direct[240][320];
		GLuint base;
		int pass, diff = 0;
		GLfixed ldir[4] = { -30000, 40000, 50000, 0 };
		glMatrixMode(GL_PROJECTION); glLoadIdentity(); zglFrustumi(-40, 40, -30, 30, 64, 4000);
		glMatrixMode(GL_MODELVIEW); glLoadIdentity();
		glLightxv(GL_LIGHT0, GL_POSITION, ldir);
		base = glGenLists(2);
		CHECK(base != 0 && glIsList(base) && glIsList(base + 1), "glGenLists %u", base);
		for (pass = 0; pass < 3; pass++) {
			/* 0: direct; 1: GL_COMPILE then called; 2: COMPILE_AND_EXECUTE */
			memset(pic, 0, sizeof pic);
			glLoadIdentity();
			glTranslatex(0, 0, -500 * 65536);
			if (pass == 1) glNewList(base + 1, GL_COMPILE);
			if (pass == 2) glNewList(base + 1, GL_COMPILE_AND_EXECUTE);
			/* the inner list: one lit quad */
			glBegin(GL_QUADS);
			glNormal3i(0, 0, 1);
			glVertex3i(-50, -50, 0); glVertex3i(50, -50, 0); glVertex3i(50, 50, 0); glVertex3i(-50, 50, 0);
			glEnd();
			if (pass) glEndList();
			if (pass == 1) {
				/* nothing drawn while compiling */
				zgl_frame();
				for (n = 0, y = 0; y < 240; y++) for (x = 0; x < 320; x++) n += pic[y][x] != 0;
				CHECK(n == 0, "GL_COMPILE drew %d pixels", n);
				memset(pic, 0, sizeof pic);
			}
			if (pass) glNewList(base, pass == 1 ? GL_COMPILE : GL_COMPILE_AND_EXECUTE);
			glEnable(GL_LIGHTING); glEnable(GL_LIGHT0); glEnable(GL_DEPTH_TEST);
			{ GLint m[3] = { 16, 31, 31 }; glMaterialiv(GL_FRONT_AND_BACK, GL_COLOR_INDEXES, m); }
			glPushMatrix();
			glRotatex(30 << 16, 0, 1 << 16, 0);
			glScalex(3 << 15, 3 << 15, 3 << 15);
			if (pass) glCallList(base + 1);
			else {
				glBegin(GL_QUADS);
				glNormal3i(0, 0, 1);
				glVertex3i(-50, -50, 0); glVertex3i(50, -50, 0); glVertex3i(50, 50, 0); glVertex3i(-50, 50, 0);
				glEnd();
			}
			glPopMatrix();
			glShadeModel(GL_SMOOTH);
			glIndexi(40);
			glDisable(GL_LIGHTING);
			glBegin(GL_LINES); glVertex3i(-150, 80, -100); glVertex3i(150, 90, 0); glEnd();
			glDisable(GL_DEPTH_TEST);
			if (pass) glEndList();
			if (pass == 1) {
				zgl_frame();
				memset(pic, 0, sizeof pic);
				glLoadIdentity();
				glTranslatex(0, 0, -500 * 65536);
				glCallList(base);
			}
			zgl_frame();
			if (pass == 0) memcpy(direct, pic, sizeof pic);
			else {
				for (y = 0; y < 240; y++) for (x = 0; x < 320; x++) diff += pic[y][x] != direct[y][x];
			}
		}
		for (n = 0, y = 0; y < 240; y++) for (x = 0; x < 320; x++) n += direct[y][x] != 0;
		CHECK(n > 2000 && diff == 0, "lists: %d pixels drawn, %d differ", n, diff);
		printf("  lists: %d pixels, %d differ from the direct calls\n", n, diff);
		glDeleteLists(base, 2);
		CHECK(!glIsList(base), "glDeleteLists");
		CHECK(glGetError() == GL_NO_ERROR, "lists: an error");
	}

	/* 10. monochrome: a white index lit, a black one dark */
	colour = false; ortho(); pal[1] = 0xfff; pal[2] = 0x000; memset(pic, 0, sizeof pic);
	glIndexi(1); glBegin(GL_QUADS); glVertex2i(0, 0); glVertex2i(160, 0); glVertex2i(160, 240); glVertex2i(0, 240); glEnd();
	glIndexi(2); glBegin(GL_QUADS); glVertex2i(160, 0); glVertex2i(320, 0); glVertex2i(320, 240); glVertex2i(160, 240); glEnd();
	zgl_frame();
	for (n = 0, y = 0; y < 240; y++) for (x = 0; x < 160; x++) n += pic[y][x];
	CHECK(n == 160 * 240, "mono white: %d", n);
	for (n = 0, y = 0; y < 240; y++) for (x = 160; x < 320; x++) n += pic[y][x];
	CHECK(n == 0, "mono black: %d", n);

	printf("test_frames: %d failed\n", bad);
	return bad ? 1 : 0;
}
