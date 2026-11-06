/*
 * mausrun3d -- Maus Run in an open 3D world (docs/mausrun3d.md).
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The mouse runs about an island made from one of mausrun's levels,
 * the camera behind it, in game mode on chunky VRAM: drawn by T2 where
 * the bitstream has it (docs/zgl_t2.md), and only playable there --
 * the triangle engine and the software draw it, a few frames a second.
 *
 *   > run mausrun3d
 *
 * mausrun's game, in three dimensions: collect the cheese, bounce on
 * the cats (dizzy, then a nap), bump the ? boxes, take the power-ups,
 * find the hidden star, reach the cheese wheel; a cat, a frog, an owl
 * or thorns cost a heart, a pit a heart and back to the last mouse
 * hole. Its sounds and tunes are mausrun's own (../mausrun/sound.c).
 *
 * -- the frame --
 *
 *   1. the sky: a gradient above the horizon, the haze below it, in
 *      picture coordinates (four triangles);
 *   2. the world: the cells in the view, out to the draw distance, each
 *      one's face sets that can face the eye (world.c), lit;
 *   3. everyone and everything in it near enough, display lists under
 *      their matrices (models.c); the mouse and its shadow;
 *   4. the HUD and any message, as lines (vfont.c).
 *
 * Everything drawn every frame in 3D is a display list, so on T2 the
 * CPU's part of a frame is the lists' words.
 *
 * -- units --
 *
 * A tile is W3_T (16) world units across and a block 16 high. Positions
 * are kept in 1/256 world units (Q8): a tile is 4096, and a tile index
 * is the position >> 12.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../../common/zfont.h"
#include "../../common/zpaths.h"
#include "../../common/games/zarcade.h"
#include "../../common/zgl/zgl.h"
#include "../mausrun/sound.h"
#include "world.h"
#include "models.h"
#include "vfont.h"

#ifndef AR_HOST_TESTS
#include "../../common/zfsapp.h"
#endif

extern const char mr3_world01[];           /* world_builtin.c */

/* -- the view ---------------------------------------------------------------- */

#define NEAR      16                 /* world units */
#define FAR       352                /* the draw distance at most: 22 tiles */
#define CAM_DIST  76                 /* behind the mouse */
#define CAM_LIFT  10                 /* the point looked at, above its feet */
#define PITCH_DEG 16                 /* looking down */
#define FOCAL     240                /* pixels: the frustum's 8/16 for 120 */

/* -- the mouse --------------------------------------------------------------- */

#define Q         256                /* Q8 */
#define TQ        (W3_T * Q)         /* a tile */
#define R_MOUSE   (5 * Q)            /* half its width */
#define H_MOUSE   (11 * Q)           /* its height */
#define SPEED     (Q * 7 / 4)        /* world units a tick, running */
#define GRAVITY   (Q * 32 / 100)
#define JUMP_V    (Q * 59 / 10)      /* 3.4 tiles high */
#define SPRING_V  (JUMP_V * 5 / 4)   /* 5 tiles */
#define BOUNCE_V  (JUMP_V * 3 / 4)   /* off a head */
#define FALL_MAX  (8 * Q)
#define SWIM_SPEED (SPEED * 6 / 10)
#define HEARTS    3
#define HEARTS_MAX 7
#define BLINK     120                /* ticks it cannot be hurt after a bump */
#define POWER_TICKS 1200             /* a power-up's 20 seconds (normal) */

static w3_world_t *world;
static int32_t mx, my, mz;           /* the mouse's feet, Q8 */
static int32_t vx, vy, vz;
static uint32_t heading;             /* turn16: 0 north (-z) */
static bool grounded, swimming, was_swimming, air_jump;
static int32_t start_x, start_y, start_z;
static int32_t check_x, check_y, check_z;    /* the last mouse hole */
static uint32_t cam_yaw;             /* turn16 */
static int32_t cam_x, cam_y, cam_z;  /* world units */
static uint32_t ticks, run_ticks;
static int hearts = HEARTS, blink;
static int cheese_got, cheese_all;
static bool star_got;
static int stride;                   /* a running frame */

/* power-ups: ticks left (0 none); the bubble until it is used */
enum { P_FEATHER, P_MAGNET, P_BELL, P_SHOES, P_N };
static int power[P_N];
static bool bubble;

/* what the screen says over the game */
enum { G_CARD, G_PLAY, G_DONE, G_OVER };
static int gstate = G_CARD;
static int gtimer;

/* -- everyone and everything in the world ------------------------------------- */

enum {
	A_CHEESE, A_CAT, A_SLEEPER, A_FROG, A_OWL, A_HOLE, A_WHEEL, A_STAR,
	A_FEATHER, A_BUBBLE, A_MAGNET, A_BELL, A_SHOES, A_HEART
};
enum { S_WALK, S_DIZZY, S_NAP, S_SLEEP, S_CHASE };

typedef struct {
	int32_t x, y, z;                 /* Q8: feet */
	int32_t lo, hi;                  /* its beat along its axis, Q8 */
	int32_t vy;                      /* a frog's hop, a popped power-up */
	uint32_t dir;                    /* heading, turn16 */
	int16_t timer;
	uint8_t kind, state;
	int8_t ax, az;                   /* its beat's axis: 1 x, or z */
	int8_t way;                      /* +1, -1 along it */
	bool alive, lit;
} actor_t;

#define MAX_ACTORS 512
static actor_t act[MAX_ACTORS];
static int nact;

/* blocks changed while playing, to put back for a new try */
typedef struct { int16_t x, z; uint8_t y, was; int16_t back; } change_t;
#define MAX_CHANGES 128
static change_t chg[MAX_CHANGES];
static int nchg;

/* -- chunky, or a message ----------------------------------------------------- */

static bool chunky;                  /* zgl open on chunky VRAM */
static bool sound_on;

static void paint(void *user, const ar_rect_t *r) {
	(void)user;
	ar_fill(r->x, r->y, r->w, r->h, 0);
	ar_text(40, 100, "MAUSRUN3D NEEDS GAME MODE", 1, &z_font_5x8, 1);
	ar_text(40, 116, "IN COLOUR: PRESS F11 (AND C)", 1, &z_font_5x8, 1);
	ar_text(40, 132, "IT IS MADE FOR T2", 1, &z_font_5x8, 1);
}

static const char help_text[] =
	"Maus Run in 3D: run about the island, collect the cheese, "
	"find the hidden star and reach the cheese wheel.\n\n"
	"Arrows\tRun (as the camera looks)\n"
	"Space\tJump; in water, a stroke\n"
	"Q E\tTurn the camera\n"
	"Enter\tThe camera behind the mouse\n"
	"M\tMusic on or off\n"
	"F11\tGame mode, or back to the window\n"
	"Esc\tLeave game mode";

static ar_desc_t desc = {
	.name = "mausrun3d",
	.help_title = "mausrun3d help",
	.help_text = help_text,
	.help_doc = "mausrun3d",
	.palette = { [1] = 0xfff },
	.ink = { [1] = { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } } },
	.paint = paint,
};

static void sound(int which) {
	if (sound_on) snd_play(which, -1);
}

/* -- the world ----------------------------------------------------------------- */

static uint32_t hash(int32_t a, int32_t b) {
	uint32_t h = (uint32_t)a * 374761393u + (uint32_t)b * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	return h ^ (h >> 16);
}

static bool load_world(void) {
	const char *err;
	int line;
	const char *text = mr3_world01;
	char *card = NULL;
#ifndef AR_HOST_TESTS
	{
		char path[] = Z_PATH_MAUSRUN3D_WORLD1;
		int len = fs_size(path);
		if (len > 0 && (card = malloc((size_t)len + 1)) != NULL) {
			if (fs_read_file(path, card, len + 1) == len) {
				card[len] = 0;
				text = card;
			} else {
				free(card);
				card = NULL;
			}
		}
	}
#endif
	world = w3_parse(text, &err, &line);
	if (!world && card) {
		printf("mausrun3d: the card's world: line %d: %s -- the built-in one instead\n", line, err);
		world = w3_parse(mr3_world01, &err, &line);
	}
	free(card);
	if (!world) {
		printf("mausrun3d: the world: line %d: %s\n", line, err);
		return false;
	}
	return true;
}

