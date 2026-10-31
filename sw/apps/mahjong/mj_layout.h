#ifndef MJ_LAYOUT_H
#define MJ_LAYOUT_H

/*
 * Zeitlos mahjong -- layouts: where the tiles sit.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * No I/O. A layout is parsed from text that came from anywhere -- a
 * built-in string (mj_layouts.c) or a file under /data/mahjong/layouts
 * that the app read -- so that the host tests build the shipped parser
 * and the board reads exactly what the tests read.
 *
 * -- coordinates are in HALF tiles --
 *
 * A tile covers a 2x2 block of half-tile cells. Most layouts sit on
 * whole-tile steps; the Turtle's three side tiles and its top tile sit
 * on half steps, which is what the half grid is for. x runs right, y
 * runs down, z is the layer (0 is the table).
 *
 * -- the text format --
 *
 *   # a comment
 *   name Turtle
 *   layer
 *   ..[][][]
 *   ..[][][]
 *   layer
 *   ...
 *
 * Every `layer` starts a new layer, numbered from 0, and every line
 * after it is one row of half cells, from the top of the layout. A
 * tile is written as its whole 2x2 block, "[]" over "[]"; anything
 * else that is not '.' (or a space) is an error. Every layer counts
 * its rows from the same top line, so a tile drawn on the fifth line
 * of layer 2 is directly above one drawn on the fifth line of layer 0.
 *
 * The whole block rather than one anchor character because a layout
 * is edited by looking at it: a missing ']' is visible in a way a
 * missing anchor among dots is not, and the parser reads it back as
 * an error with a line number rather than as a tile that moved.
 *
 * -- the free rule, precomputed --
 *
 * A tile may be taken when nothing lies on it and one of its long
 * sides is open. "On it" is any tile on a HIGHER layer whose block
 * overlaps its block at all -- a half overlap counts, and so does a
 * tile two layers up with a gap between (a floating tile still pins
 * what is under it). "Beside it" is a tile on the SAME layer whose
 * block starts exactly one tile to the left or right and overlaps it
 * vertically by any amount -- which is how the Turtle's half-step side
 * tiles hold both rows next to them.
 *
 * Those neighbour lists depend on the layout only, so they are worked
 * out once when it is parsed, and the free test while playing is a
 * handful of lookups.
 */

#include <stdint.h>
#include <stdbool.h>

/* 288 positions: two full sets. A layout larger than one set repeats
 * the set (mj_game.c); the limit is what the neighbour lists cost
 * (about 30 bytes a position) rather than anything in the rules. */
#define MJ_MAX          288
#define MJ_MAX_W        96      /* half cells */
#define MJ_MAX_H        64      /* half cells */
#define MJ_MAX_Z        12      /* layers */
#define MJ_ABOVE        8       /* tiles that may lie on one tile */
#define MJ_SIDE         2       /* neighbours on one side */
#define MJ_NAME         24

typedef struct {
	uint8_t x, y, z;            /* half cells, half cells, layer */
} mj_pos_t;

typedef struct {
	char     name[MJ_NAME];
	int      n;                 /* positions; always even, >= 2 */
	int      w, h, d;           /* extent: half cells, half cells, layers */

	/* In canonical order: by layer, then row, then column. Every other
	 * index in the engine is an index into this array. */
	mj_pos_t pos[MJ_MAX];

	uint16_t above[MJ_MAX][MJ_ABOVE];
	uint8_t  nabove[MJ_MAX];
	uint16_t left[MJ_MAX][MJ_SIDE];
	uint8_t  nleft[MJ_MAX];
	uint16_t right[MJ_MAX][MJ_SIDE];
	uint8_t  nright[MJ_MAX];
} mj_layout_t;

/* Parses `text` (NUL-terminated) into *L. On failure returns false and
 * writes a one-line reason, with the line number where there is one,
 * into err. The layout is moved so its leftmost column and top row
 * are 0. */
bool mj_layout_parse(const char *text, mj_layout_t *L, char *err, int errlen);

/* Built-in layouts (mj_layouts.c). The first is the Turtle. */
int         mj_layout_builtin_count(void);
const char *mj_layout_builtin_text(int i);

/* True when tile blocks a and b overlap in x and y (any layer). */
bool mj_layout_overlap(const mj_layout_t *L, int a, int b);

#endif
