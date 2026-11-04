/*
 * The triangle engine's test vectors (rtl/gpu/bench/tb_gpu_tri.v): a
 * few frames drawn by zgl on the build host, as the engine's commands,
 * and the chunky VRAM each should leave, from the engine's C model
 * (t_model.c), which test_frames holds to the software strips.
 *
 *   t_vectors <dir>   writes <dir>/cmd.hex, frames.hex, expect.hex, lum.hex
 *
 * frames.hex: 8 words a frame -- its command words, then 0 chunky or
 * 1 MONO, the window's x and y, and the visible rectangle (x0, x1, y0,
 * y1, picture coordinates). Frames 1-4 go to chunky VRAM, 5 and 6 to
 * the 1 bpp desktop as a window would.
 *
 * make -C sw/common/zgl vectors (rtl/gpu/bench/tri/).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../zgl.h"
#include "../zgl_int.h"
#include "t_model.h"

bool ar_color(void) { return true; }
int ar_colours(void) { return 256; }
static uint16_t pal[256];
uint16_t ar_pal_get(int i) { return pal[i & 255]; }
void ar_pal(int i, uint16_t c) { pal[i & 255] = c; }
void ar_plane_line(int k, int x0, int y0, int x1, int y1, int op) { (void)k; (void)x0; (void)y0; (void)x1; (void)y1; (void)op; }
void ar_plane(int k, const void *bits, int stride, int sx, int sy, int x, int y, int w, int h) {
	(void)k; (void)bits; (void)stride; (void)sx; (void)sy; (void)x; (void)y; (void)w; (void)h;
}
void ar_bitmap_opaque(const void *bits, int stride, int sx, int sy, int x, int y, int w, int h) {
	(void)bits; (void)stride; (void)sx; (void)sy; (void)x; (void)y; (void)w; (void)h;
}

#define MAXW 400000
static uint32_t cmd[MAXW];
static int ncmd;
static void put(uint32_t w) { if (ncmd < MAXW) cmd[ncmd++] = w; }

#define VWORDS 19200                    /* chunky: 960 framebuffer rows of 20 words */
static uint32_t vram[VWORDS];
static uint8_t lum[256];

static const uint8_t bayer8[8][8] = {
	{  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
	{ 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
	{  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
	{ 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 }
};

/* the picture into the 1 bpp desktop as MONO puts it: at (wx, wy) on
 * the screen, inside the rectangle (picture coordinates) */
static void to_desk(uint8_t pic[240][320], int wx, int wy, int x0, int x1, int y0, int y1) {
	int x, y;
	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++) {
			int sx = wx + x, sy = wy + y;
			uint32_t *w = &vram[sy * 20 + sx / 32];
			uint32_t bit = 1u << (sx & 31);
			if (lum[pic[y][x]] * 4 > bayer8[y & 7][x & 7]) *w |= bit; else *w &= ~bit;
		}
}

/* the picture's bytes inside the frame's rectangle into chunky VRAM,
 * as the engine writes them */
static void to_vram(uint8_t pic[240][320], int x0, int y0, int x1, int y1) {
	int x, y;
	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++) {
			int seg = x / 80, i = (x % 80) / 4, b = x & 3;
			int row = y + ((seg & 1) ? 240 : 0) + ((seg & 2) ? 480 : 0);
			uint32_t *w = &vram[row * 20 + i];
			*w = (*w & ~(0xffu << (8 * b))) | (uint32_t)pic[y][x] << (8 * b);
		}
}

static void sphere(int lat_n, int lon_n, int r) {
	int i, j;
	glBegin(GL_TRIANGLES);
	for (i = 0; i < lat_n; i++)
		for (j = 0; j < lon_n; j++) {
			int k;
			int ii[6] = { i, i + 1, i + 1, i, i + 1, i };
			int jj[6] = { j, j, j + 1, j, j + 1, j + 1 };
			for (k = 0; k < 6; k++) {
				uint32_t lat = (uint32_t)ii[k] * 32768u / (uint32_t)lat_n;
				uint32_t lon = (uint32_t)jj[k] * 65536u / (uint32_t)lon_n;
				int32_t s = zglSin(lat);
				int32_t x = (int32_t)(((int64_t)s * zglCos(lon)) >> 16);
				int32_t y = zglCos(lat);
				int32_t z = (int32_t)(((int64_t)s * zglSin(lon)) >> 16);
				glNormal3x(x, y, z);
				glVertex3x(x * r, y * r, z * r);
			}
		}
	glEnd();
}

static void persp(void) {
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); zglFrustumi(-40, 40, -30, 30, 100, 2000);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
}

