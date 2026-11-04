/*
 * T2 (the engine that runs the whole frame) on a build host: the same
 * scenes drawn by the software strips (ZGL_CHUNKY, no engine) and by
 * T2's stream carried out by its model (tests/t2_model.c), compared
 * pixel by pixel. Phase A's model uses zgl's own arithmetic, so the
 * two should agree except where a compiled list rounds (16-bit
 * positions, normals to 2.14) or the light is applied in the model's
 * space. make -C sw/common/zgl test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "../zgl.h"
#include "../zgl_int.h"
#include "../zgl_t2.h"
#include "t2_raster.h"
#include "t2g_iss.h"

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

/* the chunky driver, stood in for: strips into soft[], words into cmd[] */
static uint8_t soft[240][320], eng[240][320];
static uint32_t *cmd;
static uint32_t ncmd;
static int engine_kind;          /* what zglc_open() reports */

int zglc_open(uint8_t flags) { return (flags & ZGL_NO_ENGINE) ? 0 : engine_kind; }
void zglc_put(uint32_t w) { if (ncmd < (1u << 20)) cmd[ncmd++] = w; }
void zglc_finish(void) {}
void zglc_strip(int32_t y0, int rows, int32_t cx0, int32_t cx1, const uint8_t *col) {
	int r;
	for (r = 0; r < rows; r++)
		memcpy(&soft[y0 + r][cx0], &col[r * 320 + cx0], (size_t)(cx1 - cx0 + 1));
}

static int bad;
static int have_fw;
#define CHECK(c, ...) do { if (!(c)) { bad++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* -- the scenes -------------------------------------------------------------- */

static GLuint l_sphere, l_cube, l_mixed, l_ring;

/* a sphere of quad strips and fans, lit (normals = positions) */
static void sphere(int r, int nlat, int nlon) {
	int i, j;
	for (i = 0; i < nlat; i++) {
		uint32_t a0 = (uint32_t)(i * 32768 / nlat) - 16384, a1 = (uint32_t)((i + 1) * 32768 / nlat) - 16384;
		glBegin(GL_QUAD_STRIP);
		for (j = 0; j <= nlon; j++) {
			uint32_t b = (uint32_t)(j * 65536 / nlon);
			GLfixed x0 = (GLfixed)(((int64_t)zglCos(a0) * zglCos(b)) >> 16), y0 = zglSin(a0), z0 = (GLfixed)(((int64_t)zglCos(a0) * zglSin(b)) >> 16);
			GLfixed x1 = (GLfixed)(((int64_t)zglCos(a1) * zglCos(b)) >> 16), y1 = zglSin(a1), z1 = (GLfixed)(((int64_t)zglCos(a1) * zglSin(b)) >> 16);
			glNormal3x(x1, y1, z1);
			glVertex3x(x1 * r, y1 * r, z1 * r);
			glNormal3x(x0, y0, z0);
			glVertex3x(x0 * r, y0 * r, z0 * r);
		}
		glEnd();
	}
}

/* the cube's normals' length: not one, where GL_NORMALIZE is to fix it */
static int nscale = 1;

static void cube(int s, bool materials) {
	static const int8_t n[6][3] = { {0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0} };
	static const int8_t q[6][4][3] = {
		{{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}},
		{{1,-1,-1},{-1,-1,-1},{-1,1,-1},{1,1,-1}},
		{{1,-1,1},{1,-1,-1},{1,1,-1},{1,1,1}},
		{{-1,-1,-1},{-1,-1,1},{-1,1,1},{-1,1,-1}},
		{{-1,1,1},{1,1,1},{1,1,-1},{-1,1,-1}},
		{{-1,-1,-1},{1,-1,-1},{1,-1,1},{-1,-1,1}}
	};
	int f, k;
	for (f = 0; f < 6; f++) {
		if (materials) { GLint m[3] = { 40 + f * 30, 60 + f * 30, 0 }; glMaterialiv(GL_FRONT, GL_COLOR_INDEXES, m); }
		glBegin(GL_QUADS);
		glNormal3x(n[f][0] * 65536 * nscale, n[f][1] * 65536 * nscale, n[f][2] * 65536 * nscale);
		glIndexi(20 + f * 30);
		for (k = 0; k < 4; k++) glVertex3i(q[f][k][0] * s, q[f][k][1] * s, q[f][k][2] * s);
		glEnd();
	}
}

