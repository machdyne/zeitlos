#ifndef GAMEDEMO_LEVEL_H
#define GAMEDEMO_LEVEL_H

#include <stdint.h>
#include "sprites.h"

#define GD_LEVEL_ROWS 15
#define GD_LEVEL_COLS 128

#define GD_WORLD_W (GD_LEVEL_COLS * GD_TILE_W)   /* 2048 px */
#define GD_WORLD_H (GD_LEVEL_ROWS * GD_TILE_H)   /* 240 px  */

#define GD_MAX_CATS 8

typedef struct {
	int32_t x, y;
} gd_cat_spawn_t;

/* Tile ids, one byte per cell. Populated by gd_level_load() from the
 * ASCII map in level.c. */
extern uint8_t gd_level[GD_LEVEL_ROWS][GD_LEVEL_COLS];

extern int32_t gd_start_x, gd_start_y;

extern gd_cat_spawn_t gd_cat_spawns[GD_MAX_CATS];
extern int gd_cat_spawn_count;

void gd_level_load(void);

/* Solid for collision purposes.
 *
 * Cheese is deliberately NOT solid -- it is walked through and
 * collected. Empty obviously is not. Everything else is.
 *
 * A function rather than a table lookup because there are four tile
 * ids and the compiler will fold this to a pair of comparisons; a
 * table would be more code and one more thing to keep in step with
 * the enum in sprites.h. */
static inline int gd_tile_solid(uint8_t t) {
	return t == GD_TILE_GROUND || t == GD_TILE_GRASS || t == GD_TILE_BOX;
}

/* Tile at a WORLD pixel position, with out-of-bounds handled.
 *
 * Above the map reads as empty, so a high jump does not clip on
 * nothing. Below the map ALSO reads as empty, which is what makes a
 * pit lethal rather than a floor -- the player keeps falling until
 * gamedemo.c notices y has left the world. Left and right read as
 * empty too, so the level ends in open air rather than an invisible
 * wall; the camera clamp is what actually stops the player leaving. */
static inline uint8_t gd_tile_at(int32_t wx, int32_t wy) {
	int32_t c, r;
	if (wx < 0 || wy < 0) return GD_TILE_EMPTY;
	c = wx / GD_TILE_W;
	r = wy / GD_TILE_H;
	if (c >= GD_LEVEL_COLS || r >= GD_LEVEL_ROWS) return GD_TILE_EMPTY;
	return gd_level[r][c];
}

#endif
