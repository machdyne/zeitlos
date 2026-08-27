/*
 * space3d -- first-person wireframe space game
 *
 * You are flying forward through an asteroid field. Arrow keys steer
 * (the ship strafes; there's no roll), space fires, and anything that
 * reaches the cockpit ends the run. Occasional UFOs turn up among the
 * rocks and weave from side to side.
 *
 * Everything is drawn with the GPU line rasterizer (rtl/gpu/gpu_raster.v)
 * through z_win_hw_line(), so the whole game is a wireframe.
 *
 *   > run wm
 *   > run space3d
 *
 * ---------------------------------------------------------------------
 * Design notes -- the parts that aren't obvious from the code:
 *
 * COORDINATE SYSTEM. The camera never moves. It sits at the origin
 * looking down +Z, with +Y up and +X right, and every entity stores
 * its position *relative to the camera*. Flying forward is therefore
 * `z -= speed` applied to the whole world, and strafing is `x -= vx`
 * applied to the whole world. This is not a shortcut around a "real"
 * camera transform so much as the cheapest correct one: with a camera
 * that only ever translates, view space and world space differ by a
 * translation, and folding that translation into the entity update
 * removes a per-vertex subtract from the hot loop entirely. It also
 * means spawning is naturally camera-relative -- a rock spawned at
 * x=+400 is 400 units to the right of wherever the player currently
 * is, with no bookkeeping.
 *
 * NO FLOATING POINT, AND NO 64-BIT EITHER. sw/apps/gpu3d uses a
 * 12-bit fixed-point type whose multiply goes through int64
 * (__muldi3), which is fine for one spinning cube and less fine for a
 * few hundred vertices a frame. Here, world coordinates are plain
 * int32 in "world units" and the projection is a single shift and
 * divide -- see project_x()/project_y(). Only the trig table is
 * fractional (1.0 == 4096), and it's only ever applied to model-space
 * vertices, which are int8. Every intermediate below is bounded well
 * inside int32; the specific bounds are argued at each site.
 *
 * WHY THE APP CLIPS ITS OWN LINES. z_fb_hw_line() (zgfx.c) *clamps*
 * out-of-range coordinates to the screen rather than clipping them.
 * That's the right call there -- it exists to stop a bad coordinate
 * hanging the rasterizer's state machine, since gpu_x0/gpu_y0 are
 * 10-bit registers and the framebuffer is only 640x480. But clamping
 * moves an endpoint without preserving the line's slope, so a rock
 * passing just off the left edge would have its edges snap onto the
 * window border at visibly wrong angles instead of sliding cleanly
 * out of view. A 3D scene has lines going off-window constantly, so
 * this file does proper Cohen-Sutherland clipping (emit_line()) plus
 * a near-plane clip in view space (emit_edge()) before submitting
 * anything. Everything handed to z_win_hw_line() is already inside
 * the content rect, so the library's clamp and the hardware's own
 * clip registers both end up as redundant backstops -- which is what
 * you want them to be.
 *
 * DOUBLE-BUFFERED DISPLAY LIST, NOT A DOUBLE-BUFFERED SCREEN. There
 * is one framebuffer and no page flipping, so the usual trick applies:
 * keep the list of lines drawn last frame, redraw them in colour 0 to
 * erase, then draw this frame's list in colour 1. Clearing the whole
 * content area with the blitter each frame would be simpler, but it
 * blanks every pixel in the window for the duration of a full redraw
 * rather than just the thin lines that actually changed, which reads
 * as a much heavier flicker. Overlapping lines do lose a shared pixel
 * when one of them is erased; on a wireframe this is invisible in
 * motion and it is what sw/apps/gpu3d does too.
 * ---------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "../../common/zeitlos.h"
#include "../../common/zwm.h"
#include "../../common/zwin.h"
#include "../../common/zfont.h"
#include "../../common/zgfx.h"
#include "../../common/zkbd.h"

// ===========================================================================
// CONFIGURATION
// ===========================================================================

#define WIN_W            320
#define WIN_H            240

// Perspective projection: screen_x = cx + ((x << PROJ_SHIFT) / z).
//
// PROJ_SHIFT is the focal length expressed as a power of two so the
// numerator is a shift rather than a multiply. 256 against a ~157px
// content half-width puts the left/right edges of the window at
// x = +/-0.61z, i.e. a horizontal field of view of about 63 degrees --
// wide enough to feel like a cockpit canopy without the fisheye look
// a much wider one gives on a window this size. Pixels are square at
// 640x480, so Y uses the same value with no aspect correction.
#define PROJ_SHIFT       8
#define PROJ_SCALE       (1 << PROJ_SHIFT)

// Near plane. Edges crossing it are clipped to it (emit_edge());
// anything entirely behind it is dropped. Also the divisor floor for
// the projection, which is why it can't be small: the projected extent
// of a vertex scales as 1/z, and NEAR_Z together with WORLD_LIMIT is
// what bounds every intermediate in the clipper below to int32.
#define NEAR_Z           64

// Entities beyond this distance from the flight axis are culled. Along
// with NEAR_Z this caps a projected coordinate at roughly
// (WORLD_LIMIT + object radius) << PROJ_SHIFT / NEAR_Z, about 10k
// pixels from the window centre -- see emit_line()'s overflow guard.
#define WORLD_LIMIT      2200

#define SPAWN_Z          3000    // where rocks and UFOs appear
#define DESPAWN_Z        (-300)  // where they're recycled after passing
#define STAR_FAR         5200

// Ship handling. Acceleration and damping rather than direct position
// control: the drift when you let go of a key is most of what makes
// this feel like flying rather than dragging a sprite around.
#define SHIP_ACCEL       6
#define SHIP_VMAX        34
#define SHIP_DAMP_SHIFT  3       // v -= v >> 3 each frame when coasting

// A single constant cruise speed, deliberately not a difficulty ramp.
// An earlier version scaled this with score, which is the reflex for
// an arcade game and the wrong instinct for this one: accelerating
// turns a field you drift through into a field you fight, and the
// point here is the drifting. Score still counts, it just doesn't
// tighten the screws. SPEED_MAX is kept as a separate name because
// the missile hit test needs a worst-case closing speed for its
// tunnelling margin, and that argument should stay readable even
// though the two values are equal today.
#define SPEED_CRUISE     36      // forward units per frame
#define SPEED_MAX        36

// Missiles travel at MISSILE_SPEED through the world while the world
// itself is coming at us at `speed`, so their closing rate on a target
// is MISSILE_SPEED + speed -- which is what the hit test's Z tolerance
// has to cover to avoid tunnelling. See check_missile_hits().
#define MISSILE_SPEED    110
#define MISSILE_MAX_Z    3400
#define MISSILE_TAIL     70      // tracer length, along the flight axis
#define MISSILE_BOLT     24      // half-width of the bolt's cross
// Muzzle distance. Not zero, and not small: the bolt is a fixed size
// in world units, so its projected size goes as 1/z and a shot
// spawned right at the cockpit fills a third of the window on its
// first frame before shrinking -- which reads as a rendering glitch
// rather than as gunfire. 320 puts the first frame at a believable
// ~38px across, and also gives the muzzle offset below enough
// projected separation to be visible as two distinct guns.
#define MISSILE_SPAWN_Z  320
#define FIRE_COOLDOWN    5       // frames between shots

#define CRASH_Z          90      // cockpit plane for collision purposes
#define SHIP_RADIUS      70

#define MAX_OBJECTS      6
#define MAX_MISSILES     4
#define MAX_EXPLOSIONS   4

// Rocks are scenery you occasionally have to deal with, not a stream
// of targets: at roughly 90 frames of transit time, an interval in
// this range leaves about one on screen at a time and stretches of
// empty sky between them. See update()'s spawn block.
#define SPAWN_MIN        70
#define SPAWN_MAX        150

#define NUM_STARS        64

#define EXPL_FRAMES      9
#define EXPL_STEP        30

// Close stars are drawn as a short radial streak instead of a single
// pixel -- two projections instead of one, and the single cheapest
// thing in the whole file for conveying speed.
#define STREAK_Z         700
#define STREAK_LEN       260

// ~732Hz kernel tick (see z_uptime_ticks(), sw/os/kernel.c), so 24
// ticks is a hair over 30fps. The frame is fixed-timestep: if a frame
// overruns we fall behind in wall-clock time rather than taking a
// bigger simulation step, which keeps collision detection honest.
#define FRAME_TICKS      24

// -- background galaxies --
//
// Drawn as loose scatters of single pixels along spiral arms rather
// than as polylines. A spiral stroked with line segments reads as a
// drawn curve -- a piece of vector art sitting in front of the sky --
// whereas dots read as what they're meant to be, which is a very
// large number of very distant stars. It's also cheaper: a dot is a
// zero-length line and the rasterizer retires it in a handful of
// cycles.
#define MAX_GALAXIES     2
#define GALAXY_ARMS      3
#define GALAXY_DOTS      14      // per arm
#define GALAXY_TWIST     7       // byte-angle advance per radial step
#define GALAXY_Z_MIN     20000
#define GALAXY_Z_MAX     40000

// Galaxies take the ship's full lateral motion, and none of its
// forward motion.
//
// There used to be a divisor here, damping their lateral response to
// a sixth. It was a mistake, and an instructive one: perspective
// already damps distant things by 1/z, so the divisor was faking
// distance on top of a real distance and double-counting it. The
// measured result was 3.6 px/sec of drift against a near star's 130 --
// a ratio of 36 to 1, which stops reading as "very far away" and
// starts reading as "pinned to the windscreen", because nothing in
// the real world is that much more sluggish than its neighbours.
//
// The fix is to make the distance real rather than simulated: the Z
// range above went from 9-17k out to 20-40k, and the divisor is gone.
// Perspective alone now yields about 9 px/sec against the same near
// star, a ratio near 15 to 1 -- plainly background, but it moves when
// you steer, which is the whole difference between scenery and a
// sticker on the glass. Apparent size is unaffected because galaxies
// are sized by how big they should look, not how big they are (see
// spawn_galaxy()).
//
// Forward motion is still deliberately ignored. A galaxy that
// approached at cruise speed would arrive, and there is nothing
// sensible to do when it does.

// Full rotation is 256 byte-angle units. The spin accumulator is
// 16-bit and the angle is its top byte, so a rate of ~110 works out
// near 0.43 units per frame: a little over twenty seconds per
// revolution at 30fps, which is slow enough to read as majestic
// rather than as a spinning propeller.
// How long a galaxy stays away between appearances, in frames, and
// how long it takes to cross the view once it turns up. Sized against
// each other: with two slots, a crossing of ~2400 frames against a
// dormancy of 3000-9000 leaves a galaxy on screen well under half the
// time, so empty sky is the normal state and a spiral drifting
// through is an event.
#define GALAXY_DORMANT_MIN  3000
#define GALAXY_DORMANT_MAX  9000
#define GALAXY_CROSS_MIN    1800
#define GALAXY_CROSS_MAX    3200

#define GALAXY_SPIN_MIN  70
#define GALAXY_SPIN_MAX  165

// Crash strobe, in frames, alternating filled and cleared.
#define FLASH_FRAMES     8

#define MAX_LINES        512

// ===========================================================================
// TRIG
// ===========================================================================

// Angles are a single byte: 256 units to the full turn, so wrapping is
// a mask and never a modulo. Values are sin * 4096; cos is the same
// table read 64 units (90 degrees) along.
static const int16_t sin_tab[256] = {
	    0,   101,   201,   301,   401,   501,   601,   700,
	  799,   897,   995,  1092,  1189,  1285,  1380,  1474,
	 1567,  1660,  1751,  1842,  1931,  2019,  2106,  2191,
	 2276,  2359,  2440,  2520,  2598,  2675,  2751,  2824,
	 2896,  2967,  3035,  3102,  3167,  3229,  3290,  3349,
	 3406,  3461,  3513,  3564,  3612,  3659,  3703,  3745,
	 3784,  3822,  3857,  3889,  3920,  3948,  3973,  3996,
	 4017,  4036,  4052,  4065,  4076,  4085,  4091,  4095,
	 4096,  4095,  4091,  4085,  4076,  4065,  4052,  4036,
	 4017,  3996,  3973,  3948,  3920,  3889,  3857,  3822,
	 3784,  3745,  3703,  3659,  3612,  3564,  3513,  3461,
	 3406,  3349,  3290,  3229,  3167,  3102,  3035,  2967,
	 2896,  2824,  2751,  2675,  2598,  2520,  2440,  2359,
	 2276,  2191,  2106,  2019,  1931,  1842,  1751,  1660,
	 1567,  1474,  1380,  1285,  1189,  1092,   995,   897,
	  799,   700,   601,   501,   401,   301,   201,   101,
	    0,  -101,  -201,  -301,  -401,  -501,  -601,  -700,
	 -799,  -897,  -995, -1092, -1189, -1285, -1380, -1474,
	-1567, -1660, -1751, -1842, -1931, -2019, -2106, -2191,
	-2276, -2359, -2440, -2520, -2598, -2675, -2751, -2824,
	-2896, -2967, -3035, -3102, -3167, -3229, -3290, -3349,
	-3406, -3461, -3513, -3564, -3612, -3659, -3703, -3745,
	-3784, -3822, -3857, -3889, -3920, -3948, -3973, -3996,
	-4017, -4036, -4052, -4065, -4076, -4085, -4091, -4095,
	-4096, -4095, -4091, -4085, -4076, -4065, -4052, -4036,
	-4017, -3996, -3973, -3948, -3920, -3889, -3857, -3822,
	-3784, -3745, -3703, -3659, -3612, -3564, -3513, -3461,
	-3406, -3349, -3290, -3229, -3167, -3102, -3035, -2967,
	-2896, -2824, -2751, -2675, -2598, -2520, -2440, -2359,
	-2276, -2191, -2106, -2019, -1931, -1842, -1751, -1660,
	-1567, -1474, -1380, -1285, -1189, -1092,  -995,  -897,
	 -799,  -700,  -601,  -501,  -401,  -301,  -201,  -101
};

#define SIN(a)  ((int32_t)sin_tab[(a) & 255])
#define COS(a)  ((int32_t)sin_tab[((a) + 64) & 255])

// ===========================================================================
// RANDOM
// ===========================================================================

// xorshift32. rand() from newlib would do, but it drags in more than
// this needs and its state lives somewhere this file can't see -- a
// self-contained generator is three lines and makes a run reproducible
// by fixing the seed, which is genuinely useful when a crash-on-spawn
// bug only shows up every few hundred rocks.
static uint32_t rng_state = 0x2545f491u;

static uint32_t rnd(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

// inclusive on both ends
static int32_t rnd_range(int32_t lo, int32_t hi) {
	return lo + (int32_t)(rnd() % (uint32_t)(hi - lo + 1));
}

// ===========================================================================
// MODELS
// ===========================================================================

// Model vertices are int8 in a nominal +/-127 cube and get scaled by
// the instance's own radius at transform time (>> 7, so 128 == one
// radius). int8 keeps the tables small and, more usefully, bounds the
// rotation arithmetic: 127 * 4096 fits with room to spare, so the
// rotate-and-scale path never needs a wider type.

typedef struct {
	const int8_t  *verts;    // 3 per vertex
	const uint8_t *edges;    // 2 vertex indices per edge
	uint8_t nverts;
	uint8_t nedges;
} model_t;

// Three rocks. All are octahedra with the vertices pushed around a
// bit -- an unperturbed one reads as a machined crystal rather than a
// rock, and three variants is enough that a field of eight doesn't
// look cloned. Same edge list for all three.
static const int8_t rock0_verts[] = {
	   4, 118,  -6,   -6,-112,   8,  115,  10,  12,
	-108,  -8, -14,   10,  -6, 120,  -12,  14,-110
};
static const int8_t rock1_verts[] = {
	  -8, 105,  14,    6,-120, -10,  122, -12,  -8,
	 -96,  16,  10,   14,  10, 104,   -6, -14,-122
};
static const int8_t rock2_verts[] = {
	  12, 122,   8,  -10, -98, -12,   98,  14, -16,
	-120,  -6,   6,   -8,  12, 112,   16, -10, -96
};

// 0/1 are the poles, 2..5 the equator ring.
static const uint8_t rock_edges[] = {
	0,2, 0,3, 0,4, 0,5,
	1,2, 1,3, 1,4, 1,5,
	2,4, 4,3, 3,5, 5,2
};

// Flying saucer: a hexagonal rim with a smaller hexagonal dome above
// it. Twelve vertices and eighteen edges reads unmistakably as a UFO
// from a long way off, which is the whole requirement.
static const int8_t ufo_verts[] = {
	 120,   0,   0,   60,   0, 104,  -60,   0, 104,
	-120,   0,   0,  -60,   0,-104,   60,   0,-104,
	  56,  52,   0,   28,  52,  48,  -28,  52,  48,
	 -56,  52,   0,  -28,  52, -48,   28,  52, -48
};
static const uint8_t ufo_edges[] = {
	0,1, 1,2, 2,3, 3,4, 4,5, 5,0,          // rim
	6,7, 7,8, 8,9, 9,10, 10,11, 11,6,      // dome
	0,6, 1,7, 2,8, 3,9, 4,10, 5,11         // struts
};

static const model_t models[] = {
	{ rock0_verts, rock_edges, 6, 12 },
	{ rock1_verts, rock_edges, 6, 12 },
	{ rock2_verts, rock_edges, 6, 12 },
	{ ufo_verts,   ufo_edges, 12, 18 }
};

#define MODEL_UFO  3

// Directions an explosion throws its shards along. Deliberately not
// axis-aligned and not symmetric -- a burst on the axes looks like a
// snowflake, which is not the effect wanted.
static const int8_t expl_dirs[8][3] = {
	{ 100,  20,  10 }, { -90,  30, -20 },
	{  20, 100, -30 }, { -10,-100,  25 },
	{  60, -40,  80 }, { -70,  50, -80 },
	{ 110, -60, -30 }, {-100, -50,  60 }
};

// ===========================================================================
// ENTITIES
// ===========================================================================

typedef struct {
	bool     active;
	bool     is_ufo;
	uint8_t  model;
	int32_t  x, y, z;
	int32_t  vx, vy;      // lateral drift, world units per frame
	int32_t  vz;          // closing speed on top of the ship's own
	int16_t  radius;
	uint8_t  rx, ry;      // tumble angles
	int8_t   drx, dry;    // tumble rates
	uint8_t  weave;       // UFO weave phase
} object_t;

typedef struct {
	bool    active;
	int32_t x, y, z;
} missile_t;

typedef struct {
	bool    active;
	int32_t x, y, z;
	int16_t size;
	uint8_t t;
} explosion_t;

static object_t    objects[MAX_OBJECTS];
static missile_t   missiles[MAX_MISSILES];
static explosion_t explosions[MAX_EXPLOSIONS];

typedef struct {
	bool     active;
	int32_t  x, y, z;
	int32_t  vx, vy;      // slow proper motion of its own
	int16_t  radius;
	uint16_t spin;        // accumulator; the angle is its top byte
	uint16_t spin_rate;
	uint8_t  tilt;        // inclination of the disc, byte angle
	int32_t  dormant;     // frames until it next appears; 0 = on screen
} galaxy_t;

static galaxy_t galaxies[MAX_GALAXIES];

static int32_t star_x[NUM_STARS];
static int32_t star_y[NUM_STARS];
static int32_t star_z[NUM_STARS];

// ===========================================================================
// GAME STATE
// ===========================================================================

static z_win_t win;

// Content rect in absolute screen coordinates, and the projection
// centre derived from it. Both are refreshed from
// z_win_content_rect() whenever the window moves -- see
// update_geometry(). Nothing here keeps its own copy of the content
// rect formula; zwin.c owns it, for the reasons set out in that
// function's own comment.
static int32_t clip_x0, clip_y0, clip_x1, clip_y1;
static int32_t view_cx, view_cy;

static bool     key_left, key_right, key_up, key_down;
static int32_t  ship_vx, ship_vy;
static int32_t  speed;
static uint32_t score;
static bool     game_over;
static int      fire_cooldown;
static int      spawn_timer;
static int      muzzle_side;
static int      flash_timer;   // >0 while the crash strobe is running

// Display list. Two buffers: one holding what's currently on screen
// (to be erased) and one being built for this frame.
typedef struct {
	int16_t x0, y0, x1, y1;
} line_t;

static line_t line_buf[2][MAX_LINES];
static int    line_count[2];
static int    cur_buf;

static bool overflowed;   // display list filled up; reported once

// ===========================================================================
// GEOMETRY: CLIP AND EMIT
// ===========================================================================

#define OUT_LEFT    1
#define OUT_RIGHT   2
#define OUT_TOP     4
#define OUT_BOTTOM  8

static int outcode(int32_t x, int32_t y) {
	int c = 0;
	if (x < clip_x0) c |= OUT_LEFT;
	else if (x > clip_x1) c |= OUT_RIGHT;
	if (y < clip_y0) c |= OUT_TOP;
	else if (y > clip_y1) c |= OUT_BOTTOM;
	return c;
}

static void push_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
	int n = line_count[cur_buf];
	if (n >= MAX_LINES) { overflowed = true; return; }
	line_buf[cur_buf][n].x0 = (int16_t)x0;
	line_buf[cur_buf][n].y0 = (int16_t)y0;
	line_buf[cur_buf][n].x1 = (int16_t)x1;
	line_buf[cur_buf][n].y1 = (int16_t)y1;
	line_count[cur_buf] = n + 1;
}

// Bound on any coordinate reaching the clipper, in pixels from the
// window centre. Derived, not guessed: a vertex is culled past
// WORLD_LIMIT (2200) plus at most a body radius (~240), and the near
// plane floors the divisor at NEAR_Z (64), so the projection can't
// exceed 2440 * 256 / 64, about 9800. 16000 leaves generous headroom
// while keeping the worst-case product inside the clipper --
// (x1-x0) * (edge-y0), about 32000 * 16240 -- at 5.2e8, comfortably
// within int32. Anything past this is a bug elsewhere rather than a
// legitimately huge line, so dropping it is both safe and the right
// diagnosis-preserving behaviour: one missing line, not silent
// arithmetic wraparound producing a plausible-looking wrong one.
#define COORD_BOUND  16000

static void emit_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {

	int oc0, oc1, iter;

	if (x0 < view_cx - COORD_BOUND || x0 > view_cx + COORD_BOUND ||
	    x1 < view_cx - COORD_BOUND || x1 > view_cx + COORD_BOUND ||
	    y0 < view_cy - COORD_BOUND || y0 > view_cy + COORD_BOUND ||
	    y1 < view_cy - COORD_BOUND || y1 > view_cy + COORD_BOUND)
		return;

	oc0 = outcode(x0, y0);
	oc1 = outcode(x1, y1);

	// Cohen-Sutherland. Each iteration either accepts, rejects, or
	// moves one endpoint onto a boundary, and an endpoint can be
	// moved at most twice before it's inside, so four iterations is
	// sufficient; six is the same argument with slack.
	for (iter = 0; iter < 6; iter++) {

		if (!(oc0 | oc1)) {           // both inside
			push_line(x0, y0, x1, y1);
			return;
		}

		if (oc0 & oc1) return;        // both outside the same edge

		{
			int oc = oc0 ? oc0 : oc1;
			int32_t x, y;

			// The divisors below can't be zero: a horizontal line
			// (y1 == y0) that is off the top or bottom has that bit
			// set in *both* outcodes and was rejected above, and
			// likewise for a vertical line against left/right.
			if (oc & OUT_BOTTOM) {
				x = x0 + (x1 - x0) * (clip_y1 - y0) / (y1 - y0);
				y = clip_y1;
			} else if (oc & OUT_TOP) {
				x = x0 + (x1 - x0) * (clip_y0 - y0) / (y1 - y0);
				y = clip_y0;
			} else if (oc & OUT_RIGHT) {
				y = y0 + (y1 - y0) * (clip_x1 - x0) / (x1 - x0);
				x = clip_x1;
			} else {
				y = y0 + (y1 - y0) * (clip_x0 - x0) / (x1 - x0);
				x = clip_x0;
			}

			if (oc == oc0) {
				x0 = x; y0 = y; oc0 = outcode(x0, y0);
			} else {
				x1 = x; y1 = y; oc1 = outcode(x1, y1);
			}
		}
	}
}

// Project a view-space point. Callers must have ensured z >= NEAR_Z.
//
// Multiplication by PROJ_SCALE rather than a left shift by
// PROJ_SHIFT, even though the two are the same instruction on RISC-V:
// x is routinely negative here (anything left of, or below, the
// centre of the screen) and left-shifting a negative signed value is
// undefined behaviour in C, not merely implementation-defined. GCC
// happens to emit slli and get the intended answer, but this is
// exactly the kind of construct that a future -O level, a different
// toolchain version, or UBSan turns into a surprise -- and the
// multiply costs nothing, since the compiler folds a constant power
// of two back into the shift anyway.
static inline int32_t project_x(int32_t x, int32_t z) {
	return view_cx + ((x * PROJ_SCALE) / z);
}

static inline int32_t project_y(int32_t y, int32_t z) {
	// screen Y grows downward, world Y grows up
	return view_cy - ((y * PROJ_SCALE) / z);
}

// A 3D segment: near-plane clip, project, then hand to the 2D clipper.
static void emit_edge(int32_t ax, int32_t ay, int32_t az,
                      int32_t bx, int32_t by, int32_t bz) {

	if (az < NEAR_Z && bz < NEAR_Z) return;

	// Interpolating onto the near plane must happen here, in view
	// space, and not be left to the 2D clipper: a vertex behind the
	// camera projects to a *mirrored* position on the far side of the
	// screen, so a segment straddling the camera would be drawn as a
	// line to entirely the wrong place rather than one running off
	// the edge. The products are small -- the two endpoints belong to
	// one object, so (bx - ax) is at most a body diameter, and the
	// numerator is bounded by NEAR_Z - DESPAWN_Z.
	if (az < NEAR_Z) {
		int32_t num = NEAR_Z - az;
		int32_t den = bz - az;
		ax += (bx - ax) * num / den;
		ay += (by - ay) * num / den;
		az = NEAR_Z;
	} else if (bz < NEAR_Z) {
		int32_t num = NEAR_Z - bz;
		int32_t den = az - bz;
		bx += (ax - bx) * num / den;
		by += (ay - by) * num / den;
		bz = NEAR_Z;
	}

	emit_line(project_x(ax, az), project_y(ay, az),
	          project_x(bx, bz), project_y(by, bz));
}

// ===========================================================================
// MODEL RENDERING
// ===========================================================================

// Transform a model into view space and emit its edges.
//
// Rotation is Y then X only. A third axis would cost another four
// multiplies per vertex and is not distinguishable on a tumbling rock
// seen for two seconds, so it isn't there.
static void draw_model(const model_t *m, int32_t ox, int32_t oy, int32_t oz,
                       int32_t radius, uint8_t rx, uint8_t ry) {

	int32_t px[12], py[12], pz[12];
	int32_t cy = COS(ry), sy = SIN(ry);
	int32_t cx = COS(rx), sx = SIN(rx);
	int i;

	for (i = 0; i < m->nverts; i++) {

		int32_t vx = m->verts[i * 3 + 0];
		int32_t vy = m->verts[i * 3 + 1];
		int32_t vz = m->verts[i * 3 + 2];
		int32_t tx, ty, tz, tz2;

		// yaw: at most 127 * 4096 * 2, nowhere near int32's limit
		tx  = (vx * cy + vz * sy) >> 12;
		tz  = (vz * cy - vx * sy) >> 12;

		// pitch
		ty  = (vy * cx - tz * sx) >> 12;
		tz2 = (vy * sx + tz * cx) >> 12;

		// scale to the instance's radius (128 == one radius) and
		// translate into view space
		px[i] = ox + ((tx * radius) >> 7);
		py[i] = oy + ((ty * radius) >> 7);
		pz[i] = oz + ((tz2 * radius) >> 7);
	}

	for (i = 0; i < m->nedges; i++) {
		int a = m->edges[i * 2 + 0];
		int b = m->edges[i * 2 + 1];
		emit_edge(px[a], py[a], pz[a], px[b], py[b], pz[b]);
	}
}

// ===========================================================================
// SCENE
// ===========================================================================

// One galaxy: spiral arms of dots, plus a denser nucleus.
//
// The disc is generated flat (local Z is zero for every dot) and then
// inclined about the X axis, which is why the tilt below is two
// multiplies rather than a full rotation -- with local Z zero, four
// of the six terms vanish. Face-on would be cheaper still and looks
// like a clock face; the inclination is most of what makes it read as
// a disc in space.
static void draw_galaxy(const galaxy_t *g) {

	int32_t ct = COS(g->tilt), st = SIN(g->tilt);
	uint8_t spin = (uint8_t)(g->spin >> 8);
	int arm, k;

	for (arm = 0; arm < GALAXY_ARMS; arm++) {

		uint8_t base = (uint8_t)(spin + arm * (256 / GALAXY_ARMS));

		for (k = 1; k <= GALAXY_DOTS; k++) {

			// Radius grows faster than linearly so dots crowd toward
			// the middle and thin out at the rim -- both what a real
			// spiral looks like, and what stops the arms reading as
			// evenly-spaced dotted lines. Purely quadratic was the
			// first try and overdid it: so many dots landed inside
			// the core's own few pixels that they were simply spent,
			// while the outer arm broke up into scattered specks.
			// This is the average of linear and quadratic, which
			// keeps the gradient without either failure.
			int32_t r = (int32_t)g->radius * (k * k + k * GALAXY_DOTS) /
			            (2 * GALAXY_DOTS * GALAXY_DOTS);
			uint8_t a = (uint8_t)(base + k * GALAXY_TWIST);
			int32_t lx = (r * COS(a)) >> 12;
			int32_t ly = (r * SIN(a)) >> 12;
			int32_t wx = g->x + lx;
			int32_t wy = g->y + ((ly * ct) >> 12);
			int32_t wz = g->z + ((ly * st) >> 12);
			int32_t sx, sy;

			if (wz < NEAR_Z) continue;

			sx = project_x(wx, wz);
			sy = project_y(wy, wz);
			emit_line(sx, sy, sx, sy);
		}
	}

	// Nucleus: a tight clump rather than a single pixel, so it blooms
	// into a solid blob at this scale and the arms look like they're
	// coming from somewhere.
	{
		int32_t cr = g->radius / 9;
		int32_t off[9][2] = {
			{0,0}, {cr,0}, {-cr,0}, {0,cr}, {0,-cr},
			{cr,cr}, {-cr,cr}, {cr,-cr}, {-cr,-cr}
		};
		int i;
		for (i = 0; i < 9; i++) {
			int32_t wx = g->x + off[i][0];
			int32_t wy = g->y + ((off[i][1] * ct) >> 12);
			int32_t wz = g->z + ((off[i][1] * st) >> 12);
			int32_t sx, sy;
			if (wz < NEAR_Z) continue;
			sx = project_x(wx, wz);
			sy = project_y(wy, wz);
			emit_line(sx, sy, sx, sy);
		}
	}
}

static void build_galaxies(void) {
	int i;
	for (i = 0; i < MAX_GALAXIES; i++)
		if (galaxies[i].active) draw_galaxy(&galaxies[i]);
}

static void build_stars(void) {

	int i;

	for (i = 0; i < NUM_STARS; i++) {

		int32_t z = star_z[i];
		int32_t sx, sy;

		if (z < NEAR_Z) continue;

		sx = project_x(star_x[i], z);
		sy = project_y(star_y[i], z);

		if (z < STREAK_Z) {
			// Streak back toward where the star was a moment ago,
			// which is toward the vanishing point -- so the whole
			// field appears to radiate outward from the centre. The
			// tail shortens naturally with distance because it's a
			// fixed length in world space, not screen space.
			int32_t z2 = z + STREAK_LEN;
			emit_line(sx, sy, project_x(star_x[i], z2),
			          project_y(star_y[i], z2));
		} else {
			// a single pixel: the rasterizer draws a zero-length
			// line as exactly one
			emit_line(sx, sy, sx, sy);
		}
	}
}

static void build_objects(void) {

	int i;

	for (i = 0; i < MAX_OBJECTS; i++) {
		object_t *o = &objects[i];
		if (!o->active) continue;
		draw_model(&models[o->model], o->x, o->y, o->z,
		           o->radius, o->rx, o->ry);
	}
}

static void build_missiles(void) {

	int i;

	for (i = 0; i < MAX_MISSILES; i++) {

		missile_t *m = &missiles[i];
		if (!m->active) continue;

		// A diamond perpendicular to the flight axis, plus a tracer
		// along it.
		//
		// The tracer alone was the first attempt and it barely showed
		// up, for a reason worth recording: a segment lying along the
		// view axis projects to a length proportional to how far the
		// missile is from the axis, so a shot travelling almost
		// straight down the centre of the screen -- which is every
		// shot, since that's where the gun points -- projects to
		// almost nothing. Only a few pixels ever appeared, jammed
		// under the reticle. A shape with real lateral extent
		// projects at a size that falls off with distance the way a
		// receding object should, and the tracer becomes the detail
		// that sells the motion rather than the whole of the bolt.
		//
		// A diamond and not a cross, which is what this was first:
		// the reticle is also a cross, so with three or four shots in
		// flight the middle of the screen filled up with plus signs
		// and the aiming mark stopped being findable among them. Two
		// extra lines per missile buys a silhouette that can't be
		// mistaken for the sight.
		emit_edge(m->x, m->y + MISSILE_BOLT, m->z,
		          m->x + MISSILE_BOLT, m->y, m->z);
		emit_edge(m->x + MISSILE_BOLT, m->y, m->z,
		          m->x, m->y - MISSILE_BOLT, m->z);
		emit_edge(m->x, m->y - MISSILE_BOLT, m->z,
		          m->x - MISSILE_BOLT, m->y, m->z);
		emit_edge(m->x - MISSILE_BOLT, m->y, m->z,
		          m->x, m->y + MISSILE_BOLT, m->z);
		emit_edge(m->x, m->y, m->z, m->x, m->y, m->z - MISSILE_TAIL);
	}
}

static void build_explosions(void) {

	int i, j;

	for (i = 0; i < MAX_EXPLOSIONS; i++) {

		explosion_t *e = &explosions[i];
		int32_t len;

		if (!e->active) continue;

		len = (int32_t)e->t * EXPL_STEP * e->size / 100;

		for (j = 0; j < 8; j++) {
			// Shards fly outward from the centre and the inner end
			// pulls away from it too, so the burst hollows out as it
			// expands rather than staying a solid asterisk.
			int32_t dx = expl_dirs[j][0];
			int32_t dy = expl_dirs[j][1];
			int32_t dz = expl_dirs[j][2];
			int32_t inner = len / 2;
			emit_edge(e->x + dx * inner / 128,
			          e->y + dy * inner / 128,
			          e->z + dz * inner / 128,
			          e->x + dx * len / 128,
			          e->y + dy * len / 128,
			          e->z + dz * len / 128);
		}
	}
}

// Cockpit furniture, in screen space -- no projection, no clipping
// needed beyond what emit_line() does anyway. Redrawn every frame
// along with everything else: it could be drawn once and left alone,
// but then a rock passing over it would punch permanent holes in it
// when its own lines are erased next frame.
static void build_hud(void) {

	int32_t cx = view_cx, cy = view_cy;
	int32_t bx0 = clip_x0, by0 = clip_y0, bx1 = clip_x1, by1 = clip_y1;
	const int32_t B = 14;   // corner bracket arm length

	// centre reticle: four ticks with a gap in the middle, so the
	// thing you're aiming at stays visible
	emit_line(cx - 9, cy, cx - 3, cy);
	emit_line(cx + 3, cy, cx + 9, cy);
	emit_line(cx, cy - 9, cx, cy - 3);
	emit_line(cx, cy + 3, cx, cy + 9);

	// corner brackets
	emit_line(bx0, by0, bx0 + B, by0);
	emit_line(bx0, by0, bx0, by0 + B);
	emit_line(bx1, by0, bx1 - B, by0);
	emit_line(bx1, by0, bx1, by0 + B);
	emit_line(bx0, by1, bx0 + B, by1);
	emit_line(bx0, by1, bx0, by1 - B);
	emit_line(bx1, by1, bx1 - B, by1);
	emit_line(bx1, by1, bx1, by1 - B);
}

// ===========================================================================
// FRAME OUTPUT
// ===========================================================================

// Reconcile what's on screen with what should be.
//
// The first version of this erased the entire previous frame and then
// drew the entire new one -- two hardware line operations per line,
// every frame, whether or not anything had changed. On a single-
// buffered framebuffer that is not merely wasteful, it is visible:
// every line on screen spends the gap between its erase and its
// redraw switched off, so the whole scene strobes at the frame rate.
// The static furniture suffered worst, because the corner brackets
// and the reticle never move and so were being blanked and restored
// hundreds of times for no reason at all.
//
// This version erases only lines that actually differ from last
// frame, and draws all of them. Two things follow, and the second is
// the one that matters:
//
//  - Anything that didn't move is never switched off. The corners and
//    reticle now sit there continuously. Drawing a lit pixel again is
//    a no-op on 1bpp, so redrawing them costs nothing visually.
//
//  - There is no hole-punching problem. Erasing a line that crosses a
//    kept line does blank the shared pixel -- but the draw pass runs
//    afterwards and covers every line, so the crossing is repaired
//    within the same frame. That is precisely why the draw pass stays
//    unconditional instead of also being skipped for unchanged lines,
//    which would be faster still and would leave permanent scars
//    across the HUD.
//
// Comparison is index-wise, which is a heuristic about efficiency and
// never about correctness: every old line is either matched or
// erased, and every new line is drawn, whatever the indices happen to
// line up as. Emitting in a stable order (HUD first, it is always the
// same twelve lines) just makes the heuristic hit more often.
static void blit_frame(void) {

	int prev = cur_buf ^ 1;
	int n_new = line_count[cur_buf];
	int n_old = line_count[prev];
	int i;

	for (i = 0; i < n_old; i++) {
		line_t *o = &line_buf[prev][i];
		if (i < n_new) {
			line_t *n = &line_buf[cur_buf][i];
			if (o->x0 == n->x0 && o->y0 == n->y0 &&
			    o->x1 == n->x1 && o->y1 == n->y1)
				continue;          // unchanged: leave it lit
		}
		z_win_hw_line(&win, o->x0, o->y0, o->x1, o->y1, 0);
	}

	for (i = 0; i < n_new; i++) {
		line_t *l = &line_buf[cur_buf][i];
		z_win_hw_line(&win, l->x0, l->y0, l->x1, l->y1, 1);
	}

	line_count[prev] = 0;
	cur_buf = prev;
}

static void draw_hud_text(void) {

	char buf[24];

	// Zero-padded to a fixed width on purpose. The glyph blitter
	// clears the background of every cell it writes (z_fb_draw_char(),
	// zgfx.c), so redrawing text in place is self-erasing -- but only
	// over the cells it actually touches. A shorter string leaves the
	// tail of the previous, longer one on screen, so "SCORE 10"
	// followed by "SCORE 0" after a restart would read "SCORE 00".
	// This is the same bug that stranded the old game-over banner:
	// text that stops being drawn is not text that gets erased.
	snprintf(buf, sizeof(buf), "SCORE %03lu", (unsigned long)score);
	z_win_draw_text(&win, 4, 2, buf, 1, &z_font_5x8);
}

// Fills or clears the whole content area through the blitter.
//
// Deliberately z_fb_hw_fill_rect() and not z_win_fill_rect(): the
// latter routes to z_fb_fill_rect(), which is a software per-pixel
// loop, and at this window size that's over seventy thousand
// read-modify-write cycles on the framebuffer -- fine for the
// once-per-lifetime clear it was written for, hopeless for something
// that has to happen eight frames running. The blitter does the same
// area in a fraction of the time. Reaching past the z_win_* layer is
// safe here only because the rectangle passed is exactly this
// window's own content rect, which is the thing that layer would
// otherwise be clipping to.
// defined further down, next to the rest of the setup code; the crash
// strobe needs it a good deal earlier
static void reset_game(void);

static void fill_content(int color) {
	z_fb_hw_fill_rect(clip_x0, clip_y0,
	                  clip_x1 - clip_x0 + 1, clip_y1 - clip_y0 + 1, color);
}

// One frame of the crash strobe. Alternates filled and cleared, and
// on the last frame leaves the window black and starts a fresh run.
//
// The display lists are dropped rather than replayed, for the same
// reason a wm redraw drops them: the blitter has just overwritten
// every pixel either way, so the "what's currently on screen" list is
// no longer true, and replaying its erase pass would punch black
// lines through a window that's already clean.
static void run_flash_frame(void) {

	// Starts on white: FLASH_FRAMES is even, so testing the low bit
	// the other way round would open the strobe on a black frame,
	// which is indistinguishable from the game simply stopping.
	fill_content((flash_timer & 1) ? 0 : 1);

	line_count[0] = 0;
	line_count[1] = 0;

	if (--flash_timer <= 0) {
		fill_content(0);
		reset_game();
	}
}

static void render(void) {

	line_count[cur_buf] = 0;

	// Ordered most-static first, so blit_frame()'s index-wise
	// comparison lines up as often as possible. The HUD leads because
	// it is always exactly the same twelve lines at the same indices
	// and therefore always matches; galaxies follow because they are
	// the next slowest thing in the scene. Draw order carries no
	// visual meaning here -- everything is the same colour.
	build_hud();
	build_galaxies();
	build_stars();
	build_objects();
	build_missiles();
	build_explosions();

	blit_frame();

	// Text goes on last so it wins over any line crossing it. The
	// glyph blitter writes both foreground and background of every
	// cell it touches (z_fb_draw_char(), zgfx.c), so this also erases
	// the previous frame's text without a separate clear.
	draw_hud_text();
}

// ===========================================================================
// SPAWNING
// ===========================================================================

static void spawn_star(int i, bool anywhere) {

	// Spawned across the frustum cross-section at the far plane, so
	// as the field advances the stars spread outward and leave the
	// view at the edges -- which is what produces an even screen-space
	// density without ever having to think about density directly.
	int32_t z = anywhere ? rnd_range(NEAR_Z * 4, STAR_FAR) : STAR_FAR;
	int32_t half = (z * 157) >> PROJ_SHIFT;

	star_z[i] = z;
	star_x[i] = rnd_range(-half, half);
	star_y[i] = rnd_range(-half, half);
}

// Re-rolls everything about a galaxy except where it is. Called on
// first setup and again each time one wraps around out of sight, so
// the sky slowly turns over instead of being the same three objects
// for the whole session.
static void roll_galaxy_look(galaxy_t *g) {

	int32_t half = (g->z * 157) >> PROJ_SHIFT;

	g->y = rnd_range(-half / 2, half / 2);

	// Sized by how big it should look, not by how big it is.
	//
	// Picking a world radius directly and letting the projection do
	// what it likes was the obvious way and it doesn't work: apparent
	// size is radius/z, so across the Z range above the same number
	// gave anything from a smudge to a spiral wider than the window,
	// and at the large end fourteen dots per arm had to cover a
	// seventy-pixel radius, which spaces them ten pixels apart and
	// turns the arms into dotted lines. Choosing the on-screen radius
	// first and solving back for the world radius keeps dot spacing
	// -- the thing that actually decides whether an arm reads as an
	// arm -- roughly constant no matter where the galaxy sits.
	{
		int32_t proj_r = rnd_range(16, 30);   // wanted radius, pixels
		g->radius = (int16_t)((g->z * proj_r) >> PROJ_SHIFT);
	}
	// Inclination, where 0 is face-on and 64 is edge-on. Biased
	// toward face-on: the earlier 22-54 range put several galaxies
	// past 70 degrees, where the spiral collapses into a featureless
	// streak and all the work the arms do is lost. This still spans
	// enough to keep them from looking stamped from one template.
	g->tilt = (uint8_t)rnd_range(10, 44);
	g->spin = (uint16_t)rnd_range(0, 65535);
	g->spin_rate = (uint16_t)rnd_range(GALAXY_SPIN_MIN, GALAXY_SPIN_MAX);
	g->vx = rnd_range(-4, 4);
	g->vy = rnd_range(-2, 2);
}

// Brings a galaxy on from one side, aimed across the view.
//
// Entry can't be left to the parallax drift alone: that depends
// entirely on how the player happens to be steering, so a galaxy
// could sit just off the edge indefinitely, or be pushed straight
// back out the way it came. Giving it a proper motion of its own,
// pointed inward and scaled so the crossing takes a set time
// regardless of how far away it is, means an appearance always
// actually happens.
static void wake_galaxy(galaxy_t *g) {

	int32_t half, lim, side, speed_x;

	g->z = rnd_range(GALAXY_Z_MIN, GALAXY_Z_MAX);
	half = (g->z * 157) >> PROJ_SHIFT;
	lim = half * 5 / 4;

	roll_galaxy_look(g);

	side = rnd_range(0, 1) ? 1 : -1;
	g->x = side * lim;

	// Crossing time is what's chosen; the world-space speed is solved
	// back from it, so a distant galaxy and a nearer one take about
	// as long to sail past rather than the far one crawling.
	speed_x = (2 * lim) / rnd_range(GALAXY_CROSS_MIN, GALAXY_CROSS_MAX);
	if (speed_x < 1) speed_x = 1;
	g->vx = -side * speed_x;
	g->vy = rnd_range(-2, 2);

	g->dormant = 0;
	g->active = true;
}

static void sleep_galaxy(galaxy_t *g) {
	g->active = false;
	g->dormant = rnd_range(GALAXY_DORMANT_MIN, GALAXY_DORMANT_MAX);
}

static void update_galaxies(void) {

	int i;

	for (i = 0; i < MAX_GALAXIES; i++) {

		galaxy_t *g = &galaxies[i];
		int32_t half, lim;

		if (!g->active) {
			if (--g->dormant <= 0) wake_galaxy(g);
			continue;
		}

		g->spin = (uint16_t)(g->spin + g->spin_rate);

		// Fractional parallax, and no forward motion at all -- see
		// GALAXY_PARALLAX. The galaxy's own drift is what keeps the
		// sky slowly evolving when the player flies dead straight and
		// contributes nothing to the parallax term.
		g->x += g->vx - ship_vx;
		g->y += g->vy - ship_vy;

		half = (g->z * 157) >> PROJ_SHIFT;
		lim = half * 5 / 4;

		// Gone once it's fully off the side, and it stays gone for a
		// while -- see sleep_galaxy().
		//
		// An earlier version wrapped it straight round to the other
		// side instead, which was itself a fix for an even earlier
		// one that respawned onto a random side and could bounce a
		// galaxy off-screen indefinitely. Wrapping worked, but it
		// guaranteed a galaxy was almost always somewhere in view,
		// and two of them at once was busy rather than beautiful.
		// Dormancy is what makes them scenery you come across instead
		// of wallpaper you sit in front of.
		if (g->x > lim || g->x < -lim) {
			sleep_galaxy(g);
			continue;
		}

		if (g->y > lim)       { g->y -= 2 * lim; }
		else if (g->y < -lim) { g->y += 2 * lim; }
	}
}

static void spawn_object(void) {

	int i;
	object_t *o = NULL;

	for (i = 0; i < MAX_OBJECTS; i++) {
		if (!objects[i].active) { o = &objects[i]; break; }
	}
	if (!o) return;

	memset(o, 0, sizeof(*o));
	o->active = true;
	o->z = SPAWN_Z + rnd_range(0, 700);

	// Spread wider than the cockpit so not everything is a head-on
	// threat -- a good number of these are meant to sail past to one
	// side and sell the sense of moving through a field rather than
	// down a corridor.
	o->x = rnd_range(-1100, 1100);
	o->y = rnd_range(-750, 750);

	// One in five is a UFO. The old version also gated these behind a
	// few points, so a new player's first encounter was always a
	// plain rock -- which made sense when rocks arrived every second
	// and stopped making sense once they became rare enough that
	// reaching that score took minutes. Meeting a saucer first is a
	// better opening than waiting for permission to.
	if (rnd_range(0, 4) == 0) {
		o->is_ufo = true;
		o->model = MODEL_UFO;
		o->radius = rnd_range(90, 120);
		o->vz = 14;                       // closes a little faster
		o->weave = (uint8_t)rnd_range(0, 255);
		o->drx = 0;
		o->dry = (int8_t)rnd_range(2, 4);  // spins on its own axis
	} else {
		o->is_ufo = false;
		o->model = (uint8_t)rnd_range(0, 2);
		o->radius = rnd_range(110, 210);
		o->vx = rnd_range(-7, 7);
		o->vy = rnd_range(-5, 5);
		o->drx = (int8_t)rnd_range(-4, 4);
		o->dry = (int8_t)rnd_range(-4, 4);
	}

	o->rx = (uint8_t)rnd_range(0, 255);
	o->ry = (uint8_t)rnd_range(0, 255);
}

static void spawn_explosion(int32_t x, int32_t y, int32_t z, int32_t size) {

	int i;

	for (i = 0; i < MAX_EXPLOSIONS; i++) {
		if (!explosions[i].active) {
			explosions[i].active = true;
			explosions[i].x = x;
			explosions[i].y = y;
			explosions[i].z = z;
			explosions[i].size = (int16_t)size;
			explosions[i].t = 1;
			return;
		}
	}
}

static void fire_missile(void) {

	int i;

	if (fire_cooldown > 0) return;

	for (i = 0; i < MAX_MISSILES; i++) {
		if (!missiles[i].active) {
			missiles[i].active = true;
			// Alternating muzzles, offset below the sight line, so
			// shots visibly converge from under the canopy instead of
			// appearing out of the middle of the reticle.
			missiles[i].x = muzzle_side ? 46 : -46;
			missiles[i].y = -16;
			missiles[i].z = MISSILE_SPAWN_Z;
			muzzle_side ^= 1;
			fire_cooldown = FIRE_COOLDOWN;
			return;
		}
	}
}

// ===========================================================================
// SIMULATION
// ===========================================================================

static void update_ship(void) {

	int32_t ax = 0, ay = 0;

	if (key_left)  ax -= SHIP_ACCEL;
	if (key_right) ax += SHIP_ACCEL;
	if (key_up)    ay += SHIP_ACCEL;
	if (key_down)  ay -= SHIP_ACCEL;

	ship_vx += ax;
	ship_vy += ay;

	// Coast to a stop when nothing is held. Note this is a shift on a
	// signed value, which rounds toward negative infinity -- so a
	// small negative velocity decays to zero rather than sticking at
	// -1 the way a naive symmetric version would.
	if (ax == 0) ship_vx -= ship_vx >> SHIP_DAMP_SHIFT;
	if (ay == 0) ship_vy -= ship_vy >> SHIP_DAMP_SHIFT;

	if (ship_vx >  SHIP_VMAX) ship_vx =  SHIP_VMAX;
	if (ship_vx < -SHIP_VMAX) ship_vx = -SHIP_VMAX;
	if (ship_vy >  SHIP_VMAX) ship_vy =  SHIP_VMAX;
	if (ship_vy < -SHIP_VMAX) ship_vy = -SHIP_VMAX;
}

static void update_stars(void) {

	int i;

	for (i = 0; i < NUM_STARS; i++) {

		star_z[i] -= speed;
		star_x[i] -= ship_vx;
		star_y[i] -= ship_vy;

		// Recycled once behind us, or once so far off-axis that it
		// can no longer be on screen (the frustum edge is at 0.61z,
		// so 4z is safely past it while still catching a star long
		// before its projection gets large).
		if (star_z[i] < NEAR_Z ||
		    star_x[i] > 4 * star_z[i] || star_x[i] < -4 * star_z[i] ||
		    star_y[i] > 4 * star_z[i] || star_y[i] < -4 * star_z[i]) {
			spawn_star(i, false);
		}
	}
}

static void update_objects(void) {

	int i;

	for (i = 0; i < MAX_OBJECTS; i++) {

		object_t *o = &objects[i];
		if (!o->active) continue;

		o->z -= speed + o->vz;
		o->x += o->vx - ship_vx;
		o->y += o->vy - ship_vy;

		if (o->is_ufo) {
			// Weaving is a sine on the phase rather than integrated
			// acceleration: it keeps the saucer's excursion bounded
			// no matter how long it's on screen, which a random walk
			// would not.
			o->weave += 5;
			o->x += (SIN(o->weave) * 26) >> 12;
			o->y += (SIN(o->weave * 2) * 9) >> 12;
		}

		o->rx += o->drx;
		o->ry += o->dry;

		if (o->z < DESPAWN_Z ||
		    o->x > WORLD_LIMIT || o->x < -WORLD_LIMIT ||
		    o->y > WORLD_LIMIT || o->y < -WORLD_LIMIT) {
			o->active = false;
		}
	}
}

static void update_missiles(void) {

	int i;

	for (i = 0; i < MAX_MISSILES; i++) {

		missile_t *m = &missiles[i];
		if (!m->active) continue;

		// The world comes toward us at `speed` and the missile goes
		// away from us at MISSILE_SPEED, so its camera-relative
		// motion is the difference; against a target it's the sum.
		m->z += MISSILE_SPEED - speed;
		m->x -= ship_vx;
		m->y -= ship_vy;

		if (m->z > MISSILE_MAX_Z) m->active = false;
	}
}

static void update_explosions(void) {

	int i;

	for (i = 0; i < MAX_EXPLOSIONS; i++) {

		explosion_t *e = &explosions[i];
		if (!e->active) continue;

		e->z -= speed;
		e->x -= ship_vx;
		e->y -= ship_vy;
		e->t++;

		if (e->t >= EXPL_FRAMES) e->active = false;
	}
}

static void check_missile_hits(void) {

	int i, j;

	for (i = 0; i < MAX_MISSILES; i++) {

		missile_t *m = &missiles[i];
		if (!m->active) continue;

		for (j = 0; j < MAX_OBJECTS; j++) {

			object_t *o = &objects[j];
			int32_t dx, dy, dz, rxy, rz;

			if (!o->active) continue;

			dx = m->x - o->x; if (dx < 0) dx = -dx;
			dy = m->y - o->y; if (dy < 0) dy = -dy;
			dz = m->z - o->z; if (dz < 0) dz = -dz;

			rxy = o->radius + 40;

			// The Z tolerance has to exceed one frame of closing
			// distance or fast targets are tunnelled straight
			// through: the missile and the target approach at
			// MISSILE_SPEED + speed, which at maximum speed is under
			// 190, so radius + 190 can never step over a target that
			// a smaller box would have caught. A box rather than a
			// sphere -- these are lumpy rocks, and the difference is
			// not perceptible.
			rz = o->radius + MISSILE_SPEED + SPEED_MAX;

			if (dx < rxy && dy < rxy && dz < rz) {
				spawn_explosion(o->x, o->y, o->z, o->radius);
				o->active = false;
				m->active = false;
				score += o->is_ufo ? 5 : 1;
				break;
			}
		}
	}
}

static void check_crash(void) {

	int i;

	for (i = 0; i < MAX_OBJECTS; i++) {

		object_t *o = &objects[i];
		int32_t dx, dy, r;

		if (!o->active) continue;

		// A band rather than a plane, for the same tunnelling reason
		// as the missile test: nothing travels far enough in one
		// frame to step across it.
		if (o->z > CRASH_Z || o->z < DESPAWN_Z) continue;

		dx = o->x; if (dx < 0) dx = -dx;
		dy = o->y; if (dy < 0) dy = -dy;
		r = o->radius + SHIP_RADIUS;

		if (dx < r && dy < r) {
			o->active = false;
			game_over = true;
			flash_timer = FLASH_FRAMES;
			return;
		}
	}
}

static void update(void) {

	if (fire_cooldown > 0) fire_cooldown--;

	update_ship();
	update_galaxies();
	update_stars();
	update_objects();
	update_missiles();
	update_explosions();

	if (game_over) return;

	check_missile_hits();
	check_crash();

	// No difficulty ramp -- see SPEED_CRUISE. Speed is constant and
	// the spawn interval doesn't tighten with score, so the field
	// stays as sparse on the twentieth rock as on the first.
	if (--spawn_timer <= 0) {
		spawn_timer = (int)rnd_range(SPAWN_MIN, SPAWN_MAX);
		spawn_object();
	}
}

// ===========================================================================
// SETUP AND INPUT
// ===========================================================================

static void reset_game(void) {

	int i;

	memset(objects, 0, sizeof(objects));
	memset(missiles, 0, sizeof(missiles));
	memset(explosions, 0, sizeof(explosions));

	for (i = 0; i < NUM_STARS; i++) spawn_star(i, true);
	// One is brought straight on so a run doesn't necessarily open on
	// bare sky; the rest start dormant on staggered timers so they
	// don't all arrive together later.
	for (i = 0; i < MAX_GALAXIES; i++) {
		if (i == 0) {
			wake_galaxy(&galaxies[i]);
			// placed part-way across rather than at the edge, so it's
			// already in view at frame one
			galaxies[i].x /= 2;
		} else {
			sleep_galaxy(&galaxies[i]);
			galaxies[i].dormant = rnd_range(600, GALAXY_DORMANT_MAX);
		}
	}

	ship_vx = ship_vy = 0;
	speed = SPEED_CRUISE;
	score = 0;
	game_over = false;
	flash_timer = 0;
	fire_cooldown = 0;
	// A long first interval: the run opens on empty sky, which is the
	// tone the rest of it is going for.
	spawn_timer = (int)rnd_range(SPAWN_MIN, SPAWN_MAX);
	muzzle_side = 0;
	key_left = key_right = key_up = key_down = false;
}

static void update_geometry(void) {

	z_clip_t clip;

	z_win_content_rect(&win, &clip);

	clip_x0 = clip.x0;
	clip_y0 = clip.y0;
	clip_x1 = clip.x1;
	clip_y1 = clip.y1;

	view_cx = (clip_x0 + clip_x1) / 2;
	view_cy = (clip_y0 + clip_y1) / 2;
}

static void handle_key(uint32_t packed) {

	uint32_t keysym = Z_WM_UNPACK_KEY_KEYSYM(packed);
	bool pressed = Z_WM_UNPACK_KEY_PRESSED(packed) != 0;

	// Arrows are tracked as held state rather than acted on per event:
	// Z_WM_KEY delivers real press and release edges (sw/os/hid.c
	// diffs the HID report itself), so there's no auto-repeat to fight
	// and holding a key gives smooth continuous thrust.
	switch (keysym) {
		case Z_KEY_LEFT:  key_left  = pressed; return;
		case Z_KEY_RIGHT: key_right = pressed; return;
		case Z_KEY_UP:    key_up    = pressed; return;
		case Z_KEY_DOWN:  key_down  = pressed; return;
	}

	if (!pressed) return;

	// No restart key any more -- a crash strobes the window and starts
	// a fresh run by itself (see run_flash_frame()). The old "PRESS R"
	// banner is gone with it: nothing here ever cleared that text, so
	// it stayed on screen through the next run.
	if (keysym == ' ') {
		if (!game_over) fire_missile();
	}
}

// Non-blocking message drain. Z_WM_REDRAW means the wm has just
// cleared the whole screen, so the erase list is stale -- dropping it
// rather than replaying it is both faster and necessary: those lines
// are gone, and drawing them in colour 0 over a freshly drawn window
// would punch holes in the wm's own chrome. Z_WM_WINDOW_MOVED updates
// geometry but is deliberately not its own redraw trigger, for the
// reason set out in hello_win's drain_messages().
static bool drain_messages(void) {

	bool got_redraw = false;
	z_msg_t msg;

	while (z_msg_read(&msg) == Z_OK) {
		if (msg.subject == Z_WM_REDRAW) {
			z_win_apply_redraw(&win, msg.obj.val.uint32);
			update_geometry();
			line_count[cur_buf ^ 1] = 0;
			got_redraw = true;
		} else if (msg.subject == Z_WM_WINDOW_MOVED) {
			z_win_parse_rect(&win, &msg.obj);
			update_geometry();
			line_count[cur_buf ^ 1] = 0;
		} else if (msg.subject == Z_WM_KEY) {
			handle_key(msg.obj.val.uint32);
		}
	}

	return got_redraw;
}

int main(void) {

	uint32_t next_frame;

	printf("space3d: wireframe space game\n");

	if (z_win_create_flags(&win, "space3d", WIN_W, WIN_H, -1, -1,
	    Z_WIN_FLAG_CLOSE_ICON | Z_WIN_FLAG_CLOSE_KILLS_OWNER) != Z_OK) {
		printf("space3d: failed to create window\n");
		return 1;
	}

	update_geometry();
	fill_content(0);

	// Seeded from the tick counter so two runs in a session differ.
	// Nonzero is required -- xorshift is stuck at zero -- and the
	// counter could plausibly be zero if the window came up on the
	// very first tick after boot.
	rng_state = z_uptime_ticks() * 2654435761u;
	if (rng_state == 0) rng_state = 0x2545f491u;

	reset_game();

	printf("space3d: window %ld at (%ld,%ld) %ldx%ld, content %ldx%ld\n",
	       (long)win.id, (long)win.x, (long)win.y, (long)win.w, (long)win.h,
	       (long)(clip_x1 - clip_x0 + 1), (long)(clip_y1 - clip_y0 + 1));

	next_frame = z_uptime_ticks();

	while (1) {

		bool redrew = drain_messages();
		uint32_t now = z_uptime_ticks();

		// Signed comparison of the difference, so this stays correct
		// across the tick counter's own 32-bit wrap (~68 days at
		// 732Hz) instead of stalling for the rest of the session.
		if (!redrew && (int32_t)(now - next_frame) < 0) continue;

		next_frame += FRAME_TICKS;
		// If a frame overran badly (or the app was descheduled for a
		// while), don't try to catch up by running a burst of frames
		// back to back -- resync instead. Catching up would make the
		// game briefly unplayable at exactly the moment it's already
		// struggling.
		if ((int32_t)(now - next_frame) > 0) next_frame = now + FRAME_TICKS;

		if (flash_timer > 0) {
			run_flash_frame();
		} else {
			update();
			render();
		}

		if (redrew) z_win_redraw_done(&win);

		if (overflowed) {
			printf("space3d: display list full (%d lines) -- "
			       "raise MAX_LINES\n", MAX_LINES);
			overflowed = false;
		}
	}

	return 0;
}