static bool walkable(int tx, int ty, int tz) {
	uint8_t below = w3_block(world, tx, ty - 1, tz);
	return below != W3_AIR && below != W3_THORNS &&
		w3_block(world, tx, ty, tz) == W3_AIR && w3_block(world, tx, ty + 1, tz) == W3_AIR;
}

/* a foe's beat: along x or z, whichever runs further on the level it
 * stands on, up to four tiles each way */
static void beat(actor_t *a, int tx, int ty, int tz, bool fly) {
	int best = -1, k, ax;
	for (ax = 0; ax < 2; ax++) {
		int dx = ax ? 0 : 1, dz = ax ? 1 : 0, lo = 0, hi = 0;
		for (k = 1; k <= 4; k++) {
			if (fly ? w3_block(world, tx - dx * k, ty, tz - dz * k) : !walkable(tx - dx * k, ty, tz - dz * k)) break;
			lo = k;
		}
		for (k = 1; k <= 4; k++) {
			if (fly ? w3_block(world, tx + dx * k, ty, tz + dz * k) : !walkable(tx + dx * k, ty, tz + dz * k)) break;
			hi = k;
		}
		if (lo + hi > best) {
			int32_t c = ax ? a->z : a->x;
			best = lo + hi;
			a->ax = (int8_t)dx; a->az = (int8_t)dz;
			a->lo = c - lo * TQ;
			a->hi = c + hi * TQ;
		}
	}
	a->way = 1;
	a->dir = a->ax ? 16384u : 32768u;
}

static actor_t *add_actor(int kind, int32_t x, int32_t y, int32_t z) {
	actor_t *a;
	if (nact >= MAX_ACTORS) return NULL;
	a = &act[nact++];
	memset(a, 0, sizeof(*a));
	a->kind = (uint8_t)kind;
	a->x = x; a->y = y; a->z = z;
	a->alive = true;
	return a;
}

/* everyone and everything where the map puts them, every heart back: a
 * new try at the level */
static void start_level(void) {
	int i;
	nact = 0;
	cheese_all = cheese_got = 0;
	star_got = false;
	/* the blocks as they were */
	while (nchg > 0) {
		nchg--;
		w3_set_block(world, chg[nchg].x, chg[nchg].y, chg[nchg].z, chg[nchg].was);
	}
	start_x = start_z = world->n * TQ / 2;
	start_y = 8 * TQ;
	for (i = 0; i < world->nthings; i++) {
		const w3_thing_t *t = &world->things[i];
		int32_t x = t->x * TQ + TQ / 2, y = t->y * TQ, z = t->z * TQ + TQ / 2;
		actor_t *a = NULL;
		switch (t->c) {
		case 'M': start_x = x; start_y = y; start_z = z; break;
		case 'c': a = add_actor(A_CHEESE, x, y, z); cheese_all++; break;
		case 'C': a = add_actor(A_CAT, x, y, z); if (a) beat(a, t->x, t->y, t->z, false); break;
		case 'Z': a = add_actor(A_SLEEPER, x, y, z); if (a) { beat(a, t->x, t->y, t->z, false); a->state = S_SLEEP; } break;
		case 'F': a = add_actor(A_FROG, x, y, z); if (a) beat(a, t->x, t->y, t->z, false); break;
		case 'O': a = add_actor(A_OWL, x, y, z); if (a) beat(a, t->x, t->y, t->z, true); break;
		case 'H': a = add_actor(A_HOLE, x, y, z); break;
		case 'G': a = add_actor(A_WHEEL, x, y, z); break;
		case '*': a = add_actor(A_STAR, x, y, z); break;
		case 'w': a = add_actor(A_FEATHER, x, y, z); break;
		case 'b': a = add_actor(A_BUBBLE, x, y, z); break;
		case 'm': a = add_actor(A_MAGNET, x, y, z); break;
		case 'l': a = add_actor(A_BELL, x, y, z); break;
		case 'k': a = add_actor(A_SHOES, x, y, z); break;
		case 'h': a = add_actor(A_HEART, x, y, z); break;
		default: break;
		}
		(void)a;
	}
	check_x = start_x; check_y = start_y; check_z = start_z;
	mx = start_x; my = start_y; mz = start_z;
	vx = vy = vz = 0;
	grounded = false;
	hearts = HEARTS;
	blink = 0;
	bubble = false;
	memset(power, 0, sizeof(power));
	run_ticks = 0;
	gstate = G_CARD;
	gtimer = 180;
}

static void change_block(int x, int y, int z, uint8_t k, int back) {
	uint8_t was = w3_block(world, x, y, z);
	int i;
	for (i = 0; i < nchg; i++)
		if (chg[i].x == x && chg[i].y == y && chg[i].z == z) break;
	if (i == nchg) {
		if (nchg >= MAX_CHANGES) return;
		chg[i].x = (int16_t)x; chg[i].y = (uint8_t)y; chg[i].z = (int16_t)z;
		chg[i].was = was;
		nchg++;
	}
	chg[i].back = (int16_t)back;
	w3_set_block(world, x, y, z, k);
}

/* -- the mouse's physics -------------------------------------------------------- */

/* the direction (east, north) as turn16, 0 north and a quarter east:
 * atan by x(pi/4) + 0.273 x(1 - x), within a quarter of a degree */
static uint32_t angle_of(int32_t e, int32_t n) {
	int32_t ae = e < 0 ? -e : e, an = n < 0 ? -n : n;
	uint32_t a;
	int32_t x;                                   /* the smaller over the larger, 16.16 */
	if (!ae && !an) return heading;
	x = (int32_t)(((int64_t)(ae < an ? ae : an) << 16) / (ae < an ? an : ae));
	a = (uint32_t)((8192 * (int64_t)x + 2847 * (((int64_t)x * (65536 - x)) >> 16)) >> 16);
	if (ae > an) a = 16384 - a;
	if (e >= 0) return n >= 0 ? a : 32768 - a;
	return n >= 0 ? 65536 - a : 32768 + a;
}

/* the first block that stops the mouse sideways inside its box, were its
 * feet at (x, y, z) (a shelf does not); W3_AIR for none. *hx..*hz: where. */
static uint8_t box_hits(int32_t x, int32_t y, int32_t z, int *hx, int *hy, int *hz) {
	int tx0 = (x - R_MOUSE) >> 12, tx1 = (x + R_MOUSE - 1) >> 12;
	int tz0 = (z - R_MOUSE) >> 12, tz1 = (z + R_MOUSE - 1) >> 12;
	int ty0 = (y + Q) >> 12, ty1 = (y + H_MOUSE - 1) >> 12;
	int tx, ty, tz;
	for (ty = ty1; ty >= ty0; ty--)
		for (tz = tz0; tz <= tz1; tz++)
			for (tx = tx0; tx <= tx1; tx++) {
				uint8_t k = w3_block(world, tx, ty, tz);
				if (k != W3_AIR && k != W3_SHELF) {
					if (hx) { *hx = tx; *hy = ty; *hz = tz; }
					return k;
				}
			}
	return W3_AIR;
}

/* the highest top at or under `from` and at or over y (Q8) inside the
 * mouse's box, or INT32_MIN; *kind: the block it is the top of */
