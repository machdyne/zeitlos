/*
 * Zeitlos -- gpu3d
 *
 * Wireframe 3D viewer. Spins a cube by default; opens STL files from
 * the SD card through the titlebar's open icon.
 *
 * ============================================================
 * What this app is for
 * ============================================================
 *
 * It exercises the GPU line rasterizer (rtl/gpu/gpu_raster.v) the way
 * a real application would -- hundreds of clipped line commands per
 * frame, every frame, from a process that is also servicing wm
 * messages and reading a file. The frame counter in the top-left
 * corner is the point of the whole thing: it is the number that says
 * whether a change to the rasterizer, the blitter, the cache or the
 * CPU made anything faster.
 *
 * ============================================================
 * Drawing, and the two erase strategies
 * ============================================================
 *
 * There is no double buffer. The framebuffer is a single 1bpp surface
 * that is being scanned out continuously, so every frame has to
 * remove the previous one and draw the new one in place.
 *
 * Two ways to remove the previous frame, and which is cheaper depends
 * entirely on the model:
 *
 *   INCREMENTAL -- redraw last frame's edges in colour 0. Costs one
 *   line command per edge, and touches nothing else, so the window
 *   does not flicker. This is what the cube wants: 12 edges, 12
 *   commands.
 *
 *   CLEAR -- one blitter fill of the whole content area
 *   (z_win_fill_rect(), which goes through rtl/gpu/gpu_blit.v, not a
 *   per-pixel software loop -- see zwin.c's own comment on why that
 *   distinction mattered enough to fix). Costs one command regardless
 *   of edge count, but blanks the window each frame, which reads as
 *   flicker.
 *
 * The crossover is not close. A decimated teapot is ~900 edges, so
 * incremental erase costs 900 extra line commands against ONE fill.
 * Below INCR_MAX_EDGES the flicker-free path is affordable; above it,
 * paying 900 commands to avoid flicker would cost more frames per
 * second than the flicker is worth. Hence adaptive rather than a
 * fixed choice.
 *
 * ============================================================
 * Reading modifier keys, and why it is not from wm
 * ============================================================
 *
 * Z_WM_MOUSE (zwm.h) carries x, y, buttons and an inside flag. It
 * does NOT carry modifiers, and it cannot be made to without changing
 * the packed layout that exists specifically to avoid allocating a
 * Z_MAP at pointer-movement rates.
 *
 * Nor can the modifier state be tracked from Z_WM_KEY: wm drops bare
 * modifier changes outright (`if (keysym == Z_KEY_NONE) continue;` in
 * its dispatch_keys()), because a modifier press has no keysym of its
 * own. An app watching only Z_WM_KEY learns that Shift is held on the
 * next Shift+letter and never otherwise -- which is no use at all for
 * "hold shift and move the mouse".
 *
 * So kbd_mods() below reads the live modifier byte straight out of
 * the USB HID report register, which is level state maintained by
 * hardware (rtl/usb_hid.v) and always current. It picks the port by
 * asking which one reports itself a keyboard, exactly as wm.c's own
 * mouse_port() picks the mouse -- there is no fixed port-to-device
 * mapping, so assuming port 0 is a keyboard is wrong half the time.
 *
 * This is a legitimate use of direct hardware access, not a shortcut
 * around wm: it is READ-ONLY level state that no other process can be
 * confused by, unlike the rasterizer registers, whose shared mutable
 * state is exactly why this file no longer touches them and goes
 * through z_win_hw_line() instead.
 *
 * ============================================================
 * Spin is timed, not counted
 * ============================================================
 *
 * The old version added a fixed number of degrees per FRAME. That
 * couples rotation speed to frame rate, which has two bad
 * consequences: the cube spun far too fast (it renders at hundreds of
 * frames per second, so 5 degrees a frame is many revolutions a
 * second), and loading a heavier model would have silently slowed the
 * rotation down as the frame rate dropped.
 *
 * Angles now advance from z_uptime_ticks() (zeitlos.h, ~732Hz) at a
 * rate in degrees per SECOND, so the cube turns at the same visible
 * speed whatever the frame rate, and the same speed as a teapot.
 * Angles are kept in 1/64-degree units so a slow rate still
 * accumulates smoothly instead of being rounded to zero each frame.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zsoc.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zfont.h"
#include "../../common/zkbd.h"
#include "../../common/zflist.h"
#include "../../common/zdialog.h"

#include "model.h"
#include "stl.h"

/* ============================================================
 * window / geometry
 * ============================================================ */

/* 200x200 rather than the old 160x160: a decimated teapot has a lot
 * more going on in it than a cube, and at 160 the wireframe turns
 * into a solid blob. Resizable so it can be made bigger still -- the
 * projection scales with the content area (update_win_geometry()), so
 * enlarging the window genuinely shows more, it does not just add
 * margin. MIN_IS_CREATE keeps it from being shrunk below the size the
 * FPS readout needs. */
#define WIN_WIDTH    200
#define WIN_HEIGHT   200

/* Generous bounds for the on-screen test below. The framebuffer is
 * 512x384 on current boards but the rasterizer is specified for
 * 640x480 (docs/gpu_raster.md); this only has to reject the wild
 * coordinates a near-zero perspective divide can produce, and
 * z_win_hw_line() clips properly to the window regardless. */
#define SCREEN_WIDTH   640
#define SCREEN_HEIGHT  480

static z_win_t win;

static int win_cx, win_cy;		/* projection centre, screen coords */
static int content_w, content_h;
static fixed_t proj_d;			/* perspective distance, Q12 */

static void update_win_geometry(void) {

	z_clip_t clip;

	z_win_content_rect(&win, &clip);

	win_cx = (clip.x0 + clip.x1) / 2;
	win_cy = (clip.y0 + clip.y1) / 2;

	content_w = clip.x1 - clip.x0 + 1;
	content_h = clip.y1 - clip.y0 + 1;

	if (content_w < 1) content_w = 1;
	if (content_h < 1) content_h = 1;

	/* Derived from the content area rather than being a constant, so
	 * one setting works at every window size. A model has half-extent
	 * 1.0 by construction (model_normalize(), stl.h) and sits at
	 * z=5, so its projected half-extent is d/5 -- and up to sqrt(3)
	 * times that at the corners once it rotates. 9/10 of the smaller
	 * content dimension leaves that worst case comfortably inside the
	 * window without wasting most of it in the common case. */
	int m = (content_w < content_h) ? content_w : content_h;

	proj_d = (fixed_t)(((int32_t)m * FIXED_ONE / 10) * 9);

}

/* ============================================================
 * model
 * ============================================================ */

static model_t model;

static const int8_t cube_v[8][3] = {
	{ -1, -1, -1 }, {  1, -1, -1 }, {  1,  1, -1 }, { -1,  1, -1 },
	{ -1, -1,  1 }, {  1, -1,  1 }, {  1,  1,  1 }, { -1,  1,  1 }
};

static const uint8_t cube_e[12][2] = {
	{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 },
	{ 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 },
	{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }
};

/* Cube faces, two triangles each, wound COUNTER-CLOCKWISE seen from
 * outside -- the convention mtri_t documents and that backface culling
 * depends on. Vertices 0-3 are the z=-1 face, 4-7 the z=+1 face.
 *
 * Getting a winding backwards does not look like a winding error. The
 * face is culled when it should be drawn, so the solid gets a hole and
 * you see the inside of the far side through it. Worth checking each
 * one against cube_v above rather than trusting the pattern.
 */
static const uint8_t cube_t[12][3] = {
	{ 0, 2, 1 }, { 0, 3, 2 },      /* back   z=-1 */
	{ 4, 5, 6 }, { 4, 6, 7 },      /* front  z=+1 */
	{ 0, 1, 5 }, { 0, 5, 4 },      /* bottom y=-1 */
	{ 3, 7, 6 }, { 3, 6, 2 },      /* top    y=+1 */
	{ 0, 4, 7 }, { 0, 7, 3 },      /* left   x=-1 */
	{ 1, 2, 6 }, { 1, 6, 5 }       /* right  x=+1 */
};

