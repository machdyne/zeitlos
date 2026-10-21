/*
 * mausrun -- how things move. See phys.h.
 */

#include "phys.h"

bool mr_track_shelf;

static inline bool is_shelf(uint8_t t) {
	return t == MR_TILE_ONEWAY || (mr_track_shelf && t == MR_TILE_TRACK);
}

bool mr_box_hits_solid(int32_t px, int32_t py, int w, int h) {
	/* The four corners plus the midpoints of the vertical edges. With
	 * 16px tiles and a 14px-tall hitbox a tile cannot fit entirely
	 * between the samples, so corners alone would do -- the midpoints
	 * are there because the hitbox is allowed to grow. */
	static const int fx[6] = { 0, 1, 0, 1, 0, 1 };
	static const int fy[6] = { 0, 0, 1, 1, 2, 2 };
	int i;
	for (i = 0; i < 6; i++) {
		int32_t sx = px + (fx[i] ? (w - 1) : 0);
		int32_t sy = py + (fy[i] == 0 ? 0 : fy[i] == 1 ? (h - 1) : (h / 2));
		if (mr_tile_solid(mr_tile_at(sx, sy))) return true;
	}
	return false;
}

/* Axis-separated: moving both at once and then resolving means deciding
 * which axis caused the overlap, the classic way to get a character
 * that sticks to walls while falling past them. */
void mr_move_actor(mr_actor_t *a, int w, int h) {

	int32_t nx = a->x + a->vx, ny;
	if (mr_box_hits_solid(TOPX(nx), TOPX(a->y), w, h)) a->vx = 0;
	else a->x = nx;

	ny = a->y + a->vy;
	if (mr_box_hits_solid(TOPX(a->x), TOPX(ny), w, h)) {
		a->vy = 0;
	} else {
		/* A shelf is solid from above only: falling, with the feet
		 * above its top before the move and in it after, lands. */
		if (a->vy > 0) {
			int32_t ob = TOPX(a->y) + h, nb = TOPX(ny) + h;
			int k;
			for (k = 0; k < 2; k++) {
				int32_t fx = TOPX(a->x) + (k ? w - 1 : 0);
				if (is_shelf(mr_tile_at(fx, nb - 1))) {
					int32_t top = ((nb - 1) / MR_TILE_H) * MR_TILE_H;
					if (ob <= top) {
						ny = TOFP(top - h);
						a->vy = 0;
						break;
					}
				}
			}
		}
		a->y = ny;
	}

	/* on_ground is PROBED, not inferred from a blocked vertical move:
	 * an actor resting on a floor has vy driven to 0 by gravity then
	 * collision, so on a frame where it does not move there is nothing
	 * to infer from -- and the flag cleared on alternate frames, which
	 * made a jump pressed on the wrong frame do nothing. */
	{
		int32_t fx = TOPX(a->x);
		int32_t fy = TOPX(a->y) + h;
		a->on_ground =
			mr_tile_solid(mr_tile_at(fx, fy)) ||
			mr_tile_solid(mr_tile_at(fx + w - 1, fy)) ||
			((fy % MR_TILE_H) == 0 &&
			 (is_shelf(mr_tile_at(fx, fy)) ||
			  is_shelf(mr_tile_at(fx + w - 1, fy))));
	}
}

bool mr_box_touches(int32_t px, int32_t py, int w, int h, uint8_t tile) {
	int i;
	static const int fx[5] = { 0, 1, 0, 1, 2 };
	static const int fy[5] = { 0, 0, 1, 1, 2 };
	for (i = 0; i < 5; i++) {
		int32_t sx = px + (fx[i] == 2 ? w / 2 : fx[i] ? w - 1 : 0);
		int32_t sy = py + (fy[i] == 2 ? h / 2 : fy[i] ? h - 1 : 0);
		if (mr_tile_at(sx, sy) == tile) return true;
	}
	return false;
}

bool mr_thorns_hit(const mr_actor_t *p) {
	/* the bush's points are its lower ten rows: the feet and the
	 * middle of the body, not the ears */
	int32_t x = TOPX(p->x), y = TOPX(p->y);
	int32_t pts[3][2] = {
		{ x + 1, y + MR_PLAYER_H - 1 }, { x + MR_PLAYER_W - 2, y + MR_PLAYER_H - 1 },
		{ x + MR_PLAYER_W / 2, y + MR_PLAYER_H / 2 }
	};
	int i;
	for (i = 0; i < 3; i++) {
		int32_t px = pts[i][0], py = pts[i][1];
		if (mr_tile_at(px, py) == MR_TILE_THORNS && (py % MR_TILE_H) >= 6) return true;
	}
	return false;
}

