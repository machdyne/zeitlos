/*
 * ZGL_CHUNKY in the simulator (whose VRAM holds both sheets): frames
 * drawn into chunky VRAM by zgl_chunky.c's strip writer, read back
 * through the chunky layout, and the same frames by T2 (its model, with
 * ZSIM_GTRI=2) held to them. A whole frame, and a scissored one (the
 * strip writer's byte ends).
 * With ZSIM_CONSOLE=1 (a `VRAM_SDRAM console) the picture is in main
 * memory, linear, where z_chunky_prepare() put it, and T2 writes there.
 * make -C sw/common/zgl simtest
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "zgl.h"
#include "zgl_int.h"
#include "../../../zsoc.h"

/* the arcade runtime's calls zgl_frames.c names (unused in chunky) */
bool ar_color(void) { return true; }
int ar_colours(void) { return 256; }
uint16_t ar_pal_get(int i) { (void)i; return 0; }
void ar_pal(int i, uint16_t c) { (void)i; (void)c; }
void ar_plane_line(int k, int x0, int y0, int x1, int y1, int op) { (void)k; (void)x0; (void)y0; (void)x1; (void)y1; (void)op; }
void ar_plane(int k, const void *b, int st, int sx, int sy, int x, int y, int w, int h) {
	(void)k; (void)b; (void)st; (void)sx; (void)sy; (void)x; (void)y; (void)w; (void)h;
}
void ar_bitmap_opaque(const void *b, int st, int sx, int sy, int x, int y, int w, int h) {
	(void)b; (void)st; (void)sx; (void)sy; (void)x; (void)y; (void)w; (void)h;
}

static uint8_t want[2][240][320], got[240][320];

static uint32_t seed = 1;
static int rnd(int n) { seed = seed * 1103515245u + 12345u; return (int)((seed >> 16) % (uint32_t)n); }

static void scene(int f) {
	int k, j;
	seed = 7u + (uint32_t)f;
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); zglFrustumi(-40, 40, -30, 30, 64, 4000);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
	glClearIndex((float)(f * 5));
	glEnable(GL_DEPTH_TEST);
	glShadeModel(GL_SMOOTH);
	if (f) { glEnable(GL_SCISSOR_TEST); glScissor(37, 21, 211, 150); }
	else glDisable(GL_SCISSOR_TEST);
	for (k = 0; k < 80; k++) {
		glBegin(k % 4 == 3 ? GL_LINES : GL_TRIANGLES);
		for (j = 0; j < (k % 4 == 3 ? 2 : 3); j++) {
			glIndexi(16 + rnd(200));
			glVertex3i(rnd(1200) - 600, rnd(900) - 450, -150 - rnd(1500));
		}
		glEnd();
	}
}

int main(void) {
	zgl_desc_t soft = { ZGL_FRAMES, 0, 0, 1024, ZGL_CHUNKY | ZGL_NO_ENGINE };
	zgl_desc_t chunky = { ZGL_FRAMES, 0, 0, 1024, ZGL_CHUNKY };
	int f, x, y, bad = 0, pass, engine = 0;
	/* the console's picture in main memory (nothing elsewhere) */
	if (!z_chunky_prepare()) { printf("chunkytest: no picture\n"); return 1; }
	for (f = 0; f < 4; f++) {
		/* frames 0, 1: the software strips, kept; 2, 3: T2 where there
		 * is one (the simulator's, with ZSIM_GTRI=2), held to them */
		pass = f >> 1;
		int32_t x0 = (f & 1) ? 37 : 0, x1 = (f & 1) ? 247 : 319, y0 = (f & 1) ? 69 : 0, y1 = (f & 1) ? 218 : 239;
		for (y = 0; y < 240; y++) for (x = 0; x < 4; x++) {
			volatile uint32_t *d = z_chunky_seg((uint32_t)y, (uint32_t)x);
			int i;
			for (i = 0; i < Z_CHUNKY_SEG_WORDS; i++) d[i] = 0xa5a5a5a5u;
		}
		zgl_open(pass ? &chunky : &soft);
		if (pass && zgl_info()->backend == ZGL_BACKEND_T2) engine = 1;
		else if (zgl_info()->backend != ZGL_BACKEND_SOFTWARE) printf("chunkytest: backend %d\n", zgl_info()->backend);
		scene(f & 1);
		zgl_frame();
		glFinish();
		zgl_close();
		for (y = 0; y < 240; y++) for (x = 0; x < 320; x++)
			got[y][x] = ((volatile uint8_t *)z_chunky_seg((uint32_t)y, (uint32_t)(x / 80)))[x % 80];
		if (!pass) {
			/* the software's: outside the frame the VRAM as it was */
			int out = 0;
			for (y = 0; y < 240; y++) for (x = 0; x < 320; x++)
				if ((x < x0 || x > x1 || y < y0 || y > y1) && got[y][x] != 0xa5) out++;
			if (out) { printf("  frame %d: %d pixels outside it written\n", f, out); bad += out; }
			memcpy(want[f & 1], got, sizeof(got));
			continue;
		}
		for (y = 0; y < 240; y++) for (x = 0; x < 320; x++)
			if (got[y][x] != want[f & 1][y][x]) {
				if (bad < 5) printf("  frame %d (%d,%d): %02x, the software's %02x\n", f, x, y, got[y][x], want[f & 1][y][x]);
				bad++;
			}
	}
	printf("chunkytest: %d pixels differ (software%s)%s\n", bad, engine ? " and T2" : "",
		z_chunky_mem ? ", the picture in main memory" : "");
	return 0;
}