static int32_t floor_under(int32_t x, int32_t y, int32_t z, int32_t from, uint8_t *kind, int *fx, int *fy, int *fz) {
	int tx0 = (x - R_MOUSE) >> 12, tx1 = (x + R_MOUSE - 1) >> 12;
	int tz0 = (z - R_MOUSE) >> 12, tz1 = (z + R_MOUSE - 1) >> 12;
	int tx, tz, ty;
	int32_t best = INT32_MIN;
	for (tz = tz0; tz <= tz1; tz++)
		for (tx = tx0; tx <= tx1; tx++)
			for (ty = (from >> 12); ty >= (y >> 12) - 1 && ty >= -1; ty--) {
				uint8_t k = w3_block(world, tx, ty, tz);
				int32_t top = (ty + 1) << 12;
				if (k == W3_AIR) continue;
				/* a shelf only from above: its top under where the feet were */
				if (k == W3_SHELF && top > from) continue;
				if (top <= from && top >= y && top > best) {
					best = top;
					if (kind) { *kind = k; *fx = tx; *fy = ty; *fz = tz; }
				}
				break;
			}
	return best;
}

static int32_t water_at(int32_t x, int32_t z) {
	int w = w3_water(world, x >> 12, z >> 12);
	return w < 0 ? INT32_MIN : (w * W3_T - 3) * Q;
}

static void hurt(int32_t from_x, int32_t from_z) {
	int32_t dx = mx - from_x, dz = mz - from_z, d;
	if (blink || power[P_BELL] || gstate != G_PLAY) return;
	if (bubble) {
		bubble = false;
		blink = BLINK / 2;
		sound(SND_POP);
		return;
	}
	hearts--;
	blink = BLINK;
	sound(SND_OUCH);
	/* knocked back, away from what hit it */
	d = (dx < 0 ? -dx : dx) + (dz < 0 ? -dz : dz);
	if (d > 0) {
		vx = (int32_t)((int64_t)dx * 3 * Q / d);
		vz = (int32_t)((int64_t)dz * 3 * Q / d);
	}
	vy = JUMP_V / 2;
	grounded = false;
	if (hearts <= 0) { gstate = G_OVER; gtimer = 0; }
}

/* a ? box bumped from below: it is a box now, and out of it pops a
 * power-up -- a heart when the mouse is short of hearts */
static void bump(int tx, int ty, int tz) {
	static const uint8_t pick[5] = { A_FEATHER, A_BUBBLE, A_MAGNET, A_BELL, A_SHOES };
	actor_t *a;
	int kind = hearts < HEARTS ? A_HEART : pick[hash(tx, tz) % 5];
	change_block(tx, ty, tz, W3_BOX, 0);
	a = add_actor(kind, tx * TQ + TQ / 2, (ty + 1) * TQ, tz * TQ + TQ / 2);
	if (a) a->vy = 3 * Q;
	sound(SND_BUMP);
}

static void step_mouse(uint32_t held, bool jump) {
	int32_t want_x = 0, want_z = 0, sp;
	int32_t fx = zglSin(cam_yaw), fz = -zglCos(cam_yaw);    /* the camera's forward, 16.16 */
	int fwd = 0, side = 0, hx, hy, hz;
	int32_t wsurf = water_at(mx, mz);
	uint8_t k;
	swimming = wsurf != INT32_MIN && my < wsurf - 2 * Q;
	if (swimming && !was_swimming) sound(SND_SPLASH);
	was_swimming = swimming;
	if (held & AR_BTN_UP) fwd++;
	if (held & AR_BTN_DOWN) fwd--;
	if (held & AR_BTN_RIGHT) side++;
	if (held & AR_BTN_LEFT) side--;
	sp = swimming ? SWIM_SPEED : SPEED;
	if (power[P_SHOES]) sp = sp * 3 / 2;
	if (fwd || side) {
		/* along the camera: forward, and right (-fz, fx) */
		int32_t dx = fwd * fx + side * -fz, dz = fwd * fz + side * fx;
		if (fwd && side) {                     /* 1/sqrt 2 */
			dx = (int32_t)(((int64_t)dx * 46341) >> 16);
			dz = (int32_t)(((int64_t)dz * 46341) >> 16);
		}
		want_x = (int32_t)(((int64_t)dx * sp) >> 16);
		want_z = (int32_t)(((int64_t)dz * sp) >> 16);
		/* face the way it runs, turning quickly */
		{
			uint32_t to = angle_of(dx, -dz);
			int32_t d = (int16_t)(to - heading);
			heading += (uint32_t)(d / 4);
		}
	}
	/* a little momentum: a quarter of the way a tick */
	vx += (want_x - vx) / 4;
	vz += (want_z - vz) / 4;

	if (jump) {
		if (grounded) { vy = JUMP_V; grounded = false; air_jump = power[P_FEATHER] != 0; sound(SND_JUMP); }
		else if (swimming) vy = JUMP_V * 45 / 100;      /* a stroke */
		else if (air_jump) { vy = JUMP_V; air_jump = false; sound(SND_JUMP); }   /* the feather */
	}
	vy -= swimming ? GRAVITY / 4 : GRAVITY;
	if (vy < (swimming ? -Q : -FALL_MAX)) vy = swimming ? -Q : -FALL_MAX;

	/* sideways, an axis at a time: a wall stops that axis only; thorns'
	 * points hurt (their top is clear of them) */
	if (vx) {
		k = box_hits(mx + vx, my, mz, &hx, &hy, &hz);
		if (!k) mx += vx;
		else { if (k == W3_THORNS) hurt(hx * TQ + TQ / 2, hz * TQ + TQ / 2); vx = 0; }
	}
	if (vz) {
		k = box_hits(mx, my, mz + vz, &hx, &hy, &hz);
		if (!k) mz += vz;
		else { if (k == W3_THORNS) hurt(hx * TQ + TQ / 2, hz * TQ + TQ / 2); vz = 0; }
	}

	/* up and down */
	if (vy > 0) {
		int32_t ny = my + vy;
		k = box_hits(mx, ny, mz, &hx, &hy, &hz);
		if (k) {                                /* a head under a block */
			if (k == W3_QBOX) bump(hx, hy, hz);
			vy = 0;
		} else my = ny;
		grounded = false;
	} else {
		int32_t ny = my + vy;
		uint8_t kind = W3_AIR;
		int fx2 = 0, fy2 = 0, fz2 = 0;
		int32_t fl = floor_under(mx, ny, mz, my, &kind, &fx2, &fy2, &fz2);
		if (fl != INT32_MIN && fl >= ny) {
			bool landed = !grounded;
			my = fl;
			vy = 0;
			grounded = true;
			if (kind == W3_SPRING) {
				vy = SPRING_V;
				grounded = false;
				sound(SND_SPRING);
			} else if (kind == W3_BISCUIT) {
				int i;
				/* it cracks under the mouse: a second, then crumbles */
				for (i = 0; i < nchg; i++)
					if (chg[i].x == fx2 && chg[i].y == fy2 && chg[i].z == fz2) break;
				if (i == nchg && nchg < MAX_CHANGES) {
					chg[i].x = (int16_t)fx2; chg[i].y = (uint8_t)fy2; chg[i].z = (int16_t)fz2;
					chg[i].was = W3_BISCUIT;
					chg[i].back = -60;           /* negative: crumbling */
					nchg++;
				}
			} else if (landed && kind == W3_THORNS) {
				/* their top is clear of the points */
			}
			if (landed) {
				int i;
				/* landing wakes a sleeping cat near it */
				for (i = 0; i < nact; i++) {
					actor_t *a = &act[i];
					int32_t dx = a->x - mx, dz = a->z - mz;
					if (a->kind != A_SLEEPER || a->state != S_SLEEP) continue;
					if (dx > -3 * TQ && dx < 3 * TQ && dz > -3 * TQ && dz < 3 * TQ) {
						a->state = S_CHASE;
						a->timer = 600;
						sound(SND_WAKE);
					}
				}
			}
		} else {
			my = ny;
			grounded = false;
		}
	}
	if (grounded || swimming) air_jump = power[P_FEATHER] != 0;
	/* out of the world: a pit -- a heart, and back to the last hole */
	if (my < -3 * TQ) {
		sound(SND_FALL);
		mx = check_x; my = check_y; mz = check_z;
		vx = vy = vz = 0;
		if (!blink && !power[P_BELL]) {
			hearts--;
			if (hearts <= 0) { gstate = G_OVER; gtimer = 0; }
		}
		blink = BLINK;
	}
	if (blink) blink--;
	/* a running stride, two frames */
	if (grounded && (vx > Q / 2 || vx < -Q / 2 || vz > Q / 2 || vz < -Q / 2)) {
		if (ticks % 6 == 0) stride ^= 1;
	} else stride = 0;
}

