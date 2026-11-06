/*
 * mauskart3d -- the karts and their drivers: a matchbox on four wheels,
 * the driver's head over its back half, as display lists (front to -z,
 * on the ground at y 0, world units). Near: about 30 triangles; far: the
 * box alone (docs/mauskart3d.md, "Karts").
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 */
#include <stdint.h>
#include <stdbool.h>

#include "zgl.h"
#include "../mausrun3d/world.h"
#include "mauskart3d.h"
#include "karts.h"

uint32_t kt_list;                    /* KT_LISTS lists from here */
uint16_t kt_tris[KT_LISTS];
static int ntri;

/* the racers: the driver's ramps and the matchbox's */
const racer_t kt_racer[RACERS] = {
	{ "MAUS",  W3_R_MOUSE, W3_R_PINK,  W3_R_RED },
	{ "KATZE", W3_R_CAT,   W3_R_ROCK,  W3_R_WATER },
	{ "FROSCH", W3_R_GRASS, W3_R_QBOX, W3_R_QBOX },
	{ "EULE",  W3_R_EARTH, W3_R_QBOX,  W3_R_BOX },
};

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

/* a textured quad: corners counter-clockwise from the front, the
 * texture's corners (0,0) (16,0) (16,16) (0,16) on them */
static void tquad(const int *a, const int *b, const int *c, const int *d) {
	int32_t ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
	int32_t wx = c[0] - a[0], wy = c[1] - a[1], wz = c[2] - a[2];
	glNormal3i(uy * wz - uz * wy, uz * wx - ux * wz, ux * wy - uy * wx);
	glTexCoord2i(0, 16);  glVertex3i(a[0], a[1], a[2]);
	glTexCoord2i(16, 16); glVertex3i(b[0], b[1], b[2]);
	glTexCoord2i(16, 0);  glVertex3i(c[0], c[1], c[2]);
	glTexCoord2i(0, 16);  glVertex3i(a[0], a[1], a[2]);
	glTexCoord2i(16, 0);  glVertex3i(c[0], c[1], c[2]);
	glTexCoord2i(0, 0);   glVertex3i(d[0], d[1], d[2]);
	ntri += 2;
}

/* five faces of a box (no bottom): corners bit 0 x, 1 y, 2 z */
static void box5(int x0, int y0, int z0, int x1, int y1, int z1, bool tex) {
	int p[8][3];
	int i;
	for (i = 0; i < 8; i++) {
		p[i][0] = (i & 1) ? x1 : x0;
		p[i][1] = (i & 2) ? y1 : y0;
		p[i][2] = (i & 4) ? z1 : z0;
	}
	if (tex) {
		tquad(p[5], p[1], p[3], p[7]);    /* +x, seen from the right: its front */
		tquad(p[0], p[4], p[6], p[2]);    /* -x */
		tquad(p[4], p[5], p[7], p[6]);    /* +z (the back); no front: seen from behind */
	} else {                          /* far: seen from behind, no front */
		quad(p[1], p[3], p[7], p[5]);
		quad(p[0], p[4], p[6], p[2]);
		quad(p[4], p[5], p[7], p[6]);
	}
	glEnd();
	glBindTexture(GL_TEXTURE_2D, 0);
	glBegin(GL_TRIANGLES);
	quad(p[2], p[6], p[7], p[3]);         /* the top */
}

/* a flat triangle facing back (+z), towards the chase camera */
static void card(int ax, int ay, int az, int bx, int by, int bz, int cx, int cy, int cz) {
	if ((bx - ax) * (cy - ay) - (by - ay) * (cx - ax) > 0) tri3(ax, ay, az, bx, by, bz, cx, cy, cz);
	else tri3(ax, ay, az, cx, cy, cz, bx, by, bz);
}

/* the matchbox: label on its sides, the tray its top */
static void matchbox(int ramp, bool far) {
	mk_material(ramp);
	glBindTexture(GL_TEXTURE_2D, far ? 0 : 7);   /* TX_MATCH (track.c) */
	glBegin(GL_TRIANGLES);
	box5(-7, 3, -11, 7, 9, 11, !far);
	glEnd();
	if (far) return;
	/* wheels: black cards outside the box, facing out */
	mk_flat(C_BLACK);
	glBegin(GL_TRIANGLES);
	{
		static const int8_t wz[2] = { -7, 7 };
		int i;
		for (i = 0; i < 2; i++) {
			int z0 = wz[i] - 3, z1 = wz[i] + 3;
			int r0[3] = { 8, 0, z1 }, r1[3] = { 8, 0, z0 }, r2[3] = { 8, 6, z0 }, r3[3] = { 8, 6, z1 };
			int l0[3] = { -8, 0, z0 }, l1[3] = { -8, 0, z1 }, l2[3] = { -8, 6, z1 }, l3[3] = { -8, 6, z0 };
			quad(r0, r1, r2, r3);
			quad(l0, l1, l2, l3);
		}
	}
	glEnd();
}