/* The built-in model, and what gpu3d falls back to when a load
 * fails. Already normalized -- half-extent exactly 1.0 -- which is
 * the same contract model_normalize() gives a loaded model, so the
 * renderer never needs to know which it is looking at. */
static void load_cube(void) {

	model.nverts = 8;

	for (int i = 0; i < 8; i++) {
		model.verts[i].x = INT_TO_FIXED((int)cube_v[i][0]);
		model.verts[i].y = INT_TO_FIXED((int)cube_v[i][1]);
		model.verts[i].z = INT_TO_FIXED((int)cube_v[i][2]);
	}

	model.nedges = 12;

	for (int i = 0; i < 12; i++) {
		model.edges[i].v0 = cube_e[i][0];
		model.edges[i].v1 = cube_e[i][1];
	}

	model.ntris = 12;

	for (int i = 0; i < 12; i++) {
		model.tris[i].v0 = cube_t[i][0];
		model.tris[i].v1 = cube_t[i][1];
		model.tris[i].v2 = cube_t[i][2];
	}

	model.decimated = 0;
	strcpy(model.name, "cube");

}

/* ============================================================
 * fixed-point trig
 * ============================================================ */

static const fixed_t sin_table[90] = {
	   0,   71,  143,  214,  285,  357,  428,  499,  570,  641,
	 711,  781,  851,  921,  990, 1060, 1128, 1197, 1265, 1333,
	1400, 1468, 1534, 1600, 1665, 1730, 1795, 1859, 1922, 1985,
	2048, 2109, 2170, 2230, 2290, 2349, 2407, 2464, 2521, 2577,
	2632, 2687, 2741, 2794, 2846, 2897, 2948, 2998, 3047, 3095,
	3142, 3189, 3234, 3279, 3322, 3365, 3406, 3447, 3486, 3525,
	3562, 3598, 3633, 3668, 3701, 3733, 3764, 3794, 3823, 3851,
	3878, 3904, 3929, 3952, 3975, 3996, 4017, 4036, 4054, 4071,
	4087, 4102, 4116, 4129, 4140, 4151, 4160, 4169, 4176, 4182
};

static fixed_t fixed_sin(int deg) {

	deg %= 360;
	if (deg < 0) deg += 360;

	if (deg < 90)  return sin_table[deg];
	if (deg < 180) return sin_table[179 - deg];
	if (deg < 270) return -sin_table[deg - 180];

	return -sin_table[359 - deg];

}

static fixed_t fixed_cos(int deg) {
	return fixed_sin(deg + 90);
}

/* ============================================================
 * view state
 * ============================================================ */

/* Angles in 1/64 degree units -- see this file's header comment on
 * why rotation is timed rather than counted. */
#define ANG_FRAC   64
#define ANG_FULL   (360 * ANG_FRAC)

/* Manual rotation only: what dragging has added. The spin component
 * is NOT accumulated into these, it is computed fresh each frame from
 * spin_ticks below.
 *
 * That split is not tidiness, it is correctness. The obvious
 * implementation adds `rate * dt / TICK_HZ` to an angle every frame,
 * and at this app's frame rates that integer division truncates to
 * ZERO most frames: a frame takes 1-2 ticks (Z_TICK_HZ is 732), and
 * 5 degrees/second * 64 units * 2 ticks / 732 is 0. The slowest axis
 * would simply never move, and the others would run at whatever rate
 * the truncation happened to leave -- an error that gets worse the
 * faster the app runs, which is a deeply confusing thing to debug.
 *
 * Computing the angle from TOTAL elapsed spinning time instead does
 * the division once against a large number rather than once per frame
 * against a tiny one, so there is nothing to truncate and no drift. */
static int32_t man_x, man_y, man_z;

/* Total ticks spent spinning. Not wall-clock elapsed: pausing must
 * not silently advance the object. */
static uint32_t spin_ticks;
static uint32_t spin_last_tick;

/* Degrees per second. Slow enough to actually look at, and slow
 * enough that the three axes' beat period is long -- the old
 * per-frame steps were fast enough that the cube read as jitter
 * rather than rotation. */
#define SPIN_DPS_X   11
#define SPIN_DPS_Y   17
#define SPIN_DPS_Z    5

static bool spinning = true;

static fixed_t user_scale = FIXED_ONE;

#define SCALE_MIN   (FIXED_ONE / 8)
#define SCALE_MAX   (FIXED_ONE * 8)

static void reset_view(void) {
	man_x = man_y = man_z = 0;
	spin_ticks = 0;
	user_scale = FIXED_ONE;
	spin_last_tick = z_uptime_ticks();
}

static void advance_spin(void) {

	uint32_t now = z_uptime_ticks();
	uint32_t dt = now - spin_last_tick;

	spin_last_tick = now;

	if (!spinning) return;

	/* A long stall (a multi-second file load, during which this is
	 * not called at all) would otherwise dump the whole of that time
	 * into the next frame as one jump. */
	if (dt > Z_TICK_HZ) dt = Z_TICK_HZ;

	spin_ticks += dt;

}

/* Whole degrees for one axis: the manual offset plus however far the
 * spin has got. */
static int spin_angle(int dps, int32_t manual) {

	int64_t a = (int64_t)manual +
		(int64_t)(((uint64_t)dps * ANG_FRAC * (uint64_t)spin_ticks) / Z_TICK_HZ);

	a %= ANG_FULL;
	if (a < 0) a += ANG_FULL;

	return (int)(a / ANG_FRAC);

}

/* ============================================================
 * projection
 * ============================================================ */

static int16_t cur_x[MODEL_MAX_VERTS], cur_y[MODEL_MAX_VERTS];

/*
 * Previous frame's projection, so each edge can be erased individually
 * rather than by blanking the window.
 *
 * -- why this is now sized for the WHOLE model --
 *
 * There is no double buffer and there cannot be one without gateware:
 * VRAM is 9600 words (rtl/mem/vram.v), which is exactly 640*480/32 --
 * one frame, with nothing left over.
 *
 * So the old strategy for anything over ~96 edges was to blank the
 * content area with one blitter fill and redraw. That is far cheaper
 * in commands, and it flickers badly, for a reason the phase
 * breakdown makes obvious once measured: at 290 edges the draw phase
 * is ~31ms of a ~41ms frame, so the window is empty or half-drawn for
 * three quarters of every frame, at ~24Hz. That is squarely in the
 * band the eye is most sensitive to.
 *
 * Erasing each edge immediately before redrawing it (ERASE_INTERLEAVED
 * below) never blanks anything: at any instant exactly one edge is
 * missing, for the ~100us it takes to issue two line commands. It
 * costs one extra line command per edge -- roughly halving the frame
 * rate -- which is a trade the eye wins easily against a 75% blank
 * duty cycle.
 *
 * That means prev_x/prev_y must cover MODEL_MAX_VERTS, not the 128
 * they held when this path was reserved for small models. At int16
 * that is 1KB, which is the whole cost of not flickering.
 */
static int16_t prev_x[MODEL_MAX_VERTS], prev_y[MODEL_MAX_VERTS];
static bool prev_valid = false;

/*
 * How the previous frame gets removed.
 *
 * INTERLEAVED is the default and the one that looks right. CLEAR is
 * kept because it is genuinely faster in raw frame rate and because
 * the difference between them is a judgement about how something
 * LOOKS, which cannot be settled from the numbers alone -- press 'e'
 * to switch and decide on the actual display.
 */
#define ERASE_INTERLEAVED   0
#define ERASE_CLEAR         1

static int erase_mode = ERASE_INTERLEAVED;

/* Projected bounding box, for hit testing. */
static int bb_x0, bb_y0, bb_x1, bb_y1;
static bool bb_valid = false;

static int16_t clamp16(int v) {
	if (v < -30000) return -30000;
	if (v >  30000) return  30000;
	return (int16_t)v;
}

static bool on_screen(int x, int y) {
	return x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT;
}

