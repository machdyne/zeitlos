#ifndef MJ_BOARD_H
#define MJ_BOARD_H

/*
 * Zeitlos mahjong -- the board: where every tile lands on the canvas,
 * how it is drawn, what is under the pointer.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * No I/O: it draws into an mj_canvas_t, and the app puts the canvas on
 * the screen. The desktop face and the screen face are this one file
 * with a different canvas and a different tile size, so the two cannot
 * drift (the reason sw/apps/blackjack's bj_board.h gives).
 *
 * -- the stacks, in depth --
 *
 * A tile is drawn as a face and two sides. A layer up is shifted by the
 * side's thickness (`d`) away from the sides, so the sides of an upper
 * tile land exactly on the faces under it and the stack reads as a
 * stack. Each tile also throws a shadow `d` wide past its sides, onto
 * whatever is lower -- the felt, or the faces of the layer below --
 * which is what makes the height of a stack visible at a glance rather
 * than only on its edges. Higher layers are lighter (in 64 colours a
 * shade a layer, in 16 the table and the rest), and every face has a
 * light edge where the light comes from and a dark one opposite.
 *
 * Tiles are drawn back to front: by layer, then by how near they are
 * to the viewer along the sides' direction. A tile's sides fall under
 * its neighbour's face, which is drawn after it, so only the outer
 * edges of a row show their sides -- as on a real table.
 *
 * -- the view from a corner --
 *
 * `corner` says where the viewer stands: 0 bottom right (the sides
 * show right and below, layers shift up and left), 1 bottom left, 2 top
 * left, 3 top right. Turning the view mirrors the geometry and the
 * drawing order and nothing else: the faces are not mirrored, the
 * light still comes from the top left, and the free rule -- which is
 * about the tiles, not the view -- does not change.
 *
 * -- coordinates --
 *
 * Board space: a tile's face at x*fw/2 and y*fh/2 (the layout's half
 * cells), shifted by its layer. The board's extent is measured over
 * every PLACE, taken or not, so the board does not move as it empties.
 * Canvas = board - scroll + offset, and the board is centred in its
 * area when it fits, scrolled when it does not.
 */

#include <stdint.h>
#include <stdbool.h>

#include "mj_game.h"
#include "mj_canvas.h"
#include "mj_art.h"

/* -- colours ---------------------------------------------------------------
 *
 * The first sixteen are the picture in 16 colours, and in the colour
 * window. 8..15 are 0..7 in shadow, so a shadow is the bit 8 set (one
 * plane OR'd, mj_cv_shade()), and so that an index drawn through its
 * low three bits -- the colour window on a six-plane board, which shows
 * 8 -- is the same colour out of the shade.
 *
 * 16..31 are only drawn in 64 colours, full screen on a board with six
 * or more planes: a face and two sides per layer. 32..63 are 0..31 in
 * shadow (the bit 32). */
enum {
	MJ_C_BG = 0,        /* the felt */
	MJ_C_INK,           /* black ink on a face */
	MJ_C_RED,
	MJ_C_GREEN,
	MJ_C_BLUE,
	MJ_C_SIDE,          /* the side along the face's long edge */
	MJ_C_FACE,          /* a face */
	MJ_C_SEL,           /* the tile picked up */
	MJ_C_BG2,           /* 8..15: 0..7 in shadow (index | 8) */
	MJ_C_EDGE,          /* outlines; ink in shadow */
	MJ_C_RED2,
	MJ_C_BAR,           /* green in shadow; the status bar */
	MJ_C_BLUE2,
	MJ_C_SIDE2,         /* the short side; the long side in shadow */
	MJ_C_FACE0,         /* a face in shadow; a face's dark inner edge */
	MJ_C_HINT,          /* the selection in shadow; pulses while a hint is up */

	MJ_C_FACE_Z = 16,   /* 16..21: a face, layers 0..5 (64 colours) */
	MJ_C_SIDE_Z = 22,   /* 22..27 */
	MJ_C_SIDE2_Z = 28,  /* 28..31, layers 0..3 and up */
	MJ_C_SHADE64 = 32,  /* 32..63: 0..31 in shadow (index | 32) */
	MJ_C_COLOURS = 64
};
#define MJ_C_SHADE16  8
#define MJ_C_CURSOR   MJ_C_RED  /* the keyboard's frame */
#define MJ_C_TEXT MJ_C_FACE

/* RGB444, for ar_pal_load() / desc.palette. */
extern const uint16_t mj_palette[MJ_C_COLOURS];

/* Monochrome inks for the first 16 (ar_ink_t's layout), and which of
 * them each of the 64 uses. */
extern const uint8_t mj_inks[16][8];
extern const uint8_t mj_ink_map[MJ_C_COLOURS];

/* -- the status bar -------------------------------------------------------- */

#define MJ_BAR_H        11
#define MJ_TEXT_LEN     48

enum { MJ_B_NEW = 0, MJ_B_UNDO, MJ_B_HINT, MJ_B_SHUFFLE, MJ_B_ZOOM, MJ_NB };

/* -- the view ---------------------------------------------------------------- */

