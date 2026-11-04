/*
 * mausrun3d -- the world: its map, its blocks, and its cells as display
 * lists (docs/mausrun3d.md, "Worlds" and "Drawing the world").
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Units: a tile is W3_T world units across and a block W3_T high, x
 * east, z south, y up (GL's y). The map is SIZE x SIZE tiles (16 to
 * 512, a multiple of W3_CELL), at most W3_H blocks high.
 */
#ifndef MAUSRUN3D_WORLD_H
#define MAUSRUN3D_WORLD_H

#include <stdint.h>
#include <stdbool.h>

#define W3_T      16          /* world units a tile (and a block) */
#define W3_H      16          /* blocks high */
#define W3_CELL   8           /* tiles a side of a cell */
#define W3_MAXN   512
#define W3_NONE   0xff

/* what a block is */
enum {
	W3_AIR = 0,
	W3_GROUND,                /* the ground: its top by the surface, sides earth */
	W3_BOX,
	W3_QBOX,                  /* ? box */
	W3_SHELF,                 /* solid from above only */
	W3_SPRING,
	W3_BISCUIT,
	W3_THORNS,
	W3_KINDS
};

/* a thing on the map (the world file's `things`) */
typedef struct {
	int16_t x, z, y;          /* tile, block */
	char c;                   /* c cheese, C cat, M start, ... (gen_world.py) */
} w3_thing_t;

/* the faces a cell's lists hold, by the way they face */
enum { W3_FY = 0, W3_FNY, W3_FX, W3_FNX, W3_FZ, W3_FNZ, W3_FWATER, W3_NF };

typedef struct {
	uint32_t list;            /* GL list name of face set 0 (W3_NF in a row); 0 not made */
	uint16_t tris[W3_NF];     /* triangles each */
	/* where a face set can be seen from: the eye past lim[] (world
	 * units) on the side the faces face -- for W3_FY above the lowest
	 * top, for W3_FX east of the westmost east face, and so on */
	int16_t lim[W3_NF];
	uint8_t ymax;             /* blocks: nothing solid at or above */
	bool built;
} w3_cell_t;

typedef struct {
	char name[40];
	char theme[16];
	int n;                    /* tiles a side */
	int ncell;                /* cells a side */
	uint8_t *ground;          /* n*n: the ground's top, blocks; W3_NONE a chasm */
	uint8_t *surf;            /* n*n: g p s r d */
	uint8_t *water;           /* n*n: the water's surface, blocks; W3_NONE */
	uint8_t *vox;             /* n*n*W3_H: block kinds */
	w3_thing_t *things;
	int nthings;
	w3_cell_t *cells;         /* ncell*ncell */
} w3_world_t;

/* Parse a world file's text (gen_world.py's). NULL and *err said if it
 * does not parse. */
w3_world_t *w3_parse(const char *text, const char **err, int *line);
void w3_free(w3_world_t *w);

/* the block at (x, y, z) in tiles and blocks; air outside the map,
 * ground below it (but under a chasm) */
static inline uint8_t w3_block(const w3_world_t *w, int x, int y, int z) {
	if (x < 0 || z < 0 || x >= w->n || z >= w->n || y >= W3_H) return W3_AIR;
	/* below the map: ground, but for a chasm, which goes down for ever */
	if (y < 0) return w->ground[(uint32_t)z * (uint32_t)w->n + (uint32_t)x] == W3_NONE ? W3_AIR : W3_GROUND;
	return w->vox[((uint32_t)z * (uint32_t)w->n + (uint32_t)x) * W3_H + (uint32_t)y];
}

/* the water's surface at tile (x, z), in blocks, or -1 */
static inline int w3_water(const w3_world_t *w, int x, int z) {
	uint8_t v;
	if (x < 0 || z < 0 || x >= w->n || z >= w->n) return -1;
	v = w->water[(uint32_t)z * (uint32_t)w->n + (uint32_t)x];
	return v == W3_NONE ? -1 : v;
}

/* The palette's ramps (docs/mausrun3d.md, "Colours"): 16 entries each */
enum {
	W3_R_SKY = 16, W3_R_GRASS = 32, W3_R_EARTH = 48, W3_R_PATH = 64,
	W3_R_SAND = 80, W3_R_ROCK = 96, W3_R_BOX = 112, W3_R_QBOX = 128,
	W3_R_WATER = 144, W3_R_MOUSE = 160, W3_R_PINK = 176, W3_R_CHEESE = 192,
	W3_R_CAT = 208, W3_R_SHELF = 224, W3_R_RED = 240
};
#define W3_C_SHADOW 3
#define W3_C_WHITE  1
#define W3_C_HUD    2

/* the ramps' colours, dark to light, by theme */
void w3_palette(const w3_world_t *w, uint16_t pal[256]);

/* a block changed while playing: its cell's lists made again */
void w3_set_block(w3_world_t *w, int x, int y, int z, uint8_t k);

/* Make cell (cx, cz)'s lists if they are not made (GL lists, compiled
 * on T2 the first time they are drawn). */
void w3_cell_build(w3_world_t *w, int cx, int cz);

/* Draw the world as the camera at (ex, ey, ez) world units sees it,
 * looking along yaw (turn16: 0 north (-z), a quarter east) and down by
 * pitch (turn16), the frustum's half width and height over its depth
 * ta and tb (16.16), out to `far` world units; builds at most `build`
 * cells not made yet. Returns the triangles sent. */
extern uint32_t w3_calls;     /* lists the last w3_draw() called */
uint32_t w3_draw(w3_world_t *w, int32_t ex, int32_t ey, int32_t ez, uint32_t yaw,
	uint32_t pitch, int32_t ta, int32_t tb, int32_t far, int build);

#endif