static void scene(int f) {
	int k, j;
	GLfixed light[4] = { -40000, 50000, 40000, 0 };
	GLfixed white[4] = { 65536, 65536, 65536, 65536 };
	glDisable(GL_SCISSOR_TEST);
	glViewport(0, 0, 320, 240);
	switch (f) {
	case 0: case 3:                     /* gldemo's: lit sphere, cube, ring */
		persp();
		glClearIndex(0);
		glLightxv(GL_LIGHT0, GL_POSITION, light);
		glLightxv(GL_LIGHT0, GL_DIFFUSE, white);
		glEnable(GL_LIGHT0); glEnable(GL_LIGHTING); glEnable(GL_CULL_FACE); glEnable(GL_DEPTH_TEST);
		glShadeModel(GL_SMOOTH);
		glTranslatex(0, 0, -640 * 65536);
		glRotatex(25 << 16, 1 << 16, 0, 0);
		glPushMatrix();
		glRotatex((30 + f * 20) << 16, 0, 1 << 16, 0);
		{ GLint m[3] = { 16, 47, 47 }; glMaterialiv(GL_FRONT_AND_BACK, GL_COLOR_INDEXES, m); }
		sphere(12, 16, 100);
		glPopMatrix();
		glPushMatrix();
		glRotatex((60 + f * 40) << 16, 0, 1 << 16, 0);
		glTranslatex(170 << 16, 0, 0);
		glRotatex(40 << 16, 1 << 16, 1 << 16, 0);
		{ GLint m[3] = { 48, 79, 79 }; glMaterialiv(GL_FRONT_AND_BACK, GL_COLOR_INDEXES, m); }
		glShadeModel(GL_FLAT);
		glBegin(GL_QUADS);
		{
			static const int8_t n[6][3] = { {0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0} };
			static const int8_t q[6][4][3] = {
				{{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}}, {{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}},
				{{1,-1,1},{1,-1,-1},{1,1,-1},{1,1,1}}, {{-1,-1,-1},{-1,-1,1},{-1,1,1},{-1,1,-1}},
				{{-1,1,1},{1,1,1},{1,1,-1},{-1,1,-1}}, {{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1}} };
			for (k = 0; k < 6; k++) {
				glNormal3i(n[k][0], n[k][1], n[k][2]);
				for (j = 0; j < 4; j++) glVertex3i(q[k][j][0] * 30, q[k][j][1] * 30, q[k][j][2] * 30);
			}
		}
		glEnd();
		glPopMatrix();
		glDisable(GL_LIGHTING);
		glIndexi(100);
		glBegin(GL_LINE_LOOP);
		for (k = 0; k < 48; k++) {
			uint32_t a = (uint32_t)k * 65536u / 48;
			glVertex3x(zglCos(a) * 170, zglSin(a * 3) * 12, zglSin(a) * 170);
		}
		glEnd();
		glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_LIGHT0);
		break;
	case 1:                             /* random, depth, smooth */
		persp();
		glClearIndex(3);
		glEnable(GL_DEPTH_TEST);
		glShadeModel(GL_SMOOTH);
		srand(7);
		for (k = 0; k < 120; k++) {
			glBegin(k % 4 == 3 ? GL_LINES : GL_TRIANGLES);
			for (j = 0; j < (k % 4 == 3 ? 2 : 3); j++) {
				glIndexi(16 + rand() % 200);
				glVertex3i(rand() % 1200 - 600, rand() % 900 - 450, -150 - rand() % 1500);
			}
			glEnd();
		}
		glDisable(GL_DEPTH_TEST);
		break;
	default:                            /* ortho, scissored, flat, no depth */
		glMatrixMode(GL_PROJECTION); glLoadIdentity();
		glOrthox(0, 320 << 16, 240 << 16, 0, -65536, 65536);
		glMatrixMode(GL_MODELVIEW); glLoadIdentity();
		glClearIndex(7);
		glEnable(GL_SCISSOR_TEST); glScissor(37, 21, 211, 150);
		glShadeModel(GL_FLAT);
		srand(9);
		for (k = 0; k < 60; k++) {
			glBegin(k % 5 == 4 ? GL_LINES : GL_TRIANGLES);
			for (j = 0; j < (k % 5 == 4 ? 2 : 3); j++) {
				glIndexi(16 + rand() % 200);
				glVertex3i(rand() % 400 - 40, rand() % 320 - 40, 0);
			}
			glEnd();
		}
		break;
	}
}

