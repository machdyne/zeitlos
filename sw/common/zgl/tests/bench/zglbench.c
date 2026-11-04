/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * zglbench -- instructions a line through zgl's front end (transform,
 * clip, project; no back end), for 400 spinning cubes of 12 edges, as
 * a game draws them. Run in the simulator, whose instret counts:
 * make -C sw/common/zgl bench. docs/zgl.md, "Cost".
 */
#include <stdio.h>
#include <stdint.h>
#include "zgl.h"
#include "zgl_int.h"
static uint32_t nl;
void zgl_back_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t c) { nl++; (void)x0; (void)y0; (void)x1; (void)y1; (void)c; }
static inline uint32_t ir(void) { uint32_t v; __asm__ volatile ("rdinstret %0" : "=r"(v)); return v; }
int main(void) {
	uint32_t t0, t1, i, seed = 1;
	zgl_reset_state();
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); zglFrustumi(-40, 40, -30, 30, 64, 100000);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glScalex(65536, 65536, -65536);
	t0 = ir();
	for (i = 0; i < 400; i++) {
		int32_t x, y, z;
		seed = seed * 1103515245u + 12345u; x = (int32_t)(seed >> 22) - 512;
		seed = seed * 1103515245u + 12345u; y = (int32_t)(seed >> 22) - 512;
		seed = seed * 1103515245u + 12345u; z = 200 + (int32_t)(seed >> 21);
		glPushMatrix(); zglTranslatei(x, y, z); glRotatex((int32_t)i * 92160 * 3, 65536, 0, 0); glRotatex((int32_t)i * 92160, 0, 65536, 0);
		glScalex(40 * 512, 40 * 512, 40 * 512);
		glBegin(GL_LINES);
		{ static const int8_t v[8][3] = {{-100,-100,-100},{100,-100,-100},{100,100,-100},{-100,100,-100},{-100,-100,100},{100,-100,100},{100,100,100},{-100,100,100}};
		  static const uint8_t e[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
		  int k; for (k = 0; k < 12; k++) { glVertex3i(v[e[k][0]][0], v[e[k][0]][1], v[e[k][0]][2]); glVertex3i(v[e[k][1]][0], v[e[k][1]][1], v[e[k][1]][2]); } }
		glEnd();
		glPopMatrix();
	}
	t1 = ir();
	printf("zglbench: 400 models x 12 lines: %lu lines out, %lu instructions, %lu a line\n",
		(unsigned long)nl, (unsigned long)(t1 - t0), (unsigned long)((t1 - t0) / 4800));
	return 0;
}