/* biscuits crumbling, and baked again */
static void step_blocks(void) {
	int i;
	for (i = 0; i < nchg; i++) {
		change_t *c = &chg[i];
		if (c->back < 0) {
			if (++c->back == 0) {
				w3_set_block(world, c->x, c->y, c->z, W3_AIR);
				c->back = 300;                 /* five seconds */
				sound(SND_CRUMBLE);
			}
		} else if (c->back > 0 && c->was == W3_BISCUIT) {
			if (--c->back == 0) {
				/* not into the mouse */
				int32_t bx = c->x * TQ, by = c->y * TQ, bz = c->z * TQ;
				if (mx + R_MOUSE > bx && mx - R_MOUSE < bx + TQ && mz + R_MOUSE > bz &&
				    mz - R_MOUSE < bz + TQ && my < by + TQ && my + H_MOUSE > by)
					c->back = 30;
				else
					w3_set_block(world, c->x, c->y, c->z, W3_BISCUIT);
			}
		}
	}
}

/* -- everyone else ---------------------------------------------------------------- */

static bool near_mouse(const actor_t *a, int32_t r, int32_t h) {
	int32_t dx = a->x - mx, dz = a->z - mz, dy = a->y - my;
	return dx > -r && dx < r && dz > -r && dz < r && dy > -h && dy < h;
}

static void take(actor_t *a) {
	switch (a->kind) {
	case A_CHEESE: cheese_got++; sound(SND_COIN); break;
	case A_STAR:   star_got = true; sound(SND_POWER); break;
	case A_HEART:  if (hearts < HEARTS_MAX) hearts++; sound(SND_HEART); break;
	case A_FEATHER: power[P_FEATHER] = POWER_TICKS; air_jump = true; sound(SND_POWER); break;
	case A_MAGNET: power[P_MAGNET] = POWER_TICKS; sound(SND_POWER); break;
	case A_BELL:   power[P_BELL] = POWER_TICKS; sound(SND_POWER); break;
	case A_SHOES:  power[P_SHOES] = POWER_TICKS; sound(SND_POWER); break;
	case A_BUBBLE: bubble = true; sound(SND_POWER); break;
	default: return;
	}
	a->alive = false;
}

/* a foe meets the mouse: from above it is dizzy; else a heart */
static void meet(actor_t *a, int32_t top) {
	if (!near_mouse(a, 10 * Q, top + H_MOUSE)) return;
	if (my + H_MOUSE < a->y) return;
	if (vy < 0 && my > a->y + top / 2 && a->state != S_SLEEP) {
		a->state = S_DIZZY;
		a->timer = 90;
		vy = BOUNCE_V;
		grounded = false;
		sound(SND_STOMP);
		return;
	}
	if (a->state == S_WALK || a->state == S_CHASE) hurt(a->x, a->z);
}

static void walk(actor_t *a, int32_t speed) {
	int32_t *c = a->ax ? &a->x : &a->z;
	int32_t step = speed * a->way;
	/* the bell: everyone runs from the mouse */
	if (power[P_BELL]) {
		int32_t d = a->ax ? a->x - mx : a->z - mz;
		a->way = (int8_t)(d >= 0 ? 1 : -1);
		step = speed * 3 / 2 * a->way;
	}
	*c += step;
	if (*c > a->hi) { *c = a->hi; a->way = -1; }
	if (*c < a->lo) { *c = a->lo; a->way = 1; }
	a->dir = a->ax ? (a->way > 0 ? 16384u : 49152u) : (a->way > 0 ? 32768u : 0u);
}

static void step_actors(void) {
	int i;
	for (i = 0; i < nact; i++) {
		actor_t *a = &act[i];
		if (!a->alive) continue;
		switch (a->kind) {
		case A_CAT:
		case A_SLEEPER:
			if (a->state == S_DIZZY) {
				if (--a->timer <= 0) { a->state = S_NAP; a->timer = 600; }
			} else if (a->state == S_NAP) {
				if (--a->timer <= 0) a->state = S_WALK;
			} else if (a->state == S_CHASE) {
				/* a woken cat comes for the mouse, along its beat */
				int32_t d = a->ax ? mx - a->x : mz - a->z;
				a->way = (int8_t)(d >= 0 ? 1 : -1);
				walk(a, Q * 3 / 4);
				if (--a->timer <= 0) a->state = S_WALK;
			} else if (a->state == S_WALK) walk(a, Q / 2);
			meet(a, 14 * Q);
			break;
		case A_FROG:
			/* sits, then hops along its beat, croaking */
			if (a->state == S_WALK) {
				if (a->vy == 0) {
					if (++a->timer >= 90) { a->vy = 3 * Q; a->timer = 0; sound(SND_CROAK); }
				} else {
					walk(a, Q);
					a->vy -= GRAVITY;
					if (a->vy <= -3 * Q) a->vy = 0;
				}
			} else if (a->state == S_DIZZY) {
				if (--a->timer <= 0) { a->state = S_NAP; a->timer = 600; }
			} else if (a->state == S_NAP) {
				if (--a->timer <= 0) a->state = S_WALK;
			}
			meet(a, 10 * Q);
			break;
		case A_OWL:
			if (a->state == S_WALK) walk(a, Q * 2 / 3);
			else if (a->state == S_DIZZY) { if (--a->timer <= 0) { a->state = S_NAP; a->timer = 600; } }
			else if (a->state == S_NAP) { if (--a->timer <= 0) a->state = S_WALK; }
			meet(a, 14 * Q);
			break;
		case A_HOLE:
			if (!a->lit && near_mouse(a, 12 * Q, 16 * Q)) {
				a->lit = true;
				check_x = a->x; check_y = a->y; check_z = a->z;
				sound(SND_CHECK);
			}
			break;
		case A_WHEEL:
			if (gstate == G_PLAY && near_mouse(a, 16 * Q, 16 * Q)) {
				gstate = G_DONE;
				gtimer = 0;
				sound(SND_GOAL);
			}
			break;
		case A_CHEESE:
			if (power[P_MAGNET] && near_mouse(a, 4 * TQ, 4 * TQ)) {
				a->x += (mx - a->x) / 8;
				a->z += (mz - a->z) / 8;
				a->y += (my - a->y) / 8;
			}
			if (near_mouse(a, 10 * Q, 14 * Q)) take(a);
			break;
		default:                                 /* the star, power-ups */
			if (a->vy) {                         /* out of a ? box */
				a->y += a->vy;
				a->vy -= Q / 4;
				if (a->vy <= 0) a->vy = 0;
			}
			if (near_mouse(a, 10 * Q, 14 * Q)) take(a);
			break;
		}
	}
	for (i = 0; i < P_N; i++) if (power[i]) power[i]--;
}

/* -- the camera --------------------------------------------------------------- */

static bool solid_at(int32_t x, int32_t y, int32_t z) {     /* world units */
	return w3_block(world, x >> 4, y >> 4, z >> 4) != W3_AIR;
}

