/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * gldemo -- zgl's own demo and test scene (docs/zgl.md, docs/gldemo_app.md):
 * a lit, smooth-shaded sphere, a flat-shaded cube going round it and a
 * ring of lines, depth-tested, in ZGL_FRAMES; on the arcade runtime,
 * so in a window or full screen, in colour or black and white.
 *
 *   > run gldemo
 *
 *   > run gldemo chunky        full screen, chunky: the triangle engine
 *   > run gldemo chunky soft   the same, drawn in software, to compare
 *
 * Keys: W wireframe and solid, S smooth and flat, D depth test on and
 * off, L lighting on and off, P 256, 64 or 16 colours in full screen,
 * Space pause, F11 full screen (C there: colour or black and white),
 * F1 help.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zfont.h"
#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/games/zarcade.h"
#include "../../common/zgl/zgl.h"

/* -- the sphere: an icosahedron, each face cut in four twice -------------- */

#define SPH_R      100
#define SPH_MAXV   200
#define SPH_MAXF   320

static int16_t sph_v[SPH_MAXV][3];     /* on a sphere of radius 4096 */
static uint16_t sph_f[SPH_MAXF][3];
static int sph_nv, sph_nf;

static uint32_t isqrt32(uint32_t v) {
	uint32_t r = 0, b = 1u << 30;
	while (b > v) b >>= 2;
	while (b) {
		if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
		else r >>= 1;
		b >>= 2;
	}
	return r;
}

static int sph_vertex(int32_t x, int32_t y, int32_t z) {
	uint32_t len = isqrt32((uint32_t)(x * x + y * y + z * z));
	int i;
	x = x * 4096 / (int32_t)len; y = y * 4096 / (int32_t)len; z = z * 4096 / (int32_t)len;
	for (i = 0; i < sph_nv; i++)
		if (sph_v[i][0] == x && sph_v[i][1] == y && sph_v[i][2] == z) return i;
	sph_v[sph_nv][0] = (int16_t)x; sph_v[sph_nv][1] = (int16_t)y; sph_v[sph_nv][2] = (int16_t)z;
	return sph_nv++;
}

