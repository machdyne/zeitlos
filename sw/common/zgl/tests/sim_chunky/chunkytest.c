/*
 * ZGL_CHUNKY in software, in the simulator (whose VRAM holds both
 * sheets): frames drawn into chunky VRAM by zgl_chunky.c's strip
 * writer, read back through the chunky layout, against the same
 * frames as the triangle engine's commands carried out by its C model
 * (tests/t_model.c, which test_frames holds to the software). A whole
 * frame, and a scissored one (the strip writer's byte ends).
 * make -C sw/common/zgl simtest
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "zgl.h"
#include "zgl_int.h"
#include "../t_model.h"
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

static uint32_t cmd[60000];
static int ncmd;
static void put(uint32_t w) { cmd[ncmd++] = w; }
static uint8_t want[240][320], got[240][320];

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
	zgl_desc_t eng = { ZGL_FRAMES, 0, 0, 1024, 0 };
	zgl_desc_t soft = { ZGL_FRAMES, 0, 0, 1024, ZGL_CHUNKY | ZGL_NO_ENGINE };
	zgl_desc_t chunky = { ZGL_FRAMES, 0, 0, 1024, ZGL_CHUNKY };
	int f, x, y, bad = 0, pass, engine = 0;
	for (f = 0; f < 4; f++) {
		/* frames 0, 1: the software strips; 2, 3: the triangle engine
		 * where there is one (the simulator's, with ZSIM_GTRI) */
		pass = f >> 1;
		int32_t x0 = (f & 1) ? 37 : 0, x1 = (f & 1) ? 247 : 319, y0 = (f & 1) ? 69 : 0, y1 = (f & 1) ? 218 : 239;
		/* the model's picture */
		zgl_open(&eng);
		scene(f & 1);
		ncmd = 0;
		zglf_t_frame(put);
		memset(want, 0xa5, sizeof(want));
		{
			static uint8_t pic[240][320];
			memset(pic, 0, sizeof(pic));
			t_model_run(cmd, ncmd, pic);
			for (y = y0; y <= y1; y++) for (x = x0; x <= x1; x++) want[y][x] = pic[y][x];
		}
		zgl_close();
		/* the software's, through VRAM */
		for (y = 0; y < 240; y++) for (x = 0; x < 4; x++) {
			volatile uint32_t *d = z_chunky_seg((uint32_t)y, (uint32_t)x);
			int i;
			for (i = 0; i < Z_CHUNKY_SEG_WORDS; i++) d[i] = 0xa5a5a5a5u;
		}
		zgl_open(pass ? &chunky : &soft);
		if (pass && (zgl_info()->backend == ZGL_BACKEND_ENGINE || zgl_info()->backend == ZGL_BACKEND_T2)) engine = zgl_info()->backend;
		else if (zgl_info()->backend != ZGL_BACKEND_SOFTWARE) printf("chunkytest: backend %d\n", zgl_info()->backend);
		scene(f & 1);
		zgl_frame();
		glFinish();
		zgl_close();
		for (y = 0; y < 240; y++) for (x = 0; x < 320; x++)
			got[y][x] = ((volatile uint8_t *)z_chunky_seg((uint32_t)y, (uint32_t)(x / 80)))[x % 80];
		for (y = 0; y < 240; y++) for (x = 0; x < 320; x++)
			if (got[y][x] != want[y][x]) {
				if (bad < 5) printf("  frame %d (%d,%d): %02x, want %02x\n", f, x, y, got[y][x], want[y][x]);
				bad++;
			}
	}
	printf("chunkytest: %d pixels differ (software%s)\n", bad, engine == ZGL_BACKEND_T2 ? " and T2" : engine ? " and the engine" : "");
	return 0;
}