static vertex3d_t rotate_vertex(vertex3d_t v, int rx, int ry, int rz) {

	fixed_t c, s;
	vertex3d_t r;

	c = fixed_cos(rx); s = fixed_sin(rx);
	r.x = v.x;
	r.y = fixed_mul(v.y, c) - fixed_mul(v.z, s);
	r.z = fixed_mul(v.y, s) + fixed_mul(v.z, c);
	v = r;

	c = fixed_cos(ry); s = fixed_sin(ry);
	r.x = fixed_mul(v.x, c) + fixed_mul(v.z, s);
	r.y = v.y;
	r.z = -fixed_mul(v.x, s) + fixed_mul(v.z, c);
	v = r;

	c = fixed_cos(rz); s = fixed_sin(rz);
	r.x = fixed_mul(v.x, c) - fixed_mul(v.y, s);
	r.y = fixed_mul(v.x, s) + fixed_mul(v.y, c);
	r.z = v.z;

	return r;

}

/*
 * Reciprocal of the perspective divisor, as a Q18 fixed-point value.
 *
 * This replaced two calls to fixed_div() per vertex, and the reason is
 * worth stating because the old code looked perfectly reasonable.
 * fixed_div() computes `((int64_t)a << 12) / b`, and rv32im has no
 * 64-bit divide -- so each one was a call into libgcc's __divdi3,
 * several hundred cycles. At two per vertex over a few hundred
 * vertices every frame, that was milliseconds per frame spent in a
 * software division routine.
 *
 * Here the numerator is the CONSTANT 2^30, which fits in a uint32
 * with room to spare, so this is a single hardware DIV instruction --
 * roughly 32 cycles on picorv32's sequential divider. One of them per
 * vertex instead of two software calls.
 *
 * Why 2^30 specifically: `zo` is Q12, so 2^30/zo yields 1/zo in Q18.
 * The subsequent multiply is Q12 * Q18 = Q30, which is why the callers
 * below shift by 30 to land on whole pixels. Q18 gives about five
 * significant decimal digits on a reciprocal of ~0.2, i.e. sub-pixel
 * accuracy across any window this SOC can display.
 *
 * The multiply stays 64-bit and that is fine: rv32im HAS a widening
 * multiply (mul/mulh), so a 64-bit product is two instructions. It is
 * only division that falls off the hardware.
 *
 * Bounds, since the shift makes them non-obvious: zo is clamped to
 * >= 0.1 below, so inv peaks near 2.6e6; the Q12 numerator peaks near
 * 2.6e7 at maximum zoom. Their product is ~6.7e13, comfortably inside
 * int64.
 */
static inline int32_t recip_q18(fixed_t zo) {
	return (int32_t)(0x40000000 / zo);
}

static void project(const vertex3d_t *v, fixed_t d, int *ox, int *oy) {

	fixed_t zo = v->z + INT_TO_FIXED(5);

	if (zo <= FLOAT_TO_FIXED(0.1)) zo = FLOAT_TO_FIXED(0.1);

	int32_t inv = recip_q18(zo);

	*ox = win_cx + (int)(((int64_t)fixed_mul(v->x, d) * inv) >> 30);
	*oy = win_cy - (int)(((int64_t)fixed_mul(v->y, d) * inv) >> 30);

}

/* ============================================================
 * performance counters
 * ============================================================
 *
 * Frame time broken down by phase, reported to the serial console
 * once a second. This exists because "4 FPS" on its own says nothing
 * about what to fix, and the components have completely different
 * remedies: the vertex loop is arithmetic, the edge loop is uncached
 * MMIO, the clear is one blitter command, and anything left over is
 * the message loop or time lost to other processes.
 *
 * READ THE NUMBERS WITH THIS CAVEAT. picorv32's rdcycle/rdinstret are
 * single GLOBAL hardware counters -- not virtualised per process, not
 * saved across context switches (see sw/os/sh.c's own note on exactly
 * this). Every figure below therefore includes cycles and
 * instructions burned by OTHER processes while this one was
 * preempted. For a "how long did the user wait" question that is the
 * honest number. For "how expensive is my edge loop" it is inflated
 * by however many other runnable processes there are, so measure with
 * a quiet system -- ideally just wm and gpu3d.
 *
 * The ratio survives that. Cycles per instruction divides out the
 * inflation (both columns are scaled by it), so CPI is comparable
 * between runs regardless of system load. A CPI far above picorv32's
 * ~4-6 means memory stalls or preemption rather than more work.
 *
 * Build with -DGPU3D_PERF=0 to compile all of this out; the macros
 * become no-ops and nothing is linked.
 */

#ifndef GPU3D_PERF
#define GPU3D_PERF 1
#endif

#if GPU3D_PERF

static inline uint32_t perf_cyc(void) {
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
}

static inline uint32_t perf_ins(void) {
	uint32_t v;
	__asm__ volatile ("rdinstret %0" : "=r"(v));
	return v;
}

static struct {

	uint32_t	frames;
	uint32_t	c_frame, c_xform, c_erase, c_draw, c_text;
	uint32_t	i_frame;
	uint32_t	edges;
	uint32_t	last_tick;

} perf;

/* Subtraction on uint32 counters is wraparound-correct, so no
 * special handling is needed when rdcycle rolls over (~89s at
 * 48MHz). */
#define PERF_MARK(v)          uint32_t v = perf_cyc()
#define PERF_ACC(field, v)    perf.field += perf_cyc() - (v)
#define PERF_FRAME_BEGIN()    uint32_t pf_c = perf_cyc(); uint32_t pf_i = perf_ins()
#define PERF_FRAME_END(n)     do { \
		perf.i_frame += perf_ins() - pf_i; \
		perf.c_frame += perf_cyc() - pf_c; \
		perf.edges += (uint32_t)(n); \
		perf.frames++; \
	} while (0)

#define PERF_CYC_PER_US   (Z_SYSCLK_HZ / 1000000u)

/* One phase of the breakdown: "name 12.3ms 45%  ". Milliseconds to
 * one decimal, computed from microseconds so there is no float
 * anywhere. */
static void perf_phase(const char *name, uint32_t c, uint32_t total) {

	uint32_t us = c / PERF_CYC_PER_US;

	printf("%s %u.%ums %u%%  ", name, us / 1000, (us / 100) % 10,
		total ? (uint32_t)(((uint64_t)c * 100) / total) : 0);

}

static void perf_report(void) {

	uint32_t now = z_uptime_ticks();
	uint32_t dt = now - perf.last_tick;

	if (dt < Z_TICK_HZ) return;

	if (perf.frames == 0) {
		perf.last_tick = now;
		return;
	}

	uint32_t f = perf.frames;

	/* Everything not attributed to a phase: the message drain, the
	 * spin update, loop overhead -- and time this process did not
	 * get. A large "other" is the signature of preemption, not of
	 * slow drawing. */
	uint32_t accounted = perf.c_xform + perf.c_erase +
		perf.c_draw + perf.c_text;
	uint32_t other = (perf.c_frame > accounted) ?
		(perf.c_frame - accounted) : 0;

	uint32_t fps100 = (uint32_t)(((uint64_t)f * 100 * Z_TICK_HZ) / dt);
	uint32_t cyc_f = perf.c_frame / f;
	uint32_t us_f = cyc_f / PERF_CYC_PER_US;

	uint32_t cpi10 = perf.i_frame ?
		(uint32_t)(((uint64_t)perf.c_frame * 10) / perf.i_frame) : 0;

	printf("gpu3d: %u.%02u fps  frame %u.%ums  %u kcyc  CPI %u.%u  erase=%s\n",
		fps100 / 100, fps100 % 100,
		us_f / 1000, (us_f / 100) % 10,
		cyc_f / 1000, cpi10 / 10, cpi10 % 10,
		erase_mode == ERASE_INTERLEAVED ? "interleaved" : "clear");

	printf("gpu3d:   ");
	perf_phase("xform", perf.c_xform / f, cyc_f);
	perf_phase("erase", perf.c_erase / f, cyc_f);
	perf_phase("draw", perf.c_draw / f, cyc_f);
	perf_phase("text", perf.c_text / f, cyc_f);
	perf_phase("other", other / f, cyc_f);
	printf("\n");

	/* The per-edge figure is the one to watch when changing
	 * MODEL_MAX_EDGES or the line-issue path: frame time is very
	 * nearly linear in it. */
	if (perf.edges) {
		uint32_t ns_edge = (uint32_t)(((uint64_t)perf.c_draw * 1000) /
			(perf.edges * (uint64_t)PERF_CYC_PER_US));
		/* Under ERASE_INTERLEAVED this is TWO line commands per edge
		 * (the erase and the draw), so expect it to be about double
		 * the clear-mode figure. That is the cost of not flickering,
		 * stated plainly rather than hidden in a separate bucket. */
		printf("gpu3d:   %u edges/frame  %u.%uus/edge (%s)  %u verts\n",
			perf.edges / f, ns_edge / 1000, (ns_edge / 100) % 10,
			erase_mode == ERASE_INTERLEAVED ? "2 cmds" : "1 cmd",
			model.nverts);
	}

	memset(&perf, 0, sizeof(perf));
	perf.last_tick = now;

}