/* a head over the back half: a box tapering to its top, its face ahead */
static void head(int ramp, int hw, int y0, int y1, int z) {
	int b[4][3] = { { -hw, y0, z - hw }, { hw, y0, z - hw }, { hw, y0, z + hw }, { -hw, y0, z + hw } };
	int t[4][3] = { { -hw + 1, y1, z - hw + 1 }, { hw - 1, y1, z - hw + 1 }, { hw - 1, y1, z + hw - 1 }, { -hw + 1, y1, z + hw - 1 } };
	int i;
	mk_material(ramp);
	glBegin(GL_TRIANGLES);
	quad(t[0], t[3], t[2], t[1]);
	for (i = 1; i < 4; i++) {         /* no face ahead: seen from behind */
		int j = (i + 1) & 3;
		quad(b[i], t[i], t[j], b[j]);
	}
	glEnd();
}

static void driver(int r) {
	const racer_t *k = &kt_racer[r];
	switch (r) {
	case 0:                          /* the mouse: grey, pink ears and nose */
		head(k->ramp, 4, 8, 15, 4);
		mk_material(k->ramp2);
		glBegin(GL_TRIANGLES);
		card(-3, 14, 3, -7, 19, 4, -6, 13, 4);
		card(3, 14, 3, 6, 13, 4, 7, 19, 4);
		glEnd();
		break;
	case 1:                          /* the cat: orange, pointed ears */
		head(k->ramp, 5, 8, 16, 4);
		mk_material(k->ramp);
		glBegin(GL_TRIANGLES);
		card(-4, 15, 2, -4, 20, 3, -1, 15, 2);
		card(1, 15, 2, 4, 20, 3, 4, 15, 2);
		glEnd();
		break;
	case 2:                          /* the frog: green, wide, eyes on top */
		head(k->ramp, 5, 8, 13, 4);
		mk_material(k->ramp2);
		glBegin(GL_TRIANGLES);
		card(-4, 13, 1, -2, 16, 1, -1, 13, 1);
		card(1, 13, 1, 2, 16, 1, 4, 13, 1);
		glEnd();
		break;
	default:                         /* the owl: brown, tufts */
		head(k->ramp, 5, 8, 17, 4);
		mk_material(k->ramp2);
		glBegin(GL_TRIANGLES);
		card(-4, 16, 2, -5, 19, 3, -2, 16, 2);
		card(2, 16, 2, 5, 19, 3, 4, 16, 2);
		glEnd();
		break;
	}
}

void kt_make(void) {
	int r;
	kt_list = glGenLists(KT_LISTS);
	if (!kt_list) return;
	for (r = 0; r < RACERS; r++) {
		glNewList(kt_list + KT_NEAR(r), GL_COMPILE);
		ntri = 0;
		matchbox(kt_racer[r].box, false);
		driver(r);
		glEndList();
		kt_tris[KT_NEAR(r)] = (uint16_t)ntri;
		glNewList(kt_list + KT_FAR(r), GL_COMPILE);
		ntri = 0;
		matchbox(kt_racer[r].box, true);
		mk_material(kt_racer[r].ramp);
		glBegin(GL_TRIANGLES);
		{
			int a[3] = { -5, 9, 9 }, b[3] = { 5, 9, 9 }, c[3] = { 4, 16, 9 }, d[3] = { -4, 16, 9 };
			quad(a, b, c, d);
		}
		glEnd();
		glEndList();
		kt_tris[KT_FAR(r)] = (uint16_t)ntri;
	}
	/* a shadow under each */
	glNewList(kt_list + KT_SHADOW, GL_COMPILE);
	ntri = 0;
	mk_flat(C_SHADOW);
	glBegin(GL_TRIANGLES);
	{
		int a[3] = { -8, 1, 12 }, b[3] = { 8, 1, 12 }, c[3] = { 8, 1, -12 }, d[3] = { -8, 1, -12 };
		quad(a, b, c, d);
	}
	glEnd();
	glEndList();
	kt_tris[KT_SHADOW] = (uint16_t)ntri;
}
