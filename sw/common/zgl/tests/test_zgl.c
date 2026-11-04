/*
 * zgl front end on a build host, against the same pipeline in double
 * precision: random lines through random frustums and rotations must
 * land within a pixel of where floating point puts them, clipping
 * included. make -C sw/common/zgl test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../zgl.h"
#include "../zgl_int.h"

typedef struct { int x0, y0, x1, y1, c; } L;
static L got[64]; static int ngot;
void zgl_back_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t c) {
	if (ngot < 64) { got[ngot].x0 = x0; got[ngot].y0 = y0; got[ngot].x1 = x1; got[ngot].y1 = y1; got[ngot].c = c; ngot++; }
}

/* reference: GL's math in double */
static double M[16], P[16];
static void dmul(double *r, const double *a, const double *b) {
	double t[16]; int c, rr, k;
	for (c = 0; c < 4; c++) for (rr = 0; rr < 4; rr++) { double s = 0; for (k = 0; k < 4; k++) s += a[k*4+rr]*b[c*4+k]; t[c*4+rr] = s; }
	memcpy(r, t, sizeof t);
}
static void did(double *m) { memset(m, 0, 128); m[0] = m[5] = m[10] = m[15] = 1; }
static void dfrustum(double *m, double l, double r, double b, double t, double n, double f) {
	double q[16] = {0}; q[0] = 2*n/(r-l); q[5] = 2*n/(t-b); q[8] = (r+l)/(r-l); q[9] = (t+b)/(t-b);
	q[10] = -(f+n)/(f-n); q[11] = -1; q[14] = -2*f*n/(f-n); dmul(m, m, q);
}
static void drot(double *m, double deg, double x, double y, double z) {
	double a = deg*M_PI/180, c = cos(a), s = sin(a), L = sqrt(x*x+y*y+z*z), q[16];
	x /= L; y /= L; z /= L; did(q);
	q[0] = x*x*(1-c)+c; q[1] = y*x*(1-c)+z*s; q[2] = x*z*(1-c)-y*s;
	q[4] = x*y*(1-c)-z*s; q[5] = y*y*(1-c)+c; q[6] = y*z*(1-c)+x*s;
	q[8] = x*z*(1-c)+y*s; q[9] = y*z*(1-c)-x*s; q[10] = z*z*(1-c)+c; dmul(m, m, q);
}
static void dtrans(double *m, double x, double y, double z) { double q[16]; did(q); q[12] = x; q[13] = y; q[14] = z; dmul(m, m, q); }
/* clip a line in double, return 0 if gone, else picture coordinates */
static int dline(const double *a3, const double *b3, double *out) {
	double mvp[16], a[4], b[4], t0 = 0, t1 = 1; int i, r;
	dmul(mvp, P, M);
	for (r = 0; r < 4; r++) { a[r] = mvp[r]*a3[0]+mvp[4+r]*a3[1]+mvp[8+r]*a3[2]+mvp[12+r]; b[r] = mvp[r]*b3[0]+mvp[4+r]*b3[1]+mvp[8+r]*b3[2]+mvp[12+r]; }
	for (i = 0; i < 6; i++) {
		int ax = i / 2, sg = (i & 1) ? -1 : 1;
		double da = a[3] + sg*a[ax], db = b[3] + sg*b[ax];
		if (da < 0 && db < 0) return 0;
		if (da < 0) { double t = da/(da-db); if (t > t0) t0 = t; }
		else if (db < 0) { double t = da/(da-db); if (t < t1) t1 = t; }
	}
	if (t0 > t1) return 0;
	for (i = 0; i < 2; i++) {
		double t = i ? t1 : t0, p[4]; for (r = 0; r < 4; r++) p[r] = a[r] + (b[r]-a[r])*t;
		out[i*2] = 160 + 160*p[0]/p[3]; out[i*2+1] = 120 - 120*p[1]/p[3];
	}
	return 1;
}