#else

#define PERF_MARK(v)          ((void)0)
#define PERF_ACC(field, v)    ((void)0)
#define PERF_FRAME_BEGIN()    ((void)0)
#define PERF_FRAME_END(n)     ((void)0)

static void perf_report(void) { }

#endif

/* ============================================================
 * FPS
 * ============================================================ */

static uint32_t fps_frames;
static uint32_t fps_tick;
static int fps_value = 0;

#define FPS_BOX_W   44
#define FPS_BOX_H   9

static void fps_tick_update(void) {

	uint32_t now = z_uptime_ticks();
	uint32_t dt = now - fps_tick;

	fps_frames++;

	/* Twice a second: often enough to feel live, rarely enough that
	 * the integer division below is free. */
	if (dt < Z_TICK_HZ / 2) return;

	fps_value = (int)((fps_frames * Z_TICK_HZ) / dt);

	fps_frames = 0;
	fps_tick = now;

}

/* No snprintf: this runs every frame, and stdio's formatting machinery
 * is both slower and considerably larger than the four lines it would
 * replace here. */
static void fps_draw(void) {

	char buf[12];
	int n = 0;
	int v = fps_value;

	buf[n++] = 'F'; buf[n++] = 'P'; buf[n++] = 'S'; buf[n++] = ' ';

	if (v > 9999) v = 9999;

	if (v >= 1000) buf[n++] = (char)('0' + (v / 1000) % 10);
	if (v >= 100)  buf[n++] = (char)('0' + (v / 100) % 10);
	if (v >= 10)   buf[n++] = (char)('0' + (v / 10) % 10);

	buf[n++] = (char)('0' + v % 10);
	buf[n] = 0;

	z_win_fill_rect(&win, 0, 0, FPS_BOX_W, FPS_BOX_H, 0);
	z_win_draw_text(&win, 1, 1, buf, 1, &z_font_5x8);

}

/* ============================================================
 * rendering
 * ============================================================ */

static bool loading = false;

/* Solid shading, toggled with S. Off by default: wireframe is what
 * gpu3d has always been and what every imported model can still do. */
static bool shade_mode = false;



static void draw_edge(int x0, int y0, int x1, int y1, int color) {

	if (!on_screen(x0, y0) || !on_screen(x1, y1)) return;

	z_win_hw_line(&win, x0, y0, x1, y1, color);

}


/* ============================================================
 * flat shading
 * ============================================================
 *
 * Software edge-walking, hardware span fills. There is no triangle
 * rasterizer in the gateware and this does not need one: a flat-shaded
 * face is a set of horizontal spans, and a shaded span is exactly one
 * z_win_hw_fill_shade() call.
 *
 * Whether that is fast enough is the question this exists to answer.
 * If it is, a hardware rasterizer is never worth the gates.
 *
 * -- what this deliberately does NOT do --
 *
 * No Z-buffer. A 1bpp framebuffer has nowhere to put one, and 640x480
 * of depth would be more memory than the machine has spare. Faces are
 * sorted back-to-front and painted in that order, which is exact for
 * a convex solid (a cube) and wrong for interpenetrating geometry --
 * the classic painter's-algorithm failure. Acceptable here and worth
 * knowing before pointing this at a complicated mesh.
 *
 * No perspective-correct anything, no interpolation across the face.
 * Flat shading only: one grey per triangle.
 */

#define SHADE_MAX_TRIS MODEL_MAX_TRIS

/* Darkest a lit face gets. Not 0: a face turned fully away from the
 * light still has a silhouette, and painting it black makes the solid
 * look like it has a bite taken out of it against a black background. */
#define SHADE_AMBIENT 3

/* Sort keys, back to front. Depth is the sum of the three rotated z
 * values -- the centroid times three, which orders identically and
 * avoids a divide per face. */
static int32_t tri_depth[SHADE_MAX_TRIS];
static uint8_t tri_order[SHADE_MAX_TRIS];
static uint8_t tri_shade[SHADE_MAX_TRIS];
static int     tri_visible;

/* Rotated vertices, kept per frame so face normals can be computed in
 * VIEW space. cur_x/cur_y are already projected, and a normal taken
 * from projected coordinates is only good enough for culling, not for
 * lighting -- perspective divide distorts the angle. */
static fixed_t rot_x[MODEL_MAX_VERTS];
static fixed_t rot_y[MODEL_MAX_VERTS];
static fixed_t rot_z[MODEL_MAX_VERTS];

/* Integer square root, for normalising a face normal.
 *
 * Twelve of these per frame for a cube, so a bit-by-bit method is more
 * than fast enough and avoids pulling in any float. */
static uint32_t isqrt32(uint64_t v)
{
	uint64_t rem = 0, root = 0;
	for (int i = 0; i < 32; i++) {
		root <<= 1;
		rem = (rem << 2) | (v >> 62);
		v <<= 2;
		if (root < rem) {
			rem -= root | 1;
			root |= 2;
		}
	}
	return (uint32_t)(root >> 1);
}

/* Fixed light direction, Q12, pointing FROM the surface TOWARD the
 * light. Roughly over the viewer's left shoulder, and normalised so
 * the dot product below needs no further scaling.
 *
 * A fixed direction in VIEW space is the whole point: the model turns
 * under a stationary light, which is what makes a face's brightness
 * depend on its own orientation and nothing else. */
#define LIGHT_X  (-1683)   /* -0.411 */
#define LIGHT_Y  ( 2458)   /*  0.600 */
#define LIGHT_Z  (-2801)   /* -0.684, toward the camera */

/* Fill one horizontal span.
 *
 * Clipping is left to z_win_hw_fill_shade(), which clips to the
 * window's content rect. Doing it here as well would duplicate the
 * inclusive-bounds arithmetic that is easy to get wrong -- and getting
 * it wrong costs the last column of every span, which reads as a notch
 * down one edge of the solid rather than as a clipping bug. */
static int span_cx0, span_cy0, span_cx1, span_cy1;

/* -- span-based erase: why there is no clear --
 *
 * Clearing the window and redrawing leaves the object ABSENT for the
 * part of each frame between the two. At 15fps that is a large
 * fraction of the frame and the cube visibly flashes -- the same
 * problem ERASE_INTERLEAVED solves for wireframes by erasing and
 * redrawing one edge at a time, so the object is never fully gone.
 *
 * The equivalent for solid faces is to erase only what is no longer
 * covered. Each frame records, per scanline, the leftmost and
 * rightmost pixel the faces reached. Next frame the object is drawn
 * FIRST, and then only the slivers of the previous silhouette that the
 * new one does not cover are cleared.
 *
 * The object is therefore never blanked. Nothing flashes, and the
 * erase is proportional to how far the silhouette moved rather than to
 * the window area -- which is also why it is faster than the clear it
 * replaces.
 *
 * EXACT FOR A CONVEX SILHOUETTE, which a cube has: per scanline the
 * coverage is a single interval, so a min and a max describe it
 * completely. A non-convex model can have two intervals on one
 * scanline, and the gap between them would not be erased. That is a
 * real limitation and the reason to revisit this when STL faces
 * arrive; for the built-in solids it is exact.
 */
#define SPAN_ROWS 512

static int16_t span_prev_lo[SPAN_ROWS];
static int16_t span_prev_hi[SPAN_ROWS];
static int16_t span_cur_lo[SPAN_ROWS];
static int16_t span_cur_hi[SPAN_ROWS];
static bool    span_prev_valid = false;