/* strips, fans, triangles and a polygon, unlit, with indices */
static void mixed(void) {
	int i;
	glBegin(GL_TRIANGLE_STRIP);
	for (i = 0; i < 10; i++) { glIndexi(30 + i * 10); glVertex3i(-100 + i * 20, (i & 1) ? 30 : -10, 0); }
	glEnd();
	glBegin(GL_TRIANGLE_FAN);
	glIndexi(200); glVertex3i(0, -60, 10);
	for (i = 0; i <= 8; i++) {
		uint32_t a = (uint32_t)(i * 65536 / 16);
		glIndexi(100 + i * 12);
		glVertex3x(zglCos(a) * 50, zglSin(a) * 30 - 60 * 65536, 10 * 65536);
	}
	glEnd();
	glBegin(GL_POLYGON);
	glIndexi(77); glVertex3i(60, 40, -20);
	glIndexi(150); glVertex3i(100, 40, -20);
	glIndexi(220); glVertex3i(110, 70, -20);
	glIndexi(10); glVertex3i(80, 90, -20);
	glIndexi(120); glVertex3i(55, 70, -20);
	glEnd();
	glBegin(GL_TRIANGLES);
	glIndexi(60); glVertex3i(-120, 60, 30);
	glIndexi(90); glVertex3i(-60, 70, -30);
	glIndexi(180); glVertex3i(-90, 110, 0);
	glEnd();
}

static void ring(void) {
	int i;
	glBegin(GL_LINE_LOOP);
	for (i = 0; i < 48; i++) {
		uint32_t a = (uint32_t)i * 65536u / 48;
		glVertex3x(zglCos(a) * 170, zglSin(a * 3) * 12, zglSin(a) * 170);
	}
	glEnd();
	glBegin(GL_LINES);
	for (i = 0; i < 8; i++) {
		glVertex3i(-200 + i * 50, -80, -100 + i * 40);
		glVertex3i(-180 + i * 50, 90, 200 - i * 70);
	}
	glEnd();
}

static void lists(void) {
	l_sphere = glGenLists(4);
	l_ring = l_sphere + 3;
	glNewList(l_ring, GL_COMPILE); ring(); glEndList();
	l_cube = l_sphere + 1;
	l_mixed = l_sphere + 2;
	glNewList(l_sphere, GL_COMPILE); sphere(100, 10, 16); glEndList();
	glNewList(l_cube, GL_COMPILE); cube(30, true); glEndList();
	glNewList(l_mixed, GL_COMPILE); mixed(); glEndList();
}

typedef struct { bool smooth, depth, lit, cull, immediate, near; uint32_t angle; bool norm; } smode_t;