static void step_camera(uint32_t held1, bool recentre, bool moving) {
	int32_t tx, ty, tz, fx, fz, dx, dy, dz;
	int k, n;
	if (held1 & AR_BTN_A) cam_yaw -= 300;         /* Q */
	if (held1 & AR_BTN_B) cam_yaw += 300;         /* E */
	if (recentre) cam_yaw = heading;
	else if (moving && !(held1 & (AR_BTN_A | AR_BTN_B))) {
		/* swing round behind it as it runs, unless it runs at the camera */
		int32_t d = (int16_t)(heading - cam_yaw);
		if (d > -24000 && d < 24000) cam_yaw += (uint32_t)(d / 40);
	}
	/* behind and above the point looked at, pulled in where a block is
	 * in the way (a step along the line at a time) */
	tx = mx / Q; ty = my / Q + CAM_LIFT; tz = mz / Q;
	fx = zglSin(cam_yaw); fz = -zglCos(cam_yaw);
	dx = (int32_t)(((int64_t)-fx * CAM_DIST) >> 16);
	dz = (int32_t)(((int64_t)-fz * CAM_DIST) >> 16);
	dy = CAM_DIST * 29 / 100;                    /* tan 16 degrees */
	n = 16;
	for (k = 1; k <= n; k++)
		if (solid_at(tx + dx * k / n, ty + dy * k / n, tz + dz * k / n)) break;
	k -= 1;
	if (k < 2) k = 2;
	cam_x = tx + dx * k / n;
	cam_y = ty + dy * k / n;
	cam_z = tz + dz * k / n;
}

/* -- drawing ------------------------------------------------------------------ */

static void view_2d(void) {
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrthox(0, 320 << 16, 240 << 16, 0, -65536, 65536);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}

static void view_3d(void) {
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	/* far past the draw distance, so no triangle is clipped by it */
	zglFrustumi(-11, 11, -8, 8, NEAR, FAR + 2 * W3_CELL * W3_T);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glRotatex(PITCH_DEG << 16, 65536, 0, 0);
	glRotatex((GLfixed)(cam_yaw * 360u), 0, 65536, 0);
	zglTranslatei(-cam_x, -cam_y, -cam_z);
}

/* The sky and the HUD are display lists too: drawn by the CPU's own 3D
 * path (immediate mode) a triangle or a line costs it tens of thousands
 * of cycles -- the set-up's 64-bit divides -- and T2 does that work for
 * a list. The sky's never changes; the HUD's is made again when what it
 * shows does (docs/mausrun3d.md, "Frame budget"). */
static GLuint l_sky, l_hud;

/* textures (docs/mausrun3d.md, "Textures"): T2 has them (made at
 * open_chunky()), and T turns them off and on */
static bool tex_have, tex_want = true;

/* -- the map: the world from above, a 32x32 texture (each texel its
 * ramp and tone, ZGL_TEX_RAMP; chasms not drawn), one quad at the
 * bottom right, the mouse a pointer on it. Two triangles and one more,
 * where lines for the world's edges would be dozens. Without textures,
 * no map. -- */

#define MAP_TEX  60                  /* a texture name the world's do not use */
#define MAP_X    250
#define MAP_Y    170
#define MAP_PX   64
static bool map_ok;
static GLuint l_map;                 /* the quad: textures go in lists */

static void map_make(void) {
	static uint8_t t[32 * 32];
	int u, v;
	for (v = 0; v < 32; v++)
		for (u = 0; u < 32; u++) {
			int x = (u * 2 + 1) * world->n / 64, z = (v * 2 + 1) * world->n / 64, y;
			uint8_t g = world->ground[z * world->n + x], k = W3_AIR, c;
			int top = g == W3_NONE ? -1 : g, tone;
			char sf = (char)world->surf[z * world->n + x];
			for (y = W3_H - 1; y > top; y--)
				if ((k = w3_block(world, x, y, z)) != W3_AIR) break;
			if (y > top) top = y;
			tone = 7 + top * 8 / W3_H;
			if (tone > 15) tone = 15;
			if (w3_water(world, x, z) >= 0 && w3_water(world, x, z) >= top) c = W3_R_WATER | 11;
			else if (g == W3_NONE && k == W3_AIR) c = 0;                 /* a chasm: not drawn */
			else if (k == W3_THORNS) c = (uint8_t)(W3_R_RED | 12);
			else if (k == W3_BOX || k == W3_QBOX || k == W3_SHELF) c = (uint8_t)(W3_R_BOX | tone);
			else if (k != W3_AIR) c = (uint8_t)(W3_R_ROCK | tone);
			else c = (uint8_t)((sf == 'p' ? W3_R_PATH : sf == 's' ? W3_R_SAND : sf == 'r' ? W3_R_ROCK :
				sf == 'd' ? W3_R_EARTH : W3_R_GRASS) | tone);
			t[v * 32 + u] = c;
		}
	map_ok = zglTexImage(MAP_TEX, 5, 5, t, ZGL_TEX_RAMP, true);
	if (!map_ok) return;
	if (!l_map) l_map = glGenLists(1);
	glNewList(l_map, GL_COMPILE);
	glBindTexture(GL_TEXTURE_2D, MAP_TEX);
	glIndexi(15);                        /* a tone's full light */
	glBegin(GL_TRIANGLES);
	glTexCoord2i(0, 0); glVertex2i(MAP_X, MAP_Y);
	glTexCoord2i(0, 32); glVertex2i(MAP_X, MAP_Y + MAP_PX);
	glTexCoord2i(32, 32); glVertex2i(MAP_X + MAP_PX, MAP_Y + MAP_PX);
	glTexCoord2i(0, 0); glVertex2i(MAP_X, MAP_Y);
	glTexCoord2i(32, 32); glVertex2i(MAP_X + MAP_PX, MAP_Y + MAP_PX);
	glTexCoord2i(32, 0); glVertex2i(MAP_X + MAP_PX, MAP_Y);
	glEnd();
	glBindTexture(GL_TEXTURE_2D, 0);
	glEndList();
}

static void map_draw(void) {
	int32_t px, pz, s, c;
	if (!map_ok) return;
	glEnable(GL_TEXTURE_2D);
	glCallList(l_map);
	glDisable(GL_TEXTURE_2D);
	/* the mouse: a pointer the way it faces */
	px = MAP_X + (mx / 256) * MAP_PX / (world->n * W3_T);
	pz = MAP_Y + (mz / 256) * MAP_PX / (world->n * W3_T);
	s = zglSin(heading) * 4 >> 16;
	c = zglCos(heading) * 4 >> 16;
	glIndexi(W3_C_WHITE);
	glBegin(GL_TRIANGLES);
	glVertex2i(px + s, pz - c);
	glVertex2i(px - s / 2 - c / 2, pz + c / 2 - s / 2);
	glVertex2i(px - s / 2 + c / 2, pz + c / 2 + s / 2);
	glEnd();
}

/* W: wait for each frame at its end (glFinish), as zgl did before it
 * stopped waiting for T2 -- to compare the two on the board */
static bool frame_wait;

/* L: every frame two refreshes (30 fps, even), with W's waiting: a
 * frame rate that is not a whole fraction of the display's 60 shows
 * frames for 1, 2, 1, 2 refreshes, and motion steps unevenly */
static bool frame_lock;

/* input in the report: presses and releases, and how often what is held
 * changed -- a key held down should be one press, not a stream */
static uint32_t in_down, in_up, in_flips, in_last;

/* the sky above the horizon, a gradient; the haze under it */
static void sky_content(void) {
	int hy = 120 - FOCAL * 29 / 100;   /* the horizon: tan 16 degrees */
	glBegin(GL_TRIANGLE_STRIP);
	glIndexi(W3_R_SKY + 15); glVertex2i(0, 0);
	glIndexi(W3_R_SKY + 15); glVertex2i(320, 0);
	glIndexi(W3_R_SKY); glVertex2i(0, hy);
	glIndexi(W3_R_SKY); glVertex2i(320, hy);
	glEnd();
	glBegin(GL_TRIANGLE_STRIP);
	glIndexi(W3_R_WATER + 9); glVertex2i(0, hy);
	glIndexi(W3_R_WATER + 9); glVertex2i(320, hy);
	glIndexi(W3_R_WATER + 6); glVertex2i(0, 240);
	glIndexi(W3_R_WATER + 6); glVertex2i(320, 240);
	glEnd();
}