static void span_reset_cur(void)
{
	for (int y = 0; y < SPAN_ROWS; y++) {
		span_cur_lo[y] = 32767;
		span_cur_hi[y] = -1;
	}
}

/* Clear the parts of last frame's silhouette this frame does not
 * cover. Called AFTER the faces are drawn, which is the whole point --
 * the object is on screen the entire time. */
static void span_erase_leftovers(void)
{
	if (!span_prev_valid) return;

	for (int y = span_cy0; y <= span_cy1 && y < SPAN_ROWS; y++) {

		int plo = span_prev_lo[y], phi = span_prev_hi[y];
		int clo = span_cur_lo[y],  chi = span_cur_hi[y];

		if (phi < plo) continue;             /* nothing was there */

		if (chi < clo) {
			/* covered last frame, not at all this frame */
			z_fb_hw_span_clear(plo, y, phi - plo + 1);
			continue;
		}

		if (plo < clo)
			z_fb_hw_span_clear(plo, y,
				(clo - 1 < phi ? clo - 1 : phi) - plo + 1);

		if (phi > chi) {
			int x0 = (chi + 1 > plo) ? chi + 1 : plo;
			z_fb_hw_span_clear(x0, y, phi - x0 + 1);
		}

	}
}

/* Clip bounds for the whole frame, fetched once.
 *
 * Clipping per span rather than per call into zwin lets the inner loop
 * use z_fb_hw_span(), which writes three registers instead of six --
 * see zgfx.h. At several hundred spans a frame that halving is worth
 * more than the tidiness of letting zwin do it. */
static void shade_clip_begin(void)
{
	z_clip_t c;
	z_win_content_rect(&win, &c);
	span_cx0 = (int)c.x0; span_cy0 = (int)c.y0;
	span_cx1 = (int)c.x1; span_cy1 = (int)c.y1;
}

static void shade_span(int x0, int x1, int y)
{
	if (x1 < x0) { int t = x0; x0 = x1; x1 = t; }

	if (y < span_cy0 || y > span_cy1) return;
	if (x0 < span_cx0) x0 = span_cx0;
	if (x1 > span_cx1) x1 = span_cx1;
	if (x1 < x0) return;

	if (y < SPAN_ROWS) {
		if (x0 < span_cur_lo[y]) span_cur_lo[y] = (int16_t)x0;
		if (x1 > span_cur_hi[y]) span_cur_hi[y] = (int16_t)x1;
	}

	z_fb_hw_span(x0, y, x1 - x0 + 1);
}

/* Flat-fill one triangle by walking its edges.
 *
 * Vertices are sorted by y, then the triangle is drawn as two parts
 * split at the middle vertex: top (one apex, widening) and bottom
 * (narrowing to one apex). Standard, and the only subtlety is that
 * the long edge spans BOTH halves, so its interpolation must run
 * continuously across the split rather than restarting.
 */
static void shade_triangle(int ax, int ay, int bx, int by,
	int cx, int cy, int level)
{
	int t;

	/* Hoists height and level out of the span loop. */
	z_fb_hw_span_begin(level);

	/* sort a,b,c by y */
	if (ay > by) { t=ax; ax=bx; bx=t; t=ay; ay=by; by=t; }
	if (ay > cy) { t=ax; ax=cx; cx=t; t=ay; ay=cy; cy=t; }
	if (by > cy) { t=bx; bx=cx; cx=t; t=by; by=cy; cy=t; }

	if (cy == ay) {
		/* degenerate: a single scanline. Still worth drawing -- an
		 * edge-on face is one line, and skipping it leaves a gap in
		 * the silhouette as the model rotates. */
		int lo = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
		int hi = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
		shade_span(lo, hi, ay);
		return;
	}

	/* Clip the SCAN RANGE, not just each span. A face partly above the
	 * window would otherwise run its whole interpolation for rows that
	 * are discarded -- which for a model scaled up past the window is
	 * most of them. */
	{
		int y0 = ay < span_cy0 ? span_cy0 : ay;
		int y1 = cy > span_cy1 ? span_cy1 : cy;
		if (y1 < y0) return;

	for (int y = y0; y <= y1; y++) {

		int xl, xr;

		/* long edge a->c, spanning the whole triangle */
		xl = ax + ((cx - ax) * (y - ay)) / (cy - ay);

		/* short edge: a->b above the split, b->c below */
		if (y < by)
			xr = (by == ay) ? bx : ax + ((bx - ax) * (y - ay)) / (by - ay);
		else
			xr = (cy == by) ? bx : bx + ((cx - bx) * (y - by)) / (cy - by);

		shade_span(xl, xr, y);

	}
	}
}

/* Render the model as solid shaded faces.
 *
 * Returns false if the model has no faces, so the caller can fall back
 * to wireframe rather than showing nothing. An STL import produces no
 * faces today (see stl.c), so that path is the common one.
 */
static bool render_shaded(void)
{
	if (model.ntris <= 0) return false;

	shade_clip_begin();
	span_reset_cur();

	tri_visible = 0;

	for (int i = 0; i < model.ntris && i < SHADE_MAX_TRIS; i++) {

		int a = model.tris[i].v0;
		int b = model.tris[i].v1;
		int c = model.tris[i].v2;

		/* Backface cull on the PROJECTED cross product. Screen space
		 * is correct for this even under perspective: projection
		 * preserves winding, so the sign is the same as it would be
		 * in view space, and only the sign matters. */
		int32_t ex1 = cur_x[b] - cur_x[a];
		int32_t ey1 = cur_y[b] - cur_y[a];
		int32_t ex2 = cur_x[c] - cur_x[a];
		int32_t ey2 = cur_y[c] - cur_y[a];
		int32_t cross = ex1 * ey2 - ey1 * ex2;

		/* KEEP when the cross product is POSITIVE.
		 *
		 * This sign was wrong in the first version, and reasoning
		 * about it did not settle it -- screen y runs down, which
		 * flips the handedness, and it is genuinely easy to talk
		 * yourself into either answer. It was resolved by computing
		 * each face's true 3D normal independently and checking which
		 * sign agreed: cross > 0 matches on all twelve cube faces,
		 * from a head-on view (2 visible) and a corner view (6).
		 *
		 * Getting it backwards does not look like an inverted test.
		 * You see the inside of the far side of the solid, which
		 * reads as a hole. */
		if (cross <= 0) continue;

		/* Light level from the face's TRUE 3D NORMAL against a fixed
		 * light direction.
		 *
		 * The first version used the projected area instead, scaled
		 * against a running maximum over the visible faces. The area
		 * really is a cosine term, so a single face brightened and
		 * dimmed correctly -- but the SCALE moved with the model,
		 * because the largest visible face changes as it turns. Every
		 * face's level then shifted together, which reads exactly as
		 * a light source orbiting the object rather than as shading.
		 *
		 * A normal against a fixed direction has no shared term, so a
		 * face's brightness depends only on its own orientation. That
		 * is what makes it look lit rather than animated.
		 *
		 * Computed in VIEW space from the rotated vertices, not from
		 * the projected ones: perspective divide distorts angles, so
		 * projected coordinates are good enough to decide FACING (a
		 * sign) but not ANGLE (a magnitude).
		 */
		{
			int32_t ux = rot_x[b] - rot_x[a];
			int32_t uy = rot_y[b] - rot_y[a];
			int32_t uz = rot_z[b] - rot_z[a];
			int32_t vx = rot_x[c] - rot_x[a];
			int32_t vy = rot_y[c] - rot_y[a];
			int32_t vz = rot_z[c] - rot_z[a];

			/* Q12 inputs give a Q24 cross product; shifted back to
			 * Q12 so the magnitude below stays in range. */
			int32_t nx = (int32_t)(((int64_t)uy * vz - (int64_t)uz * vy) >> 12);
			int32_t ny = (int32_t)(((int64_t)uz * vx - (int64_t)ux * vz) >> 12);
			int32_t nz = (int32_t)(((int64_t)ux * vy - (int64_t)uy * vx) >> 12);

			uint64_t mag2 = (uint64_t)((int64_t)nx * nx)
			              + (uint64_t)((int64_t)ny * ny)
			              + (uint64_t)((int64_t)nz * nz);
			uint32_t mag = isqrt32(mag2);

			int lvl;

			if (mag == 0) {
				/* Degenerate face -- zero area in 3D. Nothing sensible
				 * to light, and it covers no pixels either. */
				lvl = 1;
			} else {
				int32_t dot = (int32_t)((((int64_t)nx * LIGHT_X)
				                       + ((int64_t)ny * LIGHT_Y)
				                       + ((int64_t)nz * LIGHT_Z)) >> 12);

				/* cos in Q12, -4096..4096 */
				int32_t cosq = (int32_t)(((int64_t)dot << 12) / (int32_t)mag);

				if (cosq < 0) cosq = 0;      /* facing away from the light */
				if (cosq > 4096) cosq = 4096;

				/* Ambient floor so an unlit face is still a shape
				 * rather than a hole in the silhouette. */
				lvl = SHADE_AMBIENT +
					(int)((cosq * (Z_SHADE_MAX - SHADE_AMBIENT)) >> 12);
			}

			if (lvl < 1) lvl = 1;
			if (lvl > Z_SHADE_MAX) lvl = Z_SHADE_MAX;

			tri_shade[tri_visible] = (uint8_t)lvl;
		}

		tri_depth[tri_visible] = rot_z[a] + rot_z[b] + rot_z[c];
		tri_order[tri_visible] = (uint8_t)i;
		tri_visible++;

	}

	/* Painter's algorithm: back to front. Insertion sort, because
	 * tri_visible is at most a few dozen for the models that have
	 * faces at all -- a heap would be more code and slower at this
	 * size. */
	for (int i = 1; i < tri_visible; i++) {
		int32_t dk = tri_depth[i];
		uint8_t ok = tri_order[i];
		uint8_t sk = tri_shade[i];
		int j = i - 1;
		while (j >= 0 && tri_depth[j] < dk) {
			tri_depth[j+1] = tri_depth[j];
			tri_order[j+1] = tri_order[j];
			tri_shade[j+1] = tri_shade[j];
			j--;
		}
		tri_depth[j+1] = dk;
		tri_order[j+1] = ok;
		tri_shade[j+1] = sk;
	}

	for (int i = 0; i < tri_visible; i++) {
		const mtri_t *tr = &model.tris[tri_order[i]];
		shade_triangle(
			cur_x[tr->v0], cur_y[tr->v0],
			cur_x[tr->v1], cur_y[tr->v1],
			cur_x[tr->v2], cur_y[tr->v2],
			tri_shade[i]);
	}

	/* Erase LAST, so the object is never off screen. */
	span_erase_leftovers();

	for (int y = 0; y < SPAN_ROWS; y++) {
		span_prev_lo[y] = span_cur_lo[y];
		span_prev_hi[y] = span_cur_hi[y];
	}
	span_prev_valid = true;

	return true;
}

