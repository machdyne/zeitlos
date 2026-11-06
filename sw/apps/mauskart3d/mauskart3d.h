/*
 * mauskart3d -- a kart race on a track, the mouse in a matchbox
 * (docs/mauskart3d.md).
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Units as mausrun3d's: a tile is 16 world units (W3_T), y up, x east,
 * z south. Positions of karts are Q8 (1/256 world unit); angles turn16
 * (65,536 a turn, 0 north = -z, a quarter east = +x).
 */
#ifndef MAUSKART_H
#define MAUSKART_H

#include <stdint.h>
#include <stdbool.h>

#define Q          256

/* -- the track (track.c) -- */

#define SEG_LEN    32          /* world units a segment, along the centreline */
#define ROAD_HW    42          /* half the road's width (34 was hard to keep to) */
#define KERB_W     6           /* a kerb's width, each side */
#define MAX_SEGS   512

typedef struct {
	int32_t x, z;              /* the centreline, world units */
	int16_t dx, dz;            /* the direction along, 2.14 (unit) */
	uint16_t yaw;              /* turn16 of (dx, dz) */
	int16_t curve;             /* the turn to the next segment, turn16 (+ right) */
	uint16_t vmax;             /* the AI's speed here, Q8 world units a tick */
	uint32_t at;               /* world units from the start line */
} seg_t;

extern seg_t tk_seg[MAX_SEGS];
extern int tk_n;               /* segments */
extern uint32_t tk_len;        /* world units a lap */

int tk_tracks(void);           /* how many tracks */
void tk_select(int t);         /* the track tk_build() builds */
int tk_selected(void);
const char *tk_name(void);
int tk_ground(void);           /* its ground's ramp */
void tk_build(void);           /* the centreline into segments */
void tk_lists(void);           /* their display lists (and the ground's) */
bool tk_textures(void);        /* the textures into T2; false where it has none */

/* the segment nearest (x, z) world units, searched round `hint`
 * (any segment when hint < 0); and the signed distance across it
 * (+ right of the direction), world units */
int tk_nearest(int32_t x, int32_t z, int hint, int32_t *across);

/* draw what the camera at (ex, ez) looking along yaw sees, out to `far`
 * world units ahead: returns the triangles sent */
uint32_t tk_draw(int32_t ex, int32_t ez, uint32_t yaw, int32_t far, int seg_hint);
extern uint32_t tk_calls;      /* lists the last tk_draw() called */
extern bool tk_scenery;        /* trees and bushes drawn (S) */

/* things beside and on the track */
enum { TH_QBOX, TH_CHEESE, TH_SPRING, TH_POST, TH_BOARD, TH_BOX };
typedef struct {
	int32_t x, z;              /* world units */
	int16_t seg;
	uint8_t kind;
	uint8_t gone;              /* ticks until a ? box or cheese is back */
	uint16_t yaw;
} thing_t;
#define MAX_THINGS 256
extern thing_t tk_thing[MAX_THINGS];
extern int tk_nthings;

/* the minimap: the centreline as points in a 64x64 box */
extern uint8_t tk_map_x[MAX_SEGS], tk_map_y[MAX_SEGS];

/* -- the palette (as mausrun3d's ramps, mausrun3d/world.h) -- */

#define C_WHITE    1
#define C_HUD      2
#define C_SHADOW   3
#define C_CHEESE   4
#define C_RED      5
#define C_BLACK    6
#define C_DARK     7

/* the material of a ramp (16 entries, dark to light) */
void mk_material(int ramp);
void mk_flat(int colour);

#endif