static void draw_sky(void) {
	view_2d();
	glShadeModel(GL_SMOOTH);
	glCallList(l_sky);
	glShadeModel(GL_FLAT);
}

static void hud_content(void) {
	char b[32];
	int i, x;
	glIndexi(4);
	glBegin(GL_TRIANGLES);               /* a cheese, for the count */
	glVertex2i(6, 12); glVertex2i(16, 12); glVertex2i(6, 4);
	/* the hearts */
	glIndexi(5);
	for (i = 0; i < hearts; i++) {
		x = 300 - i * 12;
		glVertex2i(x, 6); glVertex2i(x + 8, 6); glVertex2i(x + 4, 12);
		glVertex2i(x, 6); glVertex2i(x + 2, 3); glVertex2i(x + 4, 6);
		glVertex2i(x + 4, 6); glVertex2i(x + 6, 3); glVertex2i(x + 8, 6);
	}
	glEnd();
	snprintf(b, sizeof(b), "%d/%d", cheese_got, cheese_all);
	vf_text(22, 5, 1, W3_C_WHITE, b, false);
	if (star_got) vf_text(76, 5, 1, 4, "*", false);
	/* the power-ups in hand, a bar each for their time */
	{
		static const char *const name[P_N] = { "FEATHER", "MAGNET", "BELL", "SHOES" };
		int y = 18;
		for (i = 0; i < P_N; i++) {
			if (!power[i]) continue;
			if (power[i] > 180 || (power[i] / 8) & 1) {
				vf_text(6, y, 1, W3_C_HUD, name[i], false);
				glIndexi(W3_C_HUD);
				glBegin(GL_LINES);
				glVertex2i(52, y + 3); glVertex2i(52 + power[i] * 40 / POWER_TICKS, y + 3);
				glEnd();
			}
			y += 9;
		}
		if (bubble) vf_text(6, y, 1, W3_R_WATER + 15, "BUBBLE", false);
	}
	/* what the screen says */
	if (gstate == G_CARD) {
		vf_centred(80, 3, W3_C_WHITE, "LEVEL 1", true);
		vf_centred(110, 2, 4, world->name, false);
	} else if (gstate == G_DONE) {
		unsigned s = run_ticks / 60;
		int stars = 1 + (cheese_got == cheese_all) + star_got;
		vf_centred(60, 3, 4, "WELL DONE!", true);
		snprintf(b, sizeof(b), "CHEESE %d/%d", cheese_got, cheese_all);
		vf_centred(100, 2, W3_C_WHITE, b, false);
		snprintf(b, sizeof(b), "STARS %d/3", stars);
		vf_centred(120, 2, W3_C_WHITE, b, false);
		snprintf(b, sizeof(b), "TIME %u:%02u", s / 60, s % 60);
		vf_centred(140, 2, W3_C_WHITE, b, false);
		if (gtimer > 120) vf_centred(180, 1, W3_C_HUD, "SPACE: AGAIN", false);
	} else if (gstate == G_OVER) {
		vf_centred(90, 3, 5, "GAME OVER", true);
		if (gtimer > 60) vf_centred(130, 1, W3_C_WHITE, "SPACE: THE SAME LEVEL AGAIN", false);
	}
}

/* what the HUD shows, all of it: the list is made again when it changes */
static int hud_key(int *k) {
	int n = 0, i;
	k[n++] = cheese_got; k[n++] = cheese_all; k[n++] = hearts; k[n++] = star_got;
	k[n++] = bubble; k[n++] = gstate;
	for (i = 0; i < P_N; i++) {
		bool on = power[i] && (power[i] > 180 || ((power[i] / 8) & 1));
		k[n++] = on ? 1 + power[i] * 40 / POWER_TICKS : 0;
	}
	k[n++] = gstate == G_DONE ? (int)(run_ticks / 60) * 2 + (gtimer > 120) : 0;
	k[n++] = gstate == G_OVER ? gtimer > 60 : 0;
	return n;
}

static void draw_hud(void) {
	static int key[16], nkey = -1;
	int k[16], n = hud_key(k);
	view_2d();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LIGHTING);
	glDisable(GL_CULL_FACE);
	if (n != nkey || memcmp(k, key, (size_t)n * sizeof(k[0]))) {
		memcpy(key, k, sizeof(k));
		nkey = n;
		glNewList(l_hud, GL_COMPILE);
		hud_content();
		glEndList();
	}
	glCallList(l_hud);
	if (gstate != G_CARD) map_draw();
}

static uint32_t tris_sent;

/* where a frame's CPU cycles go (the report every two seconds) */
enum { T_SKY, T_WORLD, T_THINGS, T_HUD, T_FRAME, T_GAME, T_WAIT, T_N };
static uint32_t tsum[T_N], isum[T_N];   /* cycles; instructions */
static uint32_t calls_sum;
static inline uint32_t cycles_now(void);
static inline uint32_t instret_now(void);

/* The draw distance follows what a frame costs (docs/mausrun3d.md,
 * "Frame budget"). T2 draws a frame's records only from the start of
 * vertical blanking (CTRL.VSYNC) and takes the next frame's geometry
 * only once they are drawn, so its geometry has what is left of a
 * refresh after the raster's few milliseconds: a frame that missed it
 * (ar_frame() said two refreshes went by) draws the next one a tile
 * shorter; after two seconds that all made it, a little longer. And as a
 * ceiling, the triangles a frame sends. The haze under the horizon hides
 * where the world stops. */
#define FAR_MIN   144
#define TRIS_HIGH 340
static int32_t far_now = FAR;
static int on_time;                  /* frames in a row that made the refresh */

static void pace(uint32_t dt) {
	if (dt > 1) {
		if (far_now > FAR_MIN) far_now -= 16;
		on_time = 0;
	} else if (++on_time >= 120 && far_now < FAR) {
		far_now += 8;
		on_time = 60;
	}
}

/* is (x, z) world units in front of the camera and within the draw
 * distance? (cheap: the view cone's sides are left to T2) */
static bool in_view(int32_t x, int32_t z) {
	int32_t dx = x - cam_x, dz = z - cam_z;
	int32_t along = (int32_t)(((int64_t)dx * zglSin(cam_yaw) - (int64_t)dz * zglCos(cam_yaw)) >> 16);
	return along > -24 && dx * dx + dz * dz < far_now * far_now;
}

static void draw_model(int m, int32_t x, int32_t y, int32_t z, uint32_t dir) {
	if (!in_view(x / Q, z / Q)) return;
	glPushMatrix();
	glTranslatex(x * 256, y * 256, z * 256);
	if (dir) glRotatex((GLfixed)((65536u - dir) * 360u), 0, 65536, 0);
	glCallList(md_list + (GLuint)m);
	glPopMatrix();
	tris_sent += md_tris[m];
}