static void render_frame(void) {

	/* A model with faces but no edges is legal -- nedges is only
	 * needed by the wireframe path. */
	if (model.nverts <= 0) return;
	if (model.nedges <= 0 && model.ntris <= 0) return;

	PERF_FRAME_BEGIN();

	int rx = spin_angle(SPIN_DPS_X, man_x);
	int ry = spin_angle(SPIN_DPS_Y, man_y);
	int rz = spin_angle(SPIN_DPS_Z, man_z);

	fixed_t d = fixed_mul(proj_d, user_scale);

	int minx = 32767, miny = 32767, maxx = -32768, maxy = -32768;

	PERF_MARK(t_xform);

	for (int i = 0; i < model.nverts; i++) {

		vertex3d_t t = rotate_vertex(model.verts[i], rx, ry, rz);

		/* Kept for the painter's-algorithm depth sort. View space,
		 * not projected -- projection is monotonic in z so either
		 * would order correctly, but the rotated value is already in
		 * hand and needs no divide. */
		rot_x[i] = t.x;
		rot_y[i] = t.y;
		rot_z[i] = t.z;

		int px, py;
		project(&t, d, &px, &py);

		cur_x[i] = clamp16(px);
		cur_y[i] = clamp16(py);

		if (px < minx) minx = px;
		if (px > maxx) maxx = px;
		if (py < miny) miny = py;
		if (py > maxy) maxy = py;

	}

	PERF_ACC(c_xform, t_xform);



	bb_x0 = minx; bb_y0 = miny;
	bb_x1 = maxx; bb_y1 = maxy;
	bb_valid = true;

	PERF_MARK(t_erase);

	/* Shading always clears.
	 *
	 * ERASE_INTERLEAVED works by redrawing last frame's EDGES in
	 * colour 0, which has no meaning for filled faces -- there is no
	 * edge list to walk that would erase a solid. Painting the new
	 * frame's faces over the old ones almost works and fails exactly
	 * where it matters: as the model turns, a face that shrinks
	 * leaves a fringe of the previous frame behind it.
	 *
	 * The flicker that interleaved erase exists to avoid is much less
	 * visible with solid faces anyway, because the clear is
	 * immediately followed by large fills rather than by thin lines. */
	/* Shading does NOT clear here -- see span_erase_leftovers(). It
	 * erases only what the new silhouette no longer covers, after
	 * drawing, so the object is never blanked and nothing flashes. */
	if (erase_mode == ERASE_CLEAR && !(shade_mode && model.ntris > 0)) {
		z_win_fill_rect(&win, 0, 0, content_w, content_h, 0);
		prev_valid = false;
	}

	PERF_ACC(c_erase, t_erase);

	/* Text goes here, NOT at the end of the frame.
	 *
	 * Drawn last, it was on screen only from the end of one frame
	 * until the clear early in the next -- about 10ms of a 41ms
	 * frame, so it visibly flashed at ~24Hz even though it costs
	 * under a millisecond to draw. Drawn immediately after the
	 * erase, it is present for essentially the whole frame.
	 *
	 * The cost of putting it first is that edges drawn afterwards
	 * can cross over it. In the top-left corner of a centred
	 * projection that is rare, and a stable readout with the
	 * occasional line through it is much easier to read than a
	 * correct one that strobes. */
	PERF_MARK(t_text);
	fps_draw();
	PERF_ACC(c_text, t_text);

	PERF_MARK(t_draw);

	/* Solid first: render_shaded() reports false when the model has no
	 * faces, and the wireframe path below runs instead. That is the
	 * common case -- an STL import produces no faces (stl.c) -- so
	 * pressing S on an imported model correctly does nothing rather
	 * than blanking the view. */
	if (shade_mode && render_shaded()) {

		/* Faces were drawn; skip the wireframe entirely. The
		 * silhouette comes from the fills themselves. */
		prev_valid = false;

	} else if (erase_mode == ERASE_INTERLEAVED && prev_valid) {

		/* Erase and redraw each edge as a pair, so the object is
		 * never blank -- only ever one edge short, for as long as it
		 * takes to issue two line commands.
		 *
		 * Known artifact: where two edges cross, erasing this frame's
		 * old edge punches a one-pixel hole in a new edge already
		 * drawn this frame. A 1bpp framebuffer has no way to know a
		 * pixel is owed to two lines. The hole is repaired on the
		 * next frame, so crossings shimmer slightly. That is the
		 * price of not double-buffering, and it is a far smaller
		 * price than the whole object flashing. */
		for (int e = 0; e < model.nedges; e++) {

			uint16_t a = model.edges[e].v0;
			uint16_t b = model.edges[e].v1;

			draw_edge(prev_x[a], prev_y[a], prev_x[b], prev_y[b], 0);
			draw_edge(cur_x[a], cur_y[a], cur_x[b], cur_y[b], 1);

		}

	} else {

		for (int e = 0; e < model.nedges; e++)
			draw_edge(cur_x[model.edges[e].v0], cur_y[model.edges[e].v0],
				cur_x[model.edges[e].v1], cur_y[model.edges[e].v1], 1);

	}

	PERF_ACC(c_draw, t_draw);

	if (erase_mode == ERASE_INTERLEAVED) {
		for (int i = 0; i < model.nverts; i++) {
			prev_x[i] = cur_x[i];
			prev_y[i] = cur_y[i];
		}
		prev_valid = true;
	}

	PERF_FRAME_END(model.nedges);

}