static void build_sphere(void) {
	/* the icosahedron: (0, +-1, +-g), (+-1, +-g, 0), (+-g, 0, +-1), g the golden ratio */
	static const int8_t ic[12][3] = {
		{-1, 2, 0}, {1, 2, 0}, {-1, -2, 0}, {1, -2, 0},
		{0, -1, 2}, {0, 1, 2}, {0, -1, -2}, {0, 1, -2},
		{2, 0, -1}, {2, 0, 1}, {-2, 0, -1}, {-2, 0, 1}
	};
	static const uint8_t icf[20][3] = {
		{0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
		{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1}
	};
	static uint16_t tmp[SPH_MAXF][3];
	int i, lev;
	/* golden ratio 1.618 as 1618/1000 on the "2"s */
	for (i = 0; i < 12; i++)
		sph_vertex(ic[i][0] * (ic[i][0] == 2 || ic[i][0] == -2 ? 809 : 1000),
		           ic[i][1] * (ic[i][1] == 2 || ic[i][1] == -2 ? 809 : 1000),
		           ic[i][2] * (ic[i][2] == 2 || ic[i][2] == -2 ? 809 : 1000));
	for (i = 0; i < 20; i++) { sph_f[i][0] = icf[i][0]; sph_f[i][1] = icf[i][1]; sph_f[i][2] = icf[i][2]; }
	sph_nf = 20;
	for (lev = 0; lev < 2; lev++) {
		int n = 0;
		for (i = 0; i < sph_nf; i++) {
			int a = sph_f[i][0], b = sph_f[i][1], c = sph_f[i][2];
			int ab = sph_vertex(sph_v[a][0] + sph_v[b][0], sph_v[a][1] + sph_v[b][1], sph_v[a][2] + sph_v[b][2]);
			int bc = sph_vertex(sph_v[b][0] + sph_v[c][0], sph_v[b][1] + sph_v[c][1], sph_v[b][2] + sph_v[c][2]);
			int ca = sph_vertex(sph_v[c][0] + sph_v[a][0], sph_v[c][1] + sph_v[a][1], sph_v[c][2] + sph_v[a][2]);
			uint16_t f[4][3] = { {a, ab, ca}, {b, bc, ab}, {c, ca, bc}, {ab, bc, ca} };
			memcpy(&tmp[n], f, sizeof(f));
			n += 4;
		}
		memcpy(sph_f, tmp, (size_t)n * sizeof(sph_f[0]));
		sph_nf = n;
	}
}

/* -- palette ramps ---------------------------------------------------------- */

static int ramp_len, ramp_sph, ramp_cube, ramp_ring;

static void palette(void) {
	int n = ar_color() ? ar_colours() : 16;
	ar_pal(0, 0x000);
	/* 1..14 for the 16-colour picture (15 is the text); above that,
	 * the ramps start at 16 and leave the runtime's 16 alone */
	if (n >= 256) { ramp_len = 32; ramp_sph = 16; }
	else if (n >= 64) { ramp_len = 16; ramp_sph = 16; }
	else { ramp_len = 4; ramp_sph = 1; }
	ramp_cube = ramp_sph + ramp_len;
	ramp_ring = ramp_cube + ramp_len;
	zglRamp(ramp_sph, ramp_len, 0x013, 0x8cf);
	zglRamp(ramp_cube, ramp_len, 0x200, 0xfc6);
	zglRamp(ramp_ring, ramp_len, 0x020, 0x9f9);
	ar_pal(15, 0xfff);      /* text: the last entry of the 16 */
}

/* -- state ------------------------------------------------------------------- */

static bool wire, flat, nodepth, unlit, paused;
static uint32_t angle;                   /* 1/65536 turns */
static uint32_t fps_frames, fps_ticks, fps10;
static uint32_t prims;

static void material(int ramp) {
	GLint idx[3] = { ramp, ramp + ramp_len - 1, ramp + ramp_len - 1 };
	glMaterialiv(GL_FRONT_AND_BACK, GL_COLOR_INDEXES, idx);
	glIndexi(ramp + ramp_len - 1);
}

/* The sphere's triangles, made once into a display list (docs/zgl.md,
 * "Display lists"): its 960 normals and vertices are not sent again
 * each frame. The material stays outside the list, since the ramps
 * move with the colour count. */
static GLuint sphere_list;

static void sphere_triangles(void) {
	int i, k;
	glBegin(GL_TRIANGLES);
	for (i = 0; i < sph_nf; i++)
		for (k = 0; k < 3; k++) {
			const int16_t *v = sph_v[sph_f[i][k]];
			glNormal3x(v[0] * 16, v[1] * 16, v[2] * 16);
			glVertex3x(v[0] * 16 * SPH_R, v[1] * 16 * SPH_R, v[2] * 16 * SPH_R);
		}
	glEnd();
}

static void draw_sphere(void) {
	material(ramp_sph);
	if (sphere_list) glCallList(sphere_list);
	else sphere_triangles();
}

static GLuint cube_list;

static void cube_quads(void) {
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
	glBegin(GL_QUADS);
	for (f = 0; f < 6; f++) {
		glNormal3i(n[f][0], n[f][1], n[f][2]);
		for (k = 0; k < 4; k++) glVertex3i(q[f][k][0] * 30, q[f][k][1] * 30, q[f][k][2] * 30);
	}
	glEnd();
}

static GLuint ring_list;

static void ring_loop(void) {
	int i;
	glBegin(GL_LINE_LOOP);
	for (i = 0; i < 48; i++) {
		uint32_t a = (uint32_t)i * 65536u / 48;
		glVertex3x(zglCos(a) * 170, (zglSin(a * 3) * 12), zglSin(a) * 170);
	}
	glEnd();
}

/* the models are display lists: on T2 (the engine that runs the whole
 * frame) a list is compiled once and sent as it is (docs/zgl.md) */
static void draw_cube(void) {
	material(ramp_cube);
	if (cube_list) glCallList(cube_list);
	else cube_quads();
}

static void make_lists(void) {
	sphere_list = glGenLists(3);
	if (!sphere_list) return;
	cube_list = sphere_list + 1;
	ring_list = sphere_list + 2;
	glNewList(ring_list, GL_COMPILE);
	ring_loop();
	glEndList();
	glNewList(sphere_list, GL_COMPILE);
	sphere_triangles();
	glEndList();
	glNewList(cube_list, GL_COMPILE);
	cube_quads();
	glEndList();
}

static void draw_ring(void) {
	glDisable(GL_LIGHTING);
	glIndexi(ramp_ring + ramp_len - 1);
	if (ring_list) glCallList(ring_list);
	else ring_loop();
	if (!unlit) glEnable(GL_LIGHTING);
}

/* chunky's measurements: cycles and instructions in the scene's GL
 * calls (the front end), in zgl_frame() (commands, or the software
 * strips), and waiting in glFinish() */
static bool measure;
static uint32_t m_cs, m_is, m_cf, m_if, m_cw;
#ifndef AR_HOST_TESTS
static inline uint32_t rd_cycle(void) { uint32_t v; __asm__ volatile ("rdcycle %0" : "=r"(v)); return v; }
static inline uint32_t rd_instret(void) { uint32_t v; __asm__ volatile ("rdinstret %0" : "=r"(v)); return v; }
#else
static inline uint32_t rd_cycle(void) { return 0; }
static inline uint32_t rd_instret(void) { return 0; }
#endif

static void render(void) {
	uint32_t c0 = 0, i0 = 0, c1, i1;
	if (measure) { c0 = rd_cycle(); i0 = rd_instret(); }
	glPolygonMode(GL_FRONT_AND_BACK, wire ? GL_LINE : GL_FILL);
	glShadeModel(flat ? GL_FLAT : GL_SMOOTH);
	if (nodepth) glDisable(GL_DEPTH_TEST); else glEnable(GL_DEPTH_TEST);
	if (unlit) glDisable(GL_LIGHTING); else glEnable(GL_LIGHTING);

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatex(0, 0, -640 * 65536);
	glRotatex(25 << 16, 1 << 16, 0, 0);                  /* look down a little */

	glPushMatrix();
	glRotatex((int32_t)((uint64_t)angle * 360 >> 16) << 16, 0, 1 << 16, 0);
	draw_sphere();
	glPopMatrix();

	glPushMatrix();
	glRotatex((int32_t)((uint64_t)angle * 2 * 360 >> 16) << 16, 0, 1 << 16, 0);
	glTranslatex(170 << 16, 0, 0);
	glRotatex((int32_t)((uint64_t)angle * 5 * 360 >> 16) << 16, 1 << 16, 1 << 16, 0);
	draw_cube();
	glPopMatrix();

	draw_ring();

	if (measure) { c1 = rd_cycle(); i1 = rd_instret(); m_cs += c1 - c0; m_is += i1 - i0; c0 = c1; i0 = i1; }
	zgl_frame();
	if (measure) { c1 = rd_cycle(); i1 = rd_instret(); m_cf += c1 - c0; m_if += i1 - i0; c0 = c1; }
	glFinish();
	if (measure) m_cw += rd_cycle() - c0;
	prims = zgl_info()->prims;
}

static void hud(void) {
	char buf[80];
	snprintf(buf, sizeof(buf), "%lu.%lu FPS  %lu PRIMS  %d COLOURS  %s %s%s%s",
		(unsigned long)(fps10 / 10), (unsigned long)(fps10 % 10), (unsigned long)prims, ar_colours(),
		wire ? "WIRE" : "SOLID", flat ? "FLAT" : "SMOOTH",
		nodepth ? " NODEPTH" : "", unlit ? " UNLIT" : "");
	ar_text(4, 230, buf, 15, &z_font_5x8, 1);
}

static const char help_text[] =
	"A lit sphere, a cube going round it and a ring of lines,\n"
	"drawn by zgl (docs/zgl.md).\n\n"
	"W\twireframe / solid\n"
	"S\tsmooth / flat shading\n"
	"D\tdepth test on / off\n"
	"L\tlighting on / off\n"
	"P\t256, 64 or 16 colours\n"
	"Space\tpause\n"
	"F11\tfull screen\n"
	"C\tin full screen: colour or black and white\n";

static ar_desc_t desc = {
	.name = "gldemo",
	.help_title = "gldemo help",
	.help_text = help_text,
	.help_doc = "gldemo_app",
	.palette = { [15] = 0xfff },
	.ink = { [15] = { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } } },
};