static void draw_actors(void) {
	int i;
	uint32_t spin = ticks * 400u;
	int32_t bob = (int32_t)((zglSin(ticks * 600u) * 2) >> 16) * Q;
	for (i = 0; i < nact; i++) {
		const actor_t *a = &act[i];
		int m = -1;
		uint32_t dir = a->dir;
		int32_t y = a->y;
		if (!a->alive) continue;
		switch (a->kind) {
		case A_CHEESE:  m = MD_CHEESE; dir = spin; y += 2 * Q + bob; break;
		case A_CAT:
		case A_SLEEPER:
			m = (a->state == S_NAP || a->state == S_SLEEP) ? MD_CAT_NAP :
			    (a->state == S_DIZZY) ? MD_CAT : ((ticks / 8) & 1) ? MD_CAT_STEP : MD_CAT;
			break;
		case A_FROG:
			m = a->vy ? MD_FROG_HOP : MD_FROG;
			/* up the hop's parabola: (v0^2 - v^2) / 2g */
			if (a->vy) y += (int32_t)(((int64_t)9 * Q * Q - (int64_t)a->vy * a->vy) / (2 * GRAVITY));
			break;
		case A_OWL:     m = MD_OWL; y += bob * 2; break;
		case A_HOLE:    m = a->lit ? MD_HOLE_LIT : MD_HOLE; dir = cam_yaw + 32768u; break;   /* its door to the camera */
		case A_WHEEL:   m = MD_WHEEL; dir = ticks * 120u; break;
		case A_STAR:    m = MD_STAR; dir = spin; y += 4 * Q + bob; break;
		case A_HEART:   m = MD_HEART; dir = spin; y += bob; break;
		case A_FEATHER: m = MD_FEATHER; dir = spin; y += bob; break;
		case A_BUBBLE:  m = MD_BUBBLE; dir = spin; y += bob; break;
		case A_MAGNET:  m = MD_MAGNET; dir = spin; y += bob; break;
		case A_BELL:    m = MD_BELL; dir = spin; y += bob; break;
		case A_SHOES:   m = MD_SHOES; dir = spin; y += bob; break;
		}
		if (m < 0) continue;
		draw_model(m, a->x, y, a->z, dir);
		if (a->state == S_DIZZY && (a->kind == A_CAT || a->kind == A_SLEEPER || a->kind == A_FROG || a->kind == A_OWL))
			draw_model(MD_DIZZY, a->x, a->y + 20 * Q, a->z, spin * 2);
	}
}

static void render(void) {
	static const GLfixed light[4] = { -29491, 52428, 26214, 0 };
	static const GLfixed white[4] = { 65536, 65536, 65536, 65536 };
	int32_t gy;

	uint32_t t0 = cycles_now(), t1, i0 = instret_now(), i1;

	glClearIndex(W3_R_SKY);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_LIGHTING);
	glDisable(GL_CULL_FACE);
	draw_sky();
	t1 = cycles_now(); tsum[T_SKY] += t1 - t0; t0 = t1;
	i1 = instret_now(); isum[T_SKY] += i1 - i0; i0 = i1;

	view_3d();
	glLightxv(GL_LIGHT0, GL_POSITION, light);
	glLightxv(GL_LIGHT0, GL_DIFFUSE, white);
	glEnable(GL_LIGHT0);
	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glEnable(GL_LIGHTING);
	if (tex_have && tex_want) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
	tris_sent = w3_draw(world, cam_x, cam_y, cam_z, cam_yaw, (uint32_t)PITCH_DEG * 65536u / 360u,
		11 * 65536 / 16, 8 * 65536 / 16, far_now, 2);
	calls_sum += w3_calls;
	t1 = cycles_now(); tsum[T_WORLD] += t1 - t0; t0 = t1;
	i1 = instret_now(); isum[T_WORLD] += i1 - i0; i0 = i1;

	draw_actors();

	/* the mouse, blinking while nothing can hurt it */
	if (!blink || (blink / 4) % 2 == 0)
		draw_model(stride ? MD_MOUSE_STEP : MD_MOUSE, mx, my, mz, heading);

	/* its shadow, on what is under it */
	gy = floor_under(mx, -TQ, mz, my + Q, NULL, NULL, NULL, NULL);
	if (gy != INT32_MIN) {
		glDisable(GL_LIGHTING);
		draw_model(MD_SHADOW, mx, gy + Q / 2, mz, 0);
	}

	if (tris_sent > TRIS_HIGH && far_now > FAR_MIN) far_now -= 16;

	t1 = cycles_now(); tsum[T_THINGS] += t1 - t0; t0 = t1;
	i1 = instret_now(); isum[T_THINGS] += i1 - i0; i0 = i1;
	draw_hud();
	t1 = cycles_now(); tsum[T_HUD] += t1 - t0; t0 = t1;
	i1 = instret_now(); isum[T_HUD] += i1 - i0; i0 = i1;
	zgl_frame();
	t1 = cycles_now(); tsum[T_FRAME] += t1 - t0;
	i1 = instret_now(); isum[T_FRAME] += i1 - i0;
}

/* -- game mode ----------------------------------------------------------------- */

static bool open_chunky(void) {
	zgl_desc_t zd = { ZGL_FRAMES, 0, 0, 1024, ZGL_CHUNKY };
	uint16_t pal[256];
	if (!zgl_open(&zd)) {
		printf("mausrun3d: no memory for zgl\n");
		return false;
	}
	w3_palette(world, pal);
	ar_pal_load(pal, 256);
	/* before any of the world's lists is first drawn: each keeps the
	 * texture places it was compiled with */
	tex_have = w3_textures();
	map_make();
	printf("mausrun3d: game mode, chunky: %s\n",
		zgl_info()->backend == ZGL_BACKEND_T2 ? "T2" : "software (slow: T2 wanted)");
	if (!md_list) {
		md_make();
		l_sky = glGenLists(2);
		l_hud = l_sky + 1;
		glNewList(l_sky, GL_COMPILE);
		sky_content();
		glEndList();
	}
	return true;
}

static void mode_changed(void) {
	bool want = ar_mode() == AR_MODE_FULL_CHUNKY;
	if (want == chunky) return;
	if (want) chunky = open_chunky();
	else {
		zgl_close();
		chunky = false;
		ar_pal_load(desc.palette, 16);
		ar_dirty_all();
	}
}

#ifdef AR_HOST_TESTS
/* -- for tests/render.c: where things are, and the mouse put somewhere -- */
static const uint8_t test_kind[128] = {
	['c'] = A_CHEESE + 1, ['C'] = A_CAT + 1, ['Z'] = A_SLEEPER + 1, ['F'] = A_FROG + 1,
	['O'] = A_OWL + 1, ['H'] = A_HOLE + 1, ['G'] = A_WHEEL + 1, ['*'] = A_STAR + 1,
};

/* the nth (from 0) alive one of a kind (a map letter): true and where */
bool mr3_test_find(char c, int nth, int32_t *x, int32_t *y, int32_t *z) {
	int i;
	for (i = 0; i < nact; i++) {
		if (!act[i].alive || act[i].kind + 1 != test_kind[(uint8_t)c & 127]) continue;
		if (nth-- > 0) continue;
		*x = act[i].x; *y = act[i].y; *z = act[i].z;
		return true;
	}
	return false;
}

/* a ? box's tile, the nth in the world */
bool mr3_test_qbox(int nth, int *tx, int *ty, int *tz) {
	int x, y, z;
	for (z = 0; z < world->n; z++)
		for (x = 0; x < world->n; x++)
			for (y = 0; y < W3_H; y++)
				if (w3_block(world, x, y, z) == W3_QBOX && nth-- == 0) {
					*tx = x; *ty = y; *tz = z;
					return true;
				}
	return false;
}

void mr3_test_place(int32_t x, int32_t y, int32_t z, uint32_t face) {
	mx = x; my = y; mz = z;
	vx = vy = vz = 0;
	grounded = false;
	heading = cam_yaw = face;
}

void mr3_test_status(int *h, int *ch, int *gs, int *st, int *pw) {
	int i;
	*h = hearts; *ch = cheese_got; *gs = gstate; *st = star_got;
	*pw = bubble;
	for (i = 0; i < P_N; i++) if (power[i]) *pw |= 2 << i;
}

void mr3_test_hearts(int h) { hearts = h; }

uint8_t mr3_test_block(int x, int y, int z) { return w3_block(world, x, y, z); }