/* Full repaint from scratch -- what a Z_WM_REDRAW needs.
 *
 * Must NOT render the model while a load is in progress: stl_load()
 * calls the pump callback mid-build, so model.nverts/nedges and the
 * arrays they index are in an inconsistent state at that moment. This
 * is exactly the hazard that makes it worth NOT unioning the loader's
 * hash tables with the projection arrays to save memory. */
static void paint_full(void) {

	z_win_clear(&win);

	prev_valid = false;

	if (loading) {
		z_win_draw_text(&win, 2, 2, "Loading...", 1, &z_font_5x8);
		return;
	}

	render_frame();

}

/* ============================================================
 * input
 * ============================================================ */

/* Live USB HID modifier byte. See this file's header comment for why
 * this is read from hardware rather than tracked from wm messages. */
static uint8_t kbd_mods(void) {

	uint32_t i0 = reg_usb0_info;
	uint32_t i1 = reg_usb1_info;

	if (((i0 >> 24) & 0x3) == 1) return (uint8_t)(i0 & 0xFF);
	if (((i1 >> 24) & 0x3) == 1) return (uint8_t)(i1 & 0xFF);

	return 0;

}

static bool dragging = false;
static bool was_left = false;
static bool have_last = false;
static int last_mx, last_my;

/* Rotation sensitivity, in 1/64 degrees per pixel of mouse travel.
 * 48 is 0.75 degrees per pixel -- a drag across a 200px window turns
 * the object about 150 degrees, which is roughly "the object follows
 * my hand". */
#define ROT_PER_PX   48

/* Scale sensitivity, in Q12 per pixel of vertical travel. */
#define SCALE_PER_PX (FIXED_ONE / 48)

static bool hit_object(int sx, int sy) {

	if (!bb_valid) return false;

	/* A wireframe is mostly holes, so hit testing the actual lines
	 * would mean clicking had to land on a one-pixel edge. The
	 * projected bounding box, padded a little, is what a user means
	 * by "the object". */
	return sx >= bb_x0 - 4 && sx <= bb_x1 + 4 &&
		sy >= bb_y0 - 4 && sy <= bb_y1 + 4;

}

/*
 * Pointer handling.
 *
 * The control scheme, and what each part is for:
 *
 *   click on the object     stop spinning, start rotating. Clicking
 *                           the object is the gesture that says "I
 *                           want to hold this still and look at it",
 *                           so it does both at once rather than
 *                           needing a separate stop.
 *
 *   drag                    rotate: horizontal travel turns about Y,
 *                           vertical about X.
 *
 *   Ctrl + drag             rotate as well, but WITHOUT needing to
 *                           hit the object. A heavily decimated model
 *                           can be mostly empty space, and a
 *                           scaled-down one is a small target; Ctrl
 *                           is the "I mean this window, not that
 *                           pixel" modifier.
 *
 *   Shift + move            scale, no button needed. Moving away from
 *                           you enlarges.
 *
 *   Alt                     suppresses all of the above. Alt+drag is
 *                           wm's own window-move gesture (see wm.c's
 *                           alt_move_focused()), so an app that also
 *                           acted on it would fight the window
 *                           manager for the same gesture.
 */
static void handle_mouse(uint32_t v) {

	int sx = (int)Z_WM_UNPACK_MOUSE_X(v);
	int sy = (int)Z_WM_UNPACK_MOUSE_Y(v);
	uint8_t btn = (uint8_t)Z_WM_UNPACK_MOUSE_BUTTONS(v);
	bool inside = Z_WM_UNPACK_MOUSE_INSIDE(v) ? true : false;

	bool left = (btn & Z_MOUSE_BTN_LEFT) != 0;
	uint8_t mods = kbd_mods();

	if (mods & Z_KBD_MOD_ALT) {
		dragging = false;
		have_last = false;
		was_left = left;
		return;
	}

	int dx = have_last ? (sx - last_mx) : 0;
	int dy = have_last ? (sy - last_my) : 0;

	if (left && !was_left && inside &&
		(hit_object(sx, sy) || (mods & Z_KBD_MOD_CTRL))) {
		spinning = false;
		dragging = true;
	}

	if (!left) dragging = false;

	if ((mods & Z_KBD_MOD_SHIFT) && !spinning && (inside || dragging) && have_last) {

		/* Screen y grows downwards; moving the mouse UP should make
		 * the object bigger, so the sign is inverted here. */
		user_scale -= dy * SCALE_PER_PX;

		if (user_scale < SCALE_MIN) user_scale = SCALE_MIN;
		if (user_scale > SCALE_MAX) user_scale = SCALE_MAX;

		prev_valid = false;

	} else if (dragging && have_last && (dx || dy)) {

		man_y += dx * ROT_PER_PX;
		man_x += dy * ROT_PER_PX;

		man_x %= ANG_FULL;
		man_y %= ANG_FULL;

	}

	last_mx = sx;
	last_my = sy;
	have_last = true;
	was_left = left;

}

/* ============================================================
 * file loading
 * ============================================================ */

static void forward_msg(z_msg_t *msg, void *user);

static z_dialog_ctx_t dlg_ctx;

static char last_dir[Z_FLIST_PATH_MAX] = "/";

static void remember_dir(const char *path) {

	int last = 0;

	for (int i = 0; path[i]; i++)
		if (path[i] == '/') last = i;

	if (last == 0) {
		last_dir[0] = '/';
		last_dir[1] = 0;
		return;
	}

	int i = 0;

	for (; i < last && i < (int)sizeof(last_dir) - 1; i++)
		last_dir[i] = path[i];

	last_dir[i] = 0;

}

static void update_title(void) {

	char t[MODEL_NAME_MAX + 2];
	int n = 0;

	/* '~' marks a model that was decimated to fit -- see model.h.
	 * Without it, a teapot that is visibly coarser than the file it
	 * came from looks like a parsing bug rather than a stated
	 * tradeoff. */
	if (model.decimated) t[n++] = '~';

	for (int i = 0; model.name[i] && n < (int)sizeof(t) - 1; i++)
		t[n++] = model.name[i];

	t[n] = 0;

	z_win_set_title(&win, t);

}

/* Called by stl_load() roughly once per 512-byte chunk. Servicing wm
 * here is not optional -- see stl.h's comment on what a silent app
 * does to the whole screen during a multi-second load. */
static void load_pump(void) {

	z_msg_t msg;

	while (z_msg_read(&msg) == Z_OK)
		forward_msg(&msg, NULL);

}

/* Where the load time went, to the serial console.
 *
 * The split that matters is I/O against everything else. If `io`
 * dominates, the fix is in the SD path or the chunk size; if the
 * remainder dominates, it is the parser; if `passes` is the outlier,
 * it is the clustering strategy needing to read the file again. Those
 * are three unrelated pieces of work, and this is what says which one
 * to do.
 *
 * Cycle counts come back 64-bit (a slow load outlives a 32-bit cycle
 * counter at 48MHz) and are converted here rather than in stl.c,
 * which has no business knowing the clock rate. */
static void report_load(const char *path) {

	const stl_stats_t *st = stl_last_stats();

	uint32_t total_ms = (uint32_t)(st->total_cycles / STL_CYCLES_PER_MS);
	uint32_t io_ms = (uint32_t)(st->io_cycles / STL_CYCLES_PER_MS);
	uint32_t cpu_ms = (total_ms > io_ms) ? (total_ms - io_ms) : 0;

	uint32_t io_pct = total_ms ? (io_ms * 100) / total_ms : 0;

	printf("gpu3d: loaded %s\n", path);
	printf("gpu3d:   %s, %u tris, grid %d -> %d verts %d edges\n",
		st->binary ? "binary" : "ascii", st->tris, st->grid,
		model.nverts, model.nedges);
	printf("gpu3d:   %u passes, %u KB read, %u.%us total"
		" (io %u.%us %u%%, cpu %u.%us)\n",
		st->passes, st->bytes / 1024,
		total_ms / 1000, (total_ms / 100) % 10,
		io_ms / 1000, (io_ms / 100) % 10, io_pct,
		cpu_ms / 1000, (cpu_ms / 100) % 10);

	uint32_t bbox_ms = (uint32_t)(st->bbox_cycles / STL_CYCLES_PER_MS);
	uint32_t build_ms = (uint32_t)(st->build_cycles / STL_CYCLES_PER_MS);

	printf("gpu3d:   bbox pass %u.%us, build passes %u.%us\n",
		bbox_ms / 1000, (bbox_ms / 100) % 10,
		build_ms / 1000, (build_ms / 100) % 10);

	if (io_ms > 0)
		printf("gpu3d:   read rate %u KB/s\n",
			(uint32_t)(((uint64_t)st->bytes * 1000) / (io_ms * 1024ULL + 1)));

}

