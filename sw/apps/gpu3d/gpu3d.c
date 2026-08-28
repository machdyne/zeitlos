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

static void draw_edge(int x0, int y0, int x1, int y1, int color) {

	if (!on_screen(x0, y0) || !on_screen(x1, y1)) return;

	z_win_hw_line(&win, x0, y0, x1, y1, color);

}

static void render_frame(void) {

	if (model.nverts <= 0 || model.nedges <= 0) return;

	PERF_FRAME_BEGIN();

	int rx = spin_angle(SPIN_DPS_X, man_x);
	int ry = spin_angle(SPIN_DPS_Y, man_y);
	int rz = spin_angle(SPIN_DPS_Z, man_z);

	fixed_t d = fixed_mul(proj_d, user_scale);

	int minx = 32767, miny = 32767, maxx = -32768, maxy = -32768;

	PERF_MARK(t_xform);

	for (int i = 0; i < model.nverts; i++) {

		vertex3d_t t = rotate_vertex(model.verts[i], rx, ry, rz);

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

	if (erase_mode == ERASE_CLEAR) {
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

	if (erase_mode == ERASE_INTERLEAVED && prev_valid) {

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

	}

	return 0;

}