static void scene(const smode_t *m) {
	GLfixed light[4] = { -40000, 50000, 40000, 0 }, white[4] = { 65536, 65536, 65536, 65536 };
	GLint mat[3] = { 16 << 0, 47, 0 };
	glViewport(0, 0, 320, 240);
	glMatrixMode(GL_PROJECTION); glLoadIdentity();
	zglFrustumi(-40, 40, -30, 30, 64, 4000);
	glMatrixMode(GL_MODELVIEW); glLoadIdentity();
	glLightxv(GL_LIGHT0, GL_POSITION, light);
	glLightxv(GL_LIGHT0, GL_DIFFUSE, white);
	glEnable(GL_LIGHT0);
	glClearIndex(3);
	glShadeModel(m->smooth ? GL_SMOOTH : GL_FLAT);
	if (m->depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
	if (m->cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
	glTranslatex(0, 0, (m->near ? -150 : -500) * 65536);
	glRotatex(25 << 16, 1 << 16, 0, 0);
	/* GL_NORMALIZE, as space3d: a mirror and one scale for every model */
	if (m->norm) {
		glEnable(GL_NORMALIZE);
		glScalex(80000, 80000, -80000);
		glFrontFace(GL_CW);
	} else {
		glDisable(GL_NORMALIZE);
		glFrontFace(GL_CCW);
	}
	/* the sphere */
	if (m->lit) glEnable(GL_LIGHTING); else glDisable(GL_LIGHTING);
	glIndexi(40);
	mat[0] = 16; mat[1] = 47;
	glMaterialiv(GL_FRONT, GL_COLOR_INDEXES, mat);
	glPushMatrix();
	glRotatex((GLfixed)((m->angle * 360u) >> 16) << 16, 0, 1 << 16, 0);
	if (m->immediate) sphere(100, 10, 16); else glCallList(l_sphere);
	glPopMatrix();
	/* the cube */
	glPushMatrix();
	glRotatex((GLfixed)((m->angle * 720u) >> 16) << 16, 0, 1 << 16, 0);
	glTranslatex(150 << 16, 0, 0);
	glRotatex((GLfixed)((m->angle * 1800u) >> 16) << 16, 1 << 16, 1 << 16, 0);
	if (m->immediate) cube(30, true); else glCallList(l_cube);
	glPopMatrix();
	/* unlit pieces in front */
	glDisable(GL_LIGHTING);
	glPushMatrix();
	glTranslatex(0, 0, 120 << 16);
	if (m->immediate) mixed(); else glCallList(l_mixed);
	glPopMatrix();
	/* a ring of lines */
	glIndexi(250);
	if (m->immediate) ring(); else glCallList(l_ring);
}

/* -- the comparison --------------------------------------------------------- */

static int compare(const smode_t *m, const char *name) {
	zgl_desc_t d = { ZGL_FRAMES, 0, 0, 2048, ZGL_CHUNKY };
	t2_stats_t st;
	int x, y, diff = 0, rc;
	nscale = m->norm ? 3 : 1;
	/* software */
	engine_kind = 0;
	memset(soft, 0xa5, sizeof(soft));
	d.flags = ZGL_CHUNKY | ZGL_NO_ENGINE;
	zgl_open(&d);
	lists();
	scene(m);
	zgl_frame();
	zgl_close();
	/* T2 */
	engine_kind = 2;
	memset(eng, 0xa5, sizeof(eng));
	ncmd = 0;
	d.flags = ZGL_CHUNKY;
	zgl_open(&d);
	CHECK(zgl_info()->backend == ZGL_BACKEND_T2, "%s: backend %d", name, zgl_info()->backend);
	lists();
	scene(m);
	zgl_frame();
	zgl_close();
	rc = t2_model_run(cmd, ncmd, eng, &st);
	CHECK(rc == 0, "%s: the model refused the stream (%u words)", name, ncmd);
	/* the RTL bench's vectors (rtl/gpu/bench/tb_zeitlos32g.v): the
	 * stream, and the records the model made of it */
	if (getenv("T2_VEC")) {
		static int nvec;
		char fn[512];
		uint32_t nm, i;
		const uint32_t *ml = t2r_list(&nm);
		FILE *f;
		snprintf(fn, sizeof(fn), "%s/s%d.hex", getenv("T2_VEC"), nvec);
		f = fopen(fn, "w");
		for (i = 0; i < ncmd; i++) fprintf(f, "%08x\n", cmd[i]);
		fclose(f);
		/* chunky VRAM as the frame leaves it (0xa5 outside the frame) */
		{
			static uint32_t vr[19200];
			int yy, xx;
			memset(vr, 0xa5, sizeof(vr));
			for (yy = st.ry0; yy <= st.ry1; yy++) for (xx = st.rx0; xx <= st.rx1; xx++) {
				uint32_t sg = (uint32_t)xx / 80, rr = (uint32_t)yy + ((sg & 1) ? 240u : 0u) + ((sg & 2) ? 480u : 0u);
				uint32_t b = rr * 80 + (uint32_t)xx % 80;
				vr[b / 4] = (vr[b / 4] & ~(0xffu << (b % 4 * 8))) | (uint32_t)eng[yy][xx] << (b % 4 * 8);
			}
			snprintf(fn, sizeof(fn), "%s/p%d.hex", getenv("T2_VEC"), nvec);
			f = fopen(fn, "w");
			for (i = 0; i < 19200; i++) fprintf(f, "%08x\n", vr[i]);
			fclose(f);
		}
		snprintf(fn, sizeof(fn), "%s/r%d.hex", getenv("T2_VEC"), nvec);
		f = fopen(fn, "w");
		fprintf(f, "%08x\n%08x\n", ncmd, nm);
		for (i = 0; i < nm; i++) fprintf(f, "%08x\n", ml[i]);
		fclose(f);
		nvec++;
	}
	/* the real firmware on zeitlos32g's simulator: the same records */
	if (have_fw) {
		static t2g_run_t r;
		uint32_t nm;
		const uint32_t *ml = t2r_list(&nm);
		memset(&r, 0, sizeof(r));
		r.in = cmd; r.in_n = ncmd;
		t2g_run(&r);
		CHECK(!r.bad && r.frames == 1, "%s: zeitlos32g: %s, %u frames", name, r.bad ? "stopped" : "ran", r.frames);
		CHECK(r.nlist == nm && !memcmp(r.list, ml, nm * 4), "%s: zeitlos32g's records differ (%u words, the model's %u)", name, r.nlist, nm);
		printf("test_t2: %-28s zeitlos32g: %llu instructions, %llu cycles\n", "", (unsigned long long)r.insns, (unsigned long long)r.cycles);
	}
	{
		int big = 0;
		for (y = 0; y < 240; y++) for (x = 0; x < 320; x++)
			if (soft[y][x] != eng[y][x]) {
				int dd = soft[y][x] - eng[y][x];
				diff++;
				if (dd > 1 || dd < -1) { if (big < 3 && getenv("T2_SHOW")) printf("  (%d,%d) %d vs %d\n", x, y, soft[y][x], eng[y][x]); big++; }
			}
		if (big) printf("test_t2: %s: %d pixels differ by more than one index\n", name, big);
	}
	printf("test_t2: %-28s %6u words, %3u draws, %4u verts, %4u tris (%3u CPU), %3u culled, %2u clipped; "
		"%5d pixels differ\n", name, ncmd, st.draws, st.verts, st.tris, st.cpu_tris, st.culled, st.clipped, diff);
	return diff;
}

int main(void) {
	static const struct { const char *name; smode_t m; int tol; } t[] = {
		/* immediate mode: everything through the CPU's TRI/LINE: exact */
		{ "immediate, smooth, depth",    { true, true, true, true, true, false, 1000 }, 0 },
		{ "immediate, flat, no depth",   { false, false, true, false, true, false, 5000 }, 0 },
		/* lists: compiled; rounding allowed for */
		{ "lists, smooth, depth",        { true, true, true, true, false, false, 1000 }, 400 },
		{ "lists, flat, depth",          { false, true, true, true, false, false, 20000 }, 400 },
		{ "lists, unlit, no cull",       { true, true, false, false, false, false, 33000 }, 400 },
		{ "lists, no depth",             { true, false, true, true, false, false, 47000 }, 400 },
		{ "lists, near clipping",        { true, true, true, false, false, true, 9000 }, 800 },
		/* GL_NORMALIZE with a scale and a mirror */
		{ "immediate, normalize",        { false, true, true, true, true, false, 14000, true }, 0 },
		{ "lists, normalize",            { false, true, true, true, false, false, 14000, true }, 400 },
		{ "lists, normalize, smooth",    { true, true, true, true, false, false, 29000, true }, 400 },
	};
	unsigned i;
	cmd = malloc((1u << 20) * 4);
	have_fw = t2g_load("../../t2fw/t2fw.bin") == 0;
	if (have_fw) t2g_reset();
	else printf("test_t2: no sw/t2fw/t2fw.bin: zeitlos32g not run\n");
	for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		int d = compare(&t[i].m, t[i].name);
		CHECK(d <= t[i].tol, "%s: %d pixels differ (allowed %d)", t[i].name, d, t[i].tol);
	}
	if (have_fw && getenv("T2G_PROF")) {
		extern uint64_t t2g_prof[];
		FILE *f = fopen(getenv("T2G_PROF"), "w");
		unsigned k;
		for (k = 0; k < 3072; k++) if (t2g_prof[k]) fprintf(f, "%08x %llu\n", k * 4, (unsigned long long)t2g_prof[k]);
		fclose(f);
	}
	printf("test_t2: %d failed\n", bad);
	free(cmd);
	return bad != 0;
}