typedef struct {
	const mj_game_t *g;

	/* What to draw it with -- set by the app, then mj_view_layout(). */
	int   size;             /* MJ_ART_SMALL or MJ_ART_LARGE */
	int   corner;           /* 0..3, see above */
	bool  rich;             /* 64 colours: the per-layer shades */
	bool  show_free;        /* dim the tiles that cannot be taken */
	int   buttons;          /* which bar buttons, bit MJ_B_* */

	/* What is lit up. -1 for none. */
	int   sel, cursor, hint_a, hint_b;

	/* The bar: `status` is what is going on; `msg`, when set, says
	 * something for a while in its place. */
	char  status[MJ_TEXT_LEN];
	char  msg[MJ_TEXT_LEN];

	/* -- worked out by mj_view_layout() -- */
	int   fw, fh, d;
	int   area_x, area_y, area_w, area_h;   /* the board's part of the canvas */
	int   bar_y;                            /* the bar's top row; area_y + area_h */
	int   cw;                               /* canvas width */
	int   bx0, by0, bw, bh;                 /* the board in board space */
	int   scroll_x, scroll_y;               /* board space at the area's corner */
	int   off_x, off_y;                     /* canvas = board + off */
	bool  scrolls;                          /* bigger than its area */
	uint16_t order[MJ_MAX];
	int   norder;
	/* For each place, the four that hide it entirely when all four are
	 * on the table: the one right on top of it, and its neighbours on
	 * the sides' side -- beside, below, and diagonally. -1 where there is
	 * no such place, and then it is never hidden. */
	int16_t cover[MJ_MAX][4];
	int   bx[MJ_NB], bwid[MJ_NB];           /* buttons, x and width; 0 = hidden */
} mj_view_t;

void mj_view_init(mj_view_t *v, const mj_game_t *g);

/* Lays the board out on a canvas w x h: the bar along the bottom, the
 * board centred above it, or scrolled to keep the scroll position when
 * it is larger. Call again after a change of size, corner, layout or
 * canvas. */
void mj_view_layout(mj_view_t *v, int w, int h);

/* Whether the board at tile size `size` fits a canvas w x h whole. */
bool mj_view_fits(const mj_game_t *g, int size, int w, int h);

/* Scrolls by (dx, dy) pixels, clamped. True if anything moved; how far
 * it actually moved is the change in scroll_x and scroll_y. */
bool mj_view_scroll(mj_view_t *v, int dx, int dy);
/* Scrolls so tile t is wholly in view, if it is not. True if moved. */
bool mj_view_reveal(mj_view_t *v, int t);

/* Canvas positions. */
void mj_tile_face(const mj_view_t *v, int t, int *x, int *y);
/* Everything tile t draws: face, sides and shadow. */
void mj_tile_box(const mj_view_t *v, int t, int *x, int *y, int *w, int *h);

/* Whether mj_render() skips tiles that are wholly covered (1, always,
 * but for the test that proves the picture is the same either way). */
extern int mj_board_cull;

/* Draws the board's area within the rectangle (felt and tiles), and
 * the bar. */
void mj_render(const mj_view_t *v, mj_canvas_t *cv, int x, int y, int w, int h);
void mj_render_bar(mj_view_t *v, mj_canvas_t *cv);
void mj_render_all(mj_view_t *v, mj_canvas_t *cv);
/* After mj_view_scroll() moved the view by (dx, dy): the board's area
 * of the canvas moved to match and the strips that came into view
 * drawn -- the same picture as mj_render_all() at a fraction of the
 * cost. False, having drawn nothing, when it cannot be done that way
 * (a move as large as the area; in mono, one that would put the
 * dithers out of step): then draw it all. */
bool mj_render_scrolled(mj_view_t *v, mj_canvas_t *cv, int dx, int dy);

/* Every tile's freedom now, a byte each. With show_free on, a move
 * changes how tiles that did not move are drawn -- the ones it freed or
 * pinned -- so the app takes this before a move and after, and repaints
 * the boxes of those that differ as well as the two that went. */
void mj_free_flags(const mj_game_t *g, uint8_t *out);

/* The tile drawn at canvas (x, y), topmost first; -1 for none. */
int  mj_hit(const mj_view_t *v, int x, int y);
/* The bar button at (x, y), or -1. */
int  mj_bar_hit(const mj_view_t *v, int x, int y);

/* Keyboard: from tile `from` (-1: the start), the nearest tile in a
 * direction (0 left, 1 right, 2 up, 3 down -- on the screen, whichever
 * way the view is turned) among the tiles with nothing on them. -1 if
 * there is none that way. */
int  mj_nav(const mj_view_t *v, int from, int dir);
/* Tab and shift-Tab: the next (step 1) or previous (-1) FREE tile in
 * reading order, round the end to the start. -1 if none is free. */
int  mj_nav_tab(const mj_view_t *v, int from, int step);
/* A tile with nothing on it to start the cursor on: the free one
 * nearest the top left, else any. -1 on an empty table. */
int  mj_nav_first(const mj_view_t *v);

#endif