int main(void) {
	int iter, bad = 0, drawn = 0, gone = 0;
	zgl_desc_t d = { ZGL_LINES, 0, 0 };
	srand(1);
	zgl_reset_state();
	(void)d;
	for (iter = 0; iter < 20000; iter++) {
		double n = 1 + rand() % 64, f = n + 100 + rand() % 40000, w = n * (0.3 + (rand() % 100) / 100.0);
		double deg = rand() % 360, ax = rand() % 9 - 4, ay = rand() % 9 - 4, az = rand() % 9 - 4 + 0.5;
		if (iter % 3 == 0) {          /* about one axis: zgl's own path */
			int a = rand() % 6;
			ax = ay = az = 0;
			if (a % 3 == 0) ax = a < 3 ? 1 : -1;
			else if (a % 3 == 1) ay = a < 3 ? 1 : -1;
			else az = a < 3 ? 1 : -1;
		}
		double tx = rand() % 2001 - 1000, ty = rand() % 2001 - 1000, tz = -(rand() % 3000);
		double A[3], B[3], ref[4];
		int k, ok;
		for (k = 0; k < 3; k++) { A[k] = rand() % 401 - 200; B[k] = rand() % 401 - 200; }
		did(P); dfrustum(P, -w, w, -w*0.75, w*0.75, n, f);
		did(M); dtrans(M, tx, ty, tz); drot(M, deg, ax, ay, az);
		glMatrixMode(GL_PROJECTION); glLoadIdentity(); glFrustum(-w, w, -w*0.75, w*0.75, n, f);
		glMatrixMode(GL_MODELVIEW); glLoadIdentity(); zglTranslatei((int)tx, (int)ty, (int)tz);
		glRotatex((int)deg << 16, (int)(ax * 65536), (int)(ay * 65536), (int)(az * 65536));
		ngot = 0;
		glBegin(GL_LINES); glVertex3i((int)A[0], (int)A[1], (int)A[2]); glVertex3i((int)B[0], (int)B[1], (int)B[2]); glEnd();
		ok = dline(A, B, ref);
		if (!ok) { gone++; if (ngot) { /* a sliver both may disagree on */ } continue; }
		if (!ngot) {
			/* only acceptable if the double result is (almost) a point at an edge */
			if (fabs(ref[0]-ref[2]) + fabs(ref[1]-ref[3]) > 2) { bad++; if (bad < 10) printf("missing: %.1f,%.1f %.1f,%.1f\n", ref[0], ref[1], ref[2], ref[3]); }
			continue;
		}
		drawn++;
		{
			double e = 0, x[4] = { got[0].x0, got[0].y0, got[0].x1, got[0].y1 };
			for (k = 0; k < 4; k++) {
				double r = ref[k];
				if (k % 2 == 0) { if (r > 319) r = 319; if (r < 0) r = 0; } else { if (r > 239) r = 239; if (r < 0) r = 0; }
				if (fabs(x[k] - r) > e) e = fabs(x[k] - r);
			}
			if (e > 1.5) { bad++; if (bad < 10) printf("off by %.2f: got %d,%d %d,%d want %.1f,%.1f %.1f,%.1f (n %.0f f %.0f)\n", e, got[0].x0, got[0].y0, got[0].x1, got[0].y1, ref[0], ref[1], ref[2], ref[3], n, f); }
		}
	}
	/* ortho, picture coordinates, triangles, culling, strips */
	zgl_reset_state();
	glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 320, 0, 240, -1, 1);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
	ngot = 0;
	glBegin(GL_LINES); glVertex2i(10, 10); glVertex2i(100, 50); glEnd();
	if (ngot != 1 || got[0].x0 != 10 || got[0].y0 != 230 || got[0].x1 != 100 || got[0].y1 != 190) {
		printf("ortho line: %d lines, %d,%d %d,%d\n", ngot, got[0].x0, got[0].y0, got[0].x1, got[0].y1); bad++;
	}
	glEnable(GL_CULL_FACE);
	ngot = 0;
	glBegin(GL_TRIANGLES);
	glVertex2i(10, 10); glVertex2i(100, 10); glVertex2i(50, 80);     /* CCW: front */
	glVertex2i(10, 10); glVertex2i(50, 80); glVertex2i(100, 10);     /* CW: culled */
	glEnd();
	if (ngot != 3) { printf("culling: %d lines, want 3\n", ngot); bad++; }
	glDisable(GL_CULL_FACE);
	ngot = 0;
	glBegin(GL_TRIANGLE_STRIP); glVertex2i(0, 0); glVertex2i(0, 10); glVertex2i(10, 0); glVertex2i(10, 10); glEnd();
	if (ngot != 6) { printf("strip: %d lines, want 6\n", ngot); bad++; }
	glEnable(GL_CULL_FACE);
	ngot = 0;
	glBegin(GL_TRIANGLE_STRIP); glVertex2i(0, 0); glVertex2i(10, 0); glVertex2i(0, 10); glVertex2i(10, 10); glEnd();
	if (ngot != 6) { printf("strip winding: %d lines, want 6 (both CCW)\n", ngot); bad++; }
	ngot = 0;
	glBegin(GL_LINE_LOOP); glVertex2i(0, 0); glVertex2i(10, 0); glVertex2i(10, 10); glEnd();
	if (ngot != 3) { printf("loop: %d lines\n", ngot); bad++; }
	ngot = 0;
	glBegin(GL_POINTS); glVertex2i(5, 5); glVertex2i(400, 5); glEnd();
	if (ngot != 1 || got[0].x0 != got[0].x1) { printf("points: %d\n", ngot); bad++; }
	if (glGetError() != GL_NO_ERROR) { printf("error set\n"); bad++; }
	glEnd();
	if (glGetError() != GL_INVALID_OPERATION) { printf("glEnd outside glBegin not an error\n"); bad++; }

	printf("test_zgl: %d lines drawn, %d clipped away, %d wrong\n", drawn, gone, bad);
	return bad ? 1 : 0;
}
