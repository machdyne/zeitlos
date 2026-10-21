#ifndef MAUSRUN_PHYS_H
#define MAUSRUN_PHYS_H

/*
 * mausrun -- how things move. No drawing, no hardware: the same file is
 * compiled into the game and into the host tools (tests/levelcheck.c),
 * so "can this level be finished" is answered by the physics the
 * player actually has.
 *
 * -- units --
 *
 * 1/16 pixel. Fixed point rather than float: this runs on a core whose
 * FPU is libgcc. 1/16 is enough resolution that gravity accumulates
 * smoothly at 60fps and coarse enough that everything stays in 32 bits.
 *
 * -- tuned together, not independently --
 *
 * With gravity 6 and a 1/16px unit, a jump of -96 gives 32 frames of
 * airtime, which at a run speed of 32 covers 64px -- four tiles -- and
 * reaches 48px high, three. The levels are built against those two
 * numbers: no gap wider than 3 tiles, no step taller than 2, so every
 * jump has a tile of margin. tests/levelcheck.c is what keeps a level
 * honest about that.
 */

#include <stdint.h>
#include <stdbool.h>

#include "level.h"

#define MR_FP 16
#define TOFP(px) ((px) * MR_FP)
#define TOPX(v)  ((v) / MR_FP)

#define MR_GRAVITY      6
#define MR_RUN_SPEED    32
#define MR_JUMP_SPEED (-96)
#define MR_MAX_FALL    112

/* Round 3. A spring throws the mouse about five and a half tiles up; a
 * foe stomped on bounces it two; sneakers run half as fast again. In
 * water everything is slow, and Jump is a stroke, at any time -- a
 * whole jump when the head is already out at the surface, and coming
 * up through the surface is a hop out onto the bank. */
#define MR_SPRING_SPEED (-132)
#define MR_BOUNCE_SPEED  (-72)
#define MR_FAST_SPEED     44
#define MR_SWIM_GRAVITY    2
#define MR_SWIM_FALL      28
#define MR_SWIM_SPEED     20
#define MR_SWIM_STROKE  (-44)
#define MR_BREACH_SPEED (-64)    /* up out of the water: about 21px more */

/* The powers mr_player_step() needs to know about. */
#define MR_PW_FEATHER   1      /* one more jump in the air */
#define MR_PW_SNEAKERS  2      /* faster */

/* The player is narrower than its sprite: a 16-wide hitbox on a
 * 16-wide sprite means the visible ears and tail catch on edges the
 * player can obviously see they should fit through. */
#define MR_PLAYER_W 10
#define MR_PLAYER_H 14
#define MR_PLAYER_XOFF 3      /* hitbox inset within the 16px sprite */

typedef struct {
	int32_t x, y;      /* fixed point, top-left of the HITBOX */
	int32_t vx, vy;
	bool on_ground;
	bool face_left;
	int anim;
	bool alive;
	uint8_t air_jumps;   /* left before landing (the feather) */
	bool in_water;
} mr_actor_t;

/* What happened during a player step, for the sounds and the pictures. */
typedef struct {
	bool jumped, landed, sprung, air_jump;
	bool splash;         /* went into the water */
	bool bumped;         /* its head hit a ? box: at bump_c, bump_r */
	int bump_c, bump_r;
} mr_step_ev_t;

/* A moving platform's track as a shelf (round 4). The game leaves it
 * false and moves the platforms itself (mausrun.c); levelcheck sets it,
 * which is to say: the mouse can stand anywhere the platform goes, by
 * waiting for it -- docs/mausrun.md, "Moving platforms". */
extern bool mr_track_shelf;

/* Is any of the box (pixels) inside something solid? */
bool mr_box_hits_solid(int32_t px, int32_t py, int w, int h);

/* Move by (vx, vy), one axis at a time, stopping at what is solid, and
 * probe whether there is ground under the feet. */
void mr_move_actor(mr_actor_t *a, int w, int h);

/* One frame of the player: run left (dir -1), right (1) or not (0),
 * jump if jump_edge and it can (on the ground; in water; once more in
 * the air with the feather), fall, move, bounce off a spring. */
void mr_player_step(mr_actor_t *p, int dir, bool jump_edge,
	unsigned powers, mr_step_ev_t *ev);

/* Does the box (pixels) touch a tile of this kind? Corners and middle. */
bool mr_box_touches(int32_t px, int32_t py, int w, int h, uint8_t tile);

/* Is the player on the points of a thorn bush? */
bool mr_thorns_hit(const mr_actor_t *p);

#endif
