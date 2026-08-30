/*
 * gamedemo -- the level.
 *
 * A tile map, 15 rows tall (240px / 16) and much longer than the
 * 320px viewport. That is the whole point of the demo: the world does
 * not fit on screen, and the scrolling machinery in
 * sw/common/zgame.h is what makes it navigable.
 *
 * Written as ASCII so the level IS its own picture. Editing it is
 * editing the strings; there is no map editor and does not need to be
 * one at this size.
 *
 *   .   empty sky
 *   =   grass-topped ground (the normal floor)
 *   #   solid ground (fill under the grass, and platform undersides)
 *   B   a box -- solid, climbable by jumping on
 *   c   cheese -- collectible, not solid
 *   M   the mouse's start position (becomes empty sky)
 *   C   a cat's start position (becomes empty sky)
 *
 * A PIT is simply a run of columns with no floor tiles. There is no
 * pit tile and there does not need to be one -- falling off the bottom
 * of the map is the failure condition, so a hole in the floor is a
 * pit by construction. That is worth stating because the obvious
 * alternative (a "pit" tile you collide with) would need its own case
 * in the physics for no gain.
 */

#include "level.h"

/* 15 rows of GD_TILE_H = 240 pixels, which is exactly one viewport
 * tall. The level scrolls horizontally only; there is nothing above
 * or below to look at, and the vertical camera is pinned by the page
 * layout anyway (see zgame.h). */
static const char *const level_rows[GD_LEVEL_ROWS] = {
/*        0         1         2         3         4         5         6         7         8         9        10        11        12  */
/*        0    5    0    5    0    5    0    5    0    5    0    5    0    5    0    5    0    5    0    5    0    5    0    5    0  */
	"................................................................................................................................",
	"................................................................................................................................",
	"....................................................................c.....c.....................................................",
	"...................................................................BB...........................................................",
	".................................................................BBB.................c..........................................",
	"..........................c...................................cBBBBB............................................................",
	"..........................B..................................BBBBBB.....................BB......................................",
	"...........................B...............................BB.............c.............BB...B..................................",
	".........c......c..........BBB.....c......................B...........................c.BB.........c.....................c......",
	"................BB..............B.......................BB................................BB..............c.............ccc.....",
	"...M..........................C...............................................C.........................C..............ccccc....",
	"===========...====================..===============..=====================...====================...===========...==============",
	"###########...####################..###############..#####################...####################...###########...##############",
	"###########...####################..###############..#####################...####################...###########...##############",
	"###########...####################..###############..#####################...####################...###########...##############",
};

uint8_t gd_level[GD_LEVEL_ROWS][GD_LEVEL_COLS];

int32_t gd_start_x, gd_start_y;

gd_cat_spawn_t gd_cat_spawns[GD_MAX_CATS];
int gd_cat_spawn_count;

void gd_level_load(void) {

	gd_cat_spawn_count = 0;
	gd_start_x = 3 * GD_TILE_W;
	gd_start_y = 10 * GD_TILE_H;

	for (int r = 0; r < GD_LEVEL_ROWS; r++) {
		const char *row = level_rows[r];
		for (int c = 0; c < GD_LEVEL_COLS; c++) {
			char ch = row[c];
			uint8_t t = GD_TILE_EMPTY;
			switch (ch) {
				case '=': t = GD_TILE_GRASS; break;
				case '#': t = GD_TILE_GROUND; break;
				case 'B': t = GD_TILE_BOX; break;
				case 'c': t = GD_TILE_CHEESE; break;
				case 'M':
					gd_start_x = c * GD_TILE_W;
					gd_start_y = r * GD_TILE_H;
					break;
				case 'C':
					if (gd_cat_spawn_count < GD_MAX_CATS) {
						gd_cat_spawns[gd_cat_spawn_count].x = c * GD_TILE_W;
						gd_cat_spawns[gd_cat_spawn_count].y = r * GD_TILE_H;
						gd_cat_spawn_count++;
					}
					break;
				default: break;
			}
			gd_level[r][c] = t;
		}
	}

}