/* a chasm's tile, the nth */
bool mr3_test_chasm(int nth, int *tx, int *tz) {
	int x, z;
	for (z = 0; z < world->n; z++)
		for (x = 0; x < world->n; x++)
			if (world->ground[z * world->n + x] == W3_NONE && nth-- == 0) {
				*tx = x; *tz = z;
				return true;
			}
	return false;
}
#endif

/* the CPU's cycle counter, for the report (none on the host) */
static inline uint32_t cycles_now(void) {
#ifdef AR_HOST_TESTS
	return 0;
#else
	uint32_t v;
	__asm__ volatile ("rdcycle %0" : "=r"(v));
	return v;
#endif
}

static inline uint32_t instret_now(void) {
#ifdef AR_HOST_TESTS
	return 0;
#else
	uint32_t v;
	__asm__ volatile ("rdinstret %0" : "=r"(v));
	return v;
#endif
}

int main(void) {
	bool quit = false;
	uint32_t dt = 1, perf_t0 = 0, perf_frames = 0, perf_cycles = 0, perf_tris = 0;

	printf("mausrun3d: Maus Run in 3D\n");
	if (!load_world()) return 1;
	start_level();
	printf("mausrun3d: %s, %dx%d tiles, %d cheese, %d others\n", world->name, world->n, world->n,
		cheese_all, nact - cheese_all);
	heading = cam_yaw = 16384;           /* east, along the path */
	/* the cells round the start now; the rest as they come into view */
	{
		int cx, cz, sx = (start_x >> 12) / W3_CELL, sz = (start_z >> 12) / W3_CELL;
		for (cz = sz - 3; cz <= sz + 3; cz++)
			for (cx = sx - 3; cx <= sx + 3; cx++)
				if (cx >= 0 && cz >= 0 && cx < world->ncell && cz < world->ncell)
					w3_cell_build(world, cx, cz);
	}

	ar_chunky(true);
	if (!ar_init(&desc)) {
		printf("mausrun3d: failed to create the window\n");
		return 1;
	}
	sound_on = zsfx_ok();
	if (sound_on) zsfx_music_play(&snd_song_meadow);
	ar_set_fullscreen(true);

	while (!quit) {
		ar_event_t e;
		bool jump = false, recentre = false;
		uint32_t k;
		while (ar_poll(&e)) {
			if (e.type == AR_EV_QUIT) quit = true;
			if (e.type == AR_EV_MODE) mode_changed();
			if (e.type == AR_EV_BUTTON) {
				if (e.pressed) in_down++; else in_up++;
			}
			if (e.type == AR_EV_BUTTON && e.pressed && e.player == 0 && e.key == AR_BTN_A) jump = true;
			if (e.type == AR_EV_BUTTON && e.pressed && e.key == AR_BTN_START) recentre = true;
			if (e.type == AR_EV_KEY && e.pressed && (e.key == 'w' || e.key == 'W')) {
				frame_wait = !frame_wait;
				printf("mausrun3d: %s\n", frame_wait ? "waiting for each frame (W)" :
					"not waiting for frames");
			}
			if (e.type == AR_EV_KEY && e.pressed && (e.key == 'l' || e.key == 'L')) {
				frame_lock = !frame_lock;
				printf("mausrun3d: %s\n", frame_lock ? "locked to 30 frames a second (L)" :
					"frame rate free");
			}
			if (e.type == AR_EV_KEY && e.pressed && (e.key == 't' || e.key == 'T')) {
				tex_want = !tex_want;
				printf("mausrun3d: textures %s%s\n", tex_want ? "on" : "off",
					tex_have ? "" : " (this T2 has none)");
			}
		}
		if (quit) break;
		{
			uint32_t h = ar_held(0) | ar_held(1);
			if (h != in_last) in_flips++;
			in_last = h;
		}
		for (k = 0; k < dt; k++) {
			uint32_t held = ar_held(0) | (ar_held(1) & AR_BTN_DIRS);
			bool j = jump && k == 0;
			switch (gstate) {
			case G_CARD:
				/* three seconds, or Space */
				if (--gtimer <= 0 || j) { gstate = G_PLAY; j = false; }
				held = 0;
				break;
			case G_DONE:
			case G_OVER:
				gtimer++;
				if (j && gtimer > 30) {
					start_level();
					heading = cam_yaw = 16384;
				}
				held = 0;
				j = false;
				break;
			default:
				run_ticks++;
				break;
			}
			if (gstate == G_PLAY || gstate == G_CARD) {
				uint32_t g0 = cycles_now(), gi = instret_now();
				step_mouse(held, j);
				step_blocks();
				step_actors();
				tsum[T_GAME] += cycles_now() - g0;
				isum[T_GAME] += instret_now() - gi;
			}
			step_camera(ar_held(1), recentre && k == 0, (held & AR_BTN_DIRS) != 0);
			ticks++;
		}
		if (chunky) {
			uint32_t c0 = cycles_now();
			render();
			perf_cycles += cycles_now() - c0;
			perf_frames++;
			perf_tris += tris_sent;
			if (ar_ticks() - perf_t0 >= 120) {
				printf("mausrun3d: %lu frames a second, %lu cycles a frame (CPU), %lu triangles sent, view %ld tiles, textures %s%s\n",
					(unsigned long)(perf_frames * 60 / (ar_ticks() - perf_t0)),
					(unsigned long)(perf_cycles / perf_frames),
					(unsigned long)(perf_tris / perf_frames), (long)(far_now / W3_T),
					!tex_have ? "none" : tex_want ? "on" : "off",
					frame_lock ? ", locked to 30" : frame_wait ? ", waiting for each frame" : "");
				printf("mausrun3d:   a frame: sky %lu, world %lu (%lu lists), things %lu, HUD %lu, zgl_frame %lu; the game %lu\n",
					(unsigned long)(tsum[T_SKY] / perf_frames), (unsigned long)(tsum[T_WORLD] / perf_frames),
					(unsigned long)(calls_sum / perf_frames), (unsigned long)(tsum[T_THINGS] / perf_frames),
					(unsigned long)(tsum[T_HUD] / perf_frames), (unsigned long)(tsum[T_FRAME] / perf_frames),
					(unsigned long)(tsum[T_GAME] / perf_frames));
				/* the same in instructions (cycles over instructions high:
				 * waiting, on T2's FIFO or memory); then ar_frame(): the
				 * wait for the frame boundary (and for T2, if it is behind) */
				printf("mausrun3d:   instructions: sky %lu, world %lu, things %lu, HUD %lu, zgl_frame %lu; the game %lu; ar_frame %lu cycles\n",
					(unsigned long)(isum[T_SKY] / perf_frames), (unsigned long)(isum[T_WORLD] / perf_frames),
					(unsigned long)(isum[T_THINGS] / perf_frames), (unsigned long)(isum[T_HUD] / perf_frames),
					(unsigned long)(isum[T_FRAME] / perf_frames), (unsigned long)(isum[T_GAME] / perf_frames),
					(unsigned long)(tsum[T_WAIT] / perf_frames));
				printf("mausrun3d:   input: %lu presses, %lu releases, what is held changed %lu times (%lu frames)\n",
					(unsigned long)in_down, (unsigned long)in_up, (unsigned long)in_flips,
					(unsigned long)perf_frames);
				in_down = in_up = in_flips = 0;
				memset(tsum, 0, sizeof(tsum));
				memset(isum, 0, sizeof(isum));
				calls_sum = 0;
				perf_t0 = ar_ticks();
				perf_frames = perf_cycles = perf_tris = 0;
			}
		}
		if (chunky && (frame_wait || frame_lock)) glFinish();
		{
			uint32_t w0 = cycles_now();
			dt = ar_frame();
			/* L: a frame stays up for two refreshes, never one */
			if (frame_lock && dt < 2) dt += ar_frame();
			tsum[T_WAIT] += cycles_now() - w0;
		}
		if (chunky && gstate == G_PLAY) pace(dt);
	}
	if (chunky) zgl_close();
	ar_shutdown();
	return 0;
}