/* -- chunky: the whole screen, no arcade runtime (as Doom) -------------- *
 *
 * zgl's ZGL_CHUNKY: frames go straight into chunky VRAM, drawn by the
 * triangle engine where the bitstream has one (docs/gpu_tri.md), else
 * by the software strips. No text on the picture: the frame rate and
 * what a frame costs go to the console every two seconds. Esc ends it;
 * Super+Esc too (wm takes the screen back). */

#ifndef AR_HOST_TESTS
static void ch_ramp(int first, int n, uint16_t dark, uint16_t light) {
	int i, k;
	for (i = 0; i < n; i++) {
		uint32_t c = 0;
		for (k = 0; k < 12; k += 4) {
			int a = (dark >> k) & 15, b = (light >> k) & 15;
			c |= (uint32_t)(a + (b - a) * i / (n > 1 ? n - 1 : 1)) << k;
		}
		z_color_set_palette((uint32_t)(first + i), c);
	}
}

static inline uint32_t ch_cycle(void) {
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

static int chunky_main(bool soft) {
	zgl_desc_t zd = { ZGL_FRAMES, 0, 0, 1024, ZGL_CHUNKY };
	GLfixed light[4] = { -40000, 50000, 40000, 0 };
	GLfixed white[4] = { 65536, 65536, 65536, 65536 };
	uint32_t y, t0, frames = 0, cyc = 0, last;
	static uint32_t black[Z_CHUNKY_SEG_WORDS];
	bool quit = false;
	if (!z_color_chunky_available()) {
		printf("gldemo: no chunky game mode on this board\n");
		return 1;
	}
	if (soft) zd.flags |= ZGL_NO_ENGINE;
	build_sphere();
	for (y = 0; y < Z_CHUNKY_H; y++) {
		int sg;
		for (sg = 0; sg < 4; sg++) {
			volatile uint32_t *d = z_chunky_seg(y, (uint32_t)sg);
			int i;
			for (i = 0; i < Z_CHUNKY_SEG_WORDS; i++) d[i] = black[i];
		}
	}
	z_game_set_enabled(true, false);
	z_game_set_view(0, 0);
	z_color_set_chunky(true);
	if (!zgl_open(&zd)) {
		z_color_set_chunky(false);
		z_game_set_enabled(false, false);
		printf("gldemo: no memory\n");
		return 1;
	}
	printf("gldemo: chunky, %s\n",
		zgl_info()->backend == ZGL_BACKEND_T2 ? "T2, the engine that runs the whole frame" :
		zgl_info()->backend == ZGL_BACKEND_ENGINE ? "the triangle engine" : "software");
	ramp_len = 32; ramp_sph = 16; ramp_cube = 48; ramp_ring = 80;
	z_color_set_palette(0, 0x000);
	ch_ramp(ramp_sph, ramp_len, 0x013, 0x8cf);
	ch_ramp(ramp_cube, ramp_len, 0x200, 0xfc6);
	ch_ramp(ramp_ring, ramp_len, 0x020, 0x9f9);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	zglFrustumi(-40, 40, -30, 30, 100, 2000);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glLightxv(GL_LIGHT0, GL_POSITION, light);
	glLightxv(GL_LIGHT0, GL_DIFFUSE, white);
	glEnable(GL_LIGHT0);
	glEnable(GL_CULL_FACE);
	glClearIndex(0);
	make_lists();

	measure = true;
	t0 = z_uptime_ticks();
	last = z_game_frame();
	while (!quit) {
		z_msg_t m;
		uint32_t c0, f;
		while (z_msg_read(&m) == Z_OK) {
			if (m.subject == Z_WM_GAME_REVOKED) quit = true;
			if (m.subject == Z_WM_KEY && m.obj.type == Z_UINT32 &&
			    Z_WM_UNPACK_KEY_PRESSED(m.obj.val.uint32)) {
				switch (Z_WM_UNPACK_KEY_KEYSYM(m.obj.val.uint32)) {
				case 0x1b: quit = true; break;
				case 'w': case 'W': wire = !wire; break;
				case 's': case 'S': flat = !flat; break;
				case 'd': case 'D': nodepth = !nodepth; break;
				case 'l': case 'L': unlit = !unlit; break;
				case ' ': paused = !paused; break;
				default: break;
				}
			}
		}
		c0 = ch_cycle();
		render();
		cyc += ch_cycle() - c0;
		frames++;
		/* the clock: 60 steps a second of the turn, whatever the rate */
		f = z_game_frame();
		if (!paused) angle += (uint16_t)(f - last) * 200u;
		last = f;
		if (z_uptime_ticks() - t0 >= 2u * Z_TICK_HZ) {
			uint32_t dt = z_uptime_ticks() - t0;
			printf("gldemo: %lu.%lu fps, %lu cycles a frame drawing (%lu prims)\n",
				(unsigned long)(frames * Z_TICK_HZ / dt),
				(unsigned long)(frames * Z_TICK_HZ * 10 / dt % 10),
				(unsigned long)(cyc / frames), (unsigned long)prims);
			printf("gldemo:   scene %lu cycles %lu instructions; zgl_frame %lu cycles %lu instructions; "
				"glFinish %lu cycles\n",
				(unsigned long)(m_cs / frames), (unsigned long)(m_is / frames),
				(unsigned long)(m_cf / frames), (unsigned long)(m_if / frames),
				(unsigned long)(m_cw / frames));
			m_cs = m_is = m_cf = m_if = m_cw = 0;
			frames = 0; cyc = 0;
			t0 = z_uptime_ticks();
		}
	}
	glFinish();
	zgl_close();
	z_color_set_chunky(false);
	z_game_set_enabled(false, false);
	/* Chunky's segments 2 and 3 are the second sheet, which the desktop
	 * reads as its accent bits when it is in colour (docs/color.md,
	 * "Desktop colour"): left as they are, the picture's bytes would
	 * tint the desktop in stripes. Cleared, then wm repaints. */
	for (y = 0; y < Z_CHUNKY_H; y++) {
		int sg;
		for (sg = 0; sg < 4; sg++) {
			volatile uint32_t *d = z_chunky_seg(y, (uint32_t)sg);
			int i;
			for (i = 0; i < Z_CHUNKY_SEG_WORDS; i++) d[i] = 0;
		}
	}
	{
		uint32_t wm_pid;
		if (z_pid_lookup("wm0", &wm_pid))
			z_msg_new_send(wm_pid, Z_WM_REPAINT, 0, z_obj_uint32(0));
	}
	return 0;
}
#endif

int main(void) {
	bool quit = false;
	zgl_desc_t zd = { ZGL_FRAMES, 0, 0, 1024 };
	GLfixed light[4] = { -40000, 50000, 40000, 0 };
	GLfixed white[4] = { 65536, 65536, 65536, 65536 };

#ifndef AR_HOST_TESTS
	{
		char arg[32];
		if (z_launch_arg_take(arg, (int)sizeof(arg))) {
			if (!strncmp(arg, "chunky", 6))
				return chunky_main(strstr(arg, "soft") != NULL);
			/* `run gldemo soft`: the window in software even where the
			 * triangle engine is, to compare */
			if (strstr(arg, "soft")) zd.flags |= ZGL_NO_ENGINE;
		}
	}
#endif
	build_sphere();
	if (!ar_init(&desc)) { printf("gldemo: no window\n"); return 1; }
	ar_set_colours(256);
	if (!zgl_open(&zd)) { printf("gldemo: no memory\n"); ar_shutdown(); return 1; }
	palette();

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	zglFrustumi(-40, 40, -30, 30, 100, 2000);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glLightxv(GL_LIGHT0, GL_POSITION, light);
	glLightxv(GL_LIGHT0, GL_DIFFUSE, white);
	glEnable(GL_LIGHT0);
	glEnable(GL_CULL_FACE);
	glClearIndex(0);
	make_lists();
	fps_ticks = ar_ticks();

	while (!quit) {
		ar_event_t e;
		uint32_t dt;
		while (ar_poll(&e)) {
			if (e.type == AR_EV_QUIT) quit = true;
			if (e.type == AR_EV_MODE) palette();
			if (e.type == AR_EV_KEY && e.pressed) {
				switch (e.key) {
				case 'w': case 'W': wire = !wire; break;
				case 's': case 'S': flat = !flat; break;
				case 'd': case 'D': nodepth = !nodepth; break;
				case 'l': case 'L': unlit = !unlit; break;
				case 'p': case 'P':
					ar_set_colours(ar_colours() == 256 ? 64 : ar_colours() == 64 ? 16 : 256);
					palette();
					break;
				case ' ': paused = !paused; break;
				default: break;
				}
			}
		}
		if (quit) break;
		render();
		hud();
		dt = ar_frame();
		if (!paused) angle += dt * 200;
		fps_frames++;
		if (ar_ticks() - fps_ticks >= 60) {
			fps10 = fps_frames * 600 / (ar_ticks() - fps_ticks);
			fps_frames = 0;
			fps_ticks = ar_ticks();
		}
	}
	zgl_close();
	ar_shutdown();
	return 0;
}