int main(int argc, char **argv) {
	const char *dir = argc > 1 ? argv[1] : ".";
	char path[512];
	FILE *fc, *ff, *fe;
	static uint8_t pic[240][320];
	zgl_desc_t d;
	int f, i;
	memset(&d, 0, sizeof d);
	d.output = ZGL_FRAMES;
	d.max_prims = 4000;
	if (!zgl_open(&d)) return 1;
	snprintf(path, sizeof path, "%s/cmd.hex", dir); fc = fopen(path, "w");
	snprintf(path, sizeof path, "%s/frames.hex", dir); ff = fopen(path, "w");
	snprintf(path, sizeof path, "%s/expect.hex", dir); fe = fopen(path, "w");
	if (!fc || !ff || !fe) { perror(dir); return 1; }
	for (i = 0; i < VWORDS; i++) vram[i] = 0xa5a5a5a5u;   /* the bench's VRAM at start */
	snprintf(path, sizeof path, "%s/lum.hex", dir);
	{
		FILE *fl = fopen(path, "w");
		if (!fl) { perror(path); return 1; }
		for (i = 0; i < 256; i++) {
			lum[i] = (uint8_t)((i * 37 + (i >> 3)) % 17);
			fprintf(fl, "%02x\n", lum[i]);
		}
		fclose(fl);
	}
	for (f = 0; f < 6; f++) {
		int start = ncmd, strips;
		int32_t x0, y0, x1, y1;
		static const int16_t win[6][6] = {     /* MONO frames: wx, wy, x0, x1, y0, y1 */
			{0}, {0}, {0}, {0},
			{ 101, 57, 0, 319, 0, 239 },
			{ 37, 200, 10, 300, 5, 200 }
		};
		bool m = f >= 4;
		scene(f == 4 ? 0 : f == 5 ? 1 : f);
		/* the frame's rectangle, as zglf_t_frame() works it out */
		x0 = 0; y0 = 0; x1 = 319; y1 = 239;
		if (zgl_s.scissor) { x0 = zgl_s.sc_x0; y0 = zgl_s.sc_y0; x1 = zgl_s.sc_x1; y1 = zgl_s.sc_y1; }
		zglf_t_frame(put);
		memset(pic, 0, sizeof pic);
		strips = t_model_run(&cmd[start], ncmd - start, pic);
		if (m) {
			int cx0 = win[f][2] > x0 ? win[f][2] : x0, cx1 = win[f][3] < x1 ? win[f][3] : x1;
			int cy0 = win[f][4] > y0 ? win[f][4] : y0, cy1 = win[f][5] < y1 ? win[f][5] : y1;
			to_desk(pic, win[f][0], win[f][1], cx0, cx1, cy0, cy1);
		} else to_vram(pic, x0, y0, x1, y1);
		fprintf(ff, "%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n%08x\n", ncmd - start, m ? 1 : 0,
			m ? win[f][0] : 0, m ? win[f][1] : 0, m ? win[f][2] : 0, m ? win[f][3] : 319,
			m ? win[f][4] : 0, m ? win[f][5] : 239);
		for (i = 0; i < VWORDS; i++) fprintf(fe, "%08x\n", vram[i]);
		printf("t_vectors: frame %d: %d words, %d strips\n", f, ncmd - start, strips);
	}
	for (i = 0; i < ncmd; i++) fprintf(fc, "%08x\n", cmd[i]);
	fclose(fc); fclose(ff); fclose(fe);
	printf("t_vectors: %d command words in all\n", ncmd);
	return 0;
}