void mr_player_step(mr_actor_t *p, int dir, bool jump_edge,
	unsigned powers, mr_step_ev_t *ev) {

	bool was_air, was_water = p->in_water;
	int32_t speed, vy_before;

	ev->jumped = ev->landed = ev->sprung = ev->air_jump = false;
	ev->splash = ev->bumped = false;

	p->in_water = mr_tile_at(TOPX(p->x) + MR_PLAYER_W / 2,
		TOPX(p->y) + MR_PLAYER_H / 2) == MR_TILE_WATER;
	if (p->in_water && !was_water && p->vy > 0) ev->splash = true;
	/* breaking the surface on the way up: a hop out, enough to land on
	 * a bank level with the water */
	if (was_water && !p->in_water && p->vy < 0 && p->vy > MR_BREACH_SPEED)
		p->vy = MR_BREACH_SPEED;

	speed = p->in_water ? MR_SWIM_SPEED :
		(powers & MR_PW_SNEAKERS) ? MR_FAST_SPEED : MR_RUN_SPEED;
	p->vx = 0;
	if (dir < 0) { p->vx = -speed; p->face_left = true; }
	if (dir > 0) { p->vx = speed; p->face_left = false; }

	if (p->on_ground) p->air_jumps = (powers & MR_PW_FEATHER) ? 1 : 0;

	if (jump_edge) {
		if (p->in_water) {
			/* a stroke -- or, with the head already out at the
			 * surface, a whole jump out of the water: climbing out
			 * on strokes alone wants two timed just so, which is
			 * not a thing to ask of a five-year-old */
			int32_t hx = TOPX(p->x) + MR_PLAYER_W / 2;
			p->vy = mr_tile_at(hx, TOPX(p->y) - 4) == MR_TILE_WATER ?
				MR_SWIM_STROKE : MR_JUMP_SPEED;
			p->on_ground = false;
			ev->jumped = true;
		} else if (p->on_ground) {
			p->vy = MR_JUMP_SPEED;
			p->on_ground = false;
			ev->jumped = true;
		} else if (p->air_jumps) {
			p->air_jumps--;
			p->vy = MR_JUMP_SPEED;
			ev->air_jump = true;
		}
	}

	was_air = !p->on_ground;
	p->vy += p->in_water ? MR_SWIM_GRAVITY : MR_GRAVITY;
	{
		int32_t fall = p->in_water ? MR_SWIM_FALL : MR_MAX_FALL;
		if (p->vy > fall) p->vy = fall;
	}
	vy_before = p->vy;
	mr_move_actor(p, MR_PLAYER_W, MR_PLAYER_H);
	if (was_air && p->on_ground) ev->landed = true;

	/* the head hit something on the way up: a ? box? */
	if (vy_before < 0 && p->vy == 0) {
		int32_t hy = TOPX(p->y + vy_before) - 1, x0 = TOPX(p->x);
		int k;
		for (k = 0; k < 3 && !ev->bumped; k++) {
			int32_t hx = k == 0 ? x0 + MR_PLAYER_W / 2 : k == 1 ? x0 : x0 + MR_PLAYER_W - 1;
			if (mr_tile_at(hx, hy) == MR_TILE_QBOX) {
				ev->bumped = true;
				ev->bump_c = (int)(hx / MR_TILE_W);
				ev->bump_r = (int)(hy / MR_TILE_H);
			}
		}
	}

	/* a spring under the feet: up and away */
	if (p->on_ground) {
		int32_t fy = TOPX(p->y) + MR_PLAYER_H, fx = TOPX(p->x);
		if (mr_tile_at(fx, fy) == MR_TILE_SPRING ||
		    mr_tile_at(fx + MR_PLAYER_W - 1, fy) == MR_TILE_SPRING) {
			p->vy = MR_SPRING_SPEED;
			p->on_ground = false;
			ev->sprung = true;
			ev->landed = false;
		}
	}

	if (p->vx) p->anim++;
}