static void do_load(const char *path) {

	loading = true;

	z_cursor_set_busy(true);
	paint_full();

	bool ok = stl_load(path, &model, load_pump);

	loading = false;
	z_cursor_set_busy(false);

	if (!ok) {
		printf("gpu3d: %s: %s\n", path, stl_last_error());
		load_cube();
	} else {
		report_load(path);
	}

	reset_view();
	update_title();

	prev_valid = false;
	bb_valid = false;

	z_win_clear(&win);

	/* The dialog is opened AFTER the window has been put back into a
	 * sane state, so the parent repaints correctly behind it. */
	if (!ok)
		z_dialog_confirm(&dlg_ctx, "Open failed", stl_last_error(),
			Z_DIALOG_OK_CANCEL);

}

static void do_open(void) {

	char path[80];

	if (!z_dialog_open(&dlg_ctx, last_dir, path, sizeof(path))) return;

	remember_dir(path);
	do_load(path);

}

/* ============================================================
 * messages
 * ============================================================ */

static void forward_msg(z_msg_t *msg, void *user) {

	(void)user;

	switch (msg->subject) {

		// The part of this window not covered by the windows in front
		// of it. Confines every subsequent draw to it -- see
		// z_win_apply_clip() in zwin.c. The ack it sends is not
		// optional: wm waits for it when a region narrows.
		case Z_WM_SET_CLIP:
			z_win_apply_clip(&win, &msg->obj);
			break;

		case Z_WM_REDRAW:

			if (msg->obj.type != Z_UINT32) break;

			/* Only ours: while a dialog is up this also sees the
			 * redraws aimed at the parent, which is the entire
			 * reason zdialog.c takes this callback. */
			if (z_win_redraw_id(msg->obj.val.uint32) != win.id) break;

			z_win_apply_redraw(&win, msg->obj.val.uint32);
			update_win_geometry();
			paint_full();
			z_win_redraw_done(&win);

			break;

		case Z_WM_WINDOW_MOVED:

			if (z_win_parse_rect(&win, &msg->obj)) {
				update_win_geometry();
				prev_valid = false;
			}

			break;

		case Z_WM_WINDOW_RESIZED:

			if (z_win_apply_resized(&win, &msg->obj)) {
				update_win_geometry();
				prev_valid = false;
			}

			break;

		default:
			break;

	}

}

static void handle_key(uint32_t keysym, uint8_t mods) {

	(void)mods;

	switch (keysym) {

		case ' ':
			/* advance_spin() refreshes spin_last_tick every frame,
			 * so there is no stale delta to discard here. */
			spinning = !spinning;
			break;

		case 'e':
		case 'E':
			erase_mode = (erase_mode == ERASE_INTERLEAVED) ?
				ERASE_CLEAR : ERASE_INTERLEAVED;
			prev_valid = false;
			z_win_clear(&win);
			break;

		case 'r':
		case 'R':
			reset_view();
			prev_valid = false;
			z_win_clear(&win);
			break;

		case 's':
		case 'S':
			/* Solid shading. Silently ineffective on a model with no
			 * faces, which is honest -- there is nothing to shade --
			 * but the status line reports which mode is in effect so
			 * it is not a mystery. */
			shade_mode = !shade_mode;
			if (erase_mode == ERASE_INTERLEAVED) prev_valid = false;
			/* The recorded silhouette describes pixels drawn by the
			 * other mode, so it cannot be used to erase them. Clear
			 * the window once on the switch and start fresh. */
			span_prev_valid = false;
			z_win_fill_rect(&win, 0, 0, content_w, content_h, 0);
			break;

		case 'o':
		case 'O':
			do_open();
			break;

		case '+':
		case '=':
			user_scale += FIXED_ONE / 8;
			if (user_scale > SCALE_MAX) user_scale = SCALE_MAX;
			prev_valid = false;
			break;

		case '-':
		case '_':
			user_scale -= FIXED_ONE / 8;
			if (user_scale < SCALE_MIN) user_scale = SCALE_MIN;
			prev_valid = false;
			break;

		default:
			break;

	}

}

/* ============================================================
 * main
 * ============================================================ */

int main(void) {

	printf("gpu3d: wireframe viewer\n");

	/* CLOSE_ICON WITHOUT CLOSE_KILLS_OWNER, deliberately: this app
	 * owns more than one window at a time whenever a dialog is open,
	 * and the killing form takes every window of a pid down the
	 * instant any one of them is clicked closed (see that flag's own
	 * warning in zwm.h). The old version could safely use it because
	 * it had no dialogs. */
	if (z_win_create_flags(&win, "cube", WIN_WIDTH, WIN_HEIGHT, -1, -1,
		Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_OPEN_ICON |
		Z_WIN_FLAG_RESIZABLE | Z_WIN_FLAG_MIN_IS_CREATE) != Z_OK) {
		printf("gpu3d: failed to create window -- is wm running?\n");
		return 1;
	}

	dlg_ctx.parent = &win;
	dlg_ctx.on_msg = forward_msg;
	dlg_ctx.user = NULL;

	update_win_geometry();
	load_cube();
	reset_view();

	fps_tick = z_uptime_ticks();
	fps_frames = 0;

	z_win_clear(&win);

	/* An STL to open, if the file browser launched us with one
	 * (Z_WM_SET_ARG, zwm.h). Must happen before the main loop and
	 * after the window exists -- z_launch_arg_take() blocks on wm's
	 * reply and discards anything else that arrives meanwhile, which
	 * is only safe this early. */
	{
		char arg[Z_WM_ARG_MAX];

		if (z_launch_arg_take(arg, sizeof(arg)) && arg[0])
			do_load(arg);
		else
			update_title();
	}

	for (;;) {

		z_msg_t msg;

		/* Drain the whole queue each pass rather than one message per
		 * iteration -- see Z_WM_MOUSE's own note in zwm.h on why
		 * handling one per loop makes an app fall progressively
		 * behind the real cursor. */
		while (z_msg_read(&msg) == Z_OK) {

			switch (msg.subject) {

				case Z_WM_KEY:

					if (msg.obj.type != Z_UINT32) break;
					if (!Z_WM_UNPACK_KEY_PRESSED(msg.obj.val.uint32)) break;

					handle_key(Z_WM_UNPACK_KEY_KEYSYM(msg.obj.val.uint32),
						(uint8_t)Z_WM_UNPACK_KEY_MODIFIERS(msg.obj.val.uint32));

					break;

				case Z_WM_MOUSE:

					if (msg.obj.type == Z_UINT32)
						handle_mouse(msg.obj.val.uint32);

					break;

				case Z_WM_TITLEBAR_ICON: {

					if (msg.obj.type != Z_UINT32) break;

					uint32_t v = msg.obj.val.uint32;

					if ((int)Z_WM_UNPACK_TBICON_ID(v) != win.id) break;

					if (Z_WM_UNPACK_TBICON_KIND(v) == Z_WM_TBICON_OPEN)
						do_open();

					break;

				}

				case Z_WM_CLOSE:

					if (msg.obj.type == Z_UINT32 &&
						(int32_t)msg.obj.val.uint32 == win.id) {
						z_win_destroy(&win);
						return 0;
					}

					break;

				default:

					forward_msg(&msg, NULL);
					break;

			}

		}

		advance_spin();
		render_frame();
		fps_tick_update();
		perf_report();

	
		/* Yield. This loop used to spin, so the app was RUNNABLE
		 * forever and took a full scheduler share from whatever was
		 * in the foreground -- see docs/app_runtime.md. renders continuously, so it caps at one tick rather than blocking. */
		z_proc_wait(1);
	}

	return 0;

}
