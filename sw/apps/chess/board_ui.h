#ifndef CHESS_BOARD_UI_H
#define CHESS_BOARD_UI_H

/*
 * Zeitlos chess -- board and panel rendering.
 *
 * -- one renderer, two very different places to draw --
 *
 * Chess is a dual app (docs/app_kinds.md): a resizable desktop window,
 * and a 320x240 screen in colour, windowed or full screen. Those look
 * nothing alike from the inside: a window is somewhere in the middle
 * of a 640x480 desktop, may be partly covered by other windows and may
 * be resized at any moment; the screen is a fixed 320x240 the arcade
 * runtime (sw/common/games/zarcade.h) draws, in colour.
 *
 * Everything here is therefore written against an ORIGIN and a CLIP
 * RECTANGLE, and neither face gets its own drawing code. The desktop
 * passes the content rectangle; the screen passes 0,0 320x240 after
 * cb_screen(true), which sends every primitive to ar_*(). Colours are
 * ROLES (CB_C_*): one bit on the desktop, cb_palette on the screen.
 *
 * The alternative -- window-relative helpers for one mode and raw
 * framebuffer writes for the other -- is two renderers that drift,
 * and the drift is invisible until somebody switches modes mid-game
 * and the board is laid out differently on the two sides.
 *
 * -- why the layout is computed rather than constant --
 *
 * Because a window can be resized and a person will resize it. The
 * layout is a pure function of the rectangle it is given, which also
 * makes it testable without a framebuffer -- tests/test_layout.c
 * checks the relationships, tests/render.c draws it and writes a
 * picture out to be LOOKED at. Both matter: see sw/common/tests/
 * zrender.h's header for what a geometry assertion cannot catch.
 */

#include "../../common/zgfx.h"
#include "../../common/games/zarcade.h"
#include "game.h"

/* Colour roles: what each thing on the board is. The desktop draws
 * INK, LAST, SEL, HINT, CHECK and RULE lit and the rest dark; the
 * screen face draws them in cb_palette. */
enum {
	CB_C_BG = 0,    /* around the board, the panel */
	CB_C_INK,       /* text */
	CB_C_LIGHT,     /* a light square */
	CB_C_DARK,      /* a dark square */
	CB_C_WPC,       /* a white piece */
	CB_C_BPC,       /* a black piece, any piece's outline */
	CB_C_LAST,      /* the last move's squares */
	CB_C_SEL,       /* the piece picked up */
	CB_C_HINT,      /* where it can go */
	CB_C_CHECK,     /* a king in check */
	CB_C_RULE,      /* rules between panel parts */
	CB_C_DETAIL     /* a black piece's details */
};

extern const uint16_t cb_palette[16];

/* The screen face's inks in black and white (ar_desc_t.ink). */
void cb_mono_inks(ar_ink_t ink[16]);

/* Draw through the arcade runtime (the screen face) or zgfx. */
void cb_screen(bool on);

#define CB_SQ         24      /* a square, in pixels */
#define CB_BOARD      (8 * CB_SQ)
#define CB_LABEL_W    9       /* rank digits down the left */
#define CB_LABEL_H    9       /* file letters along the bottom */
#define CB_GAP        4
#define CB_LINE_H     11      /* message line, command line */

/* The smallest content rectangle that holds everything. Below this
 * the panel is dropped first and the board clipped second, because a
 * board you can see most of is more use than a panel. */
#define CB_MIN_W      (CB_LABEL_W + CB_BOARD)
#define CB_MIN_H      (CB_BOARD + CB_LABEL_H + 2 * CB_LINE_H)

/* Width at which the side panel is worth drawing at all: enough for
 * "12. Nxe5 Qxe5" in the 5x8 font plus a margin. */
#define CB_PANEL_MIN_W 84

typedef struct {

	z_clip_t clip;          /* absolute; nothing is drawn outside it */

	int  ox, oy, w, h;      /* the content rectangle, absolute */

	int  board_x, board_y;  /* top-left of the 8x8 grid, absolute */
	int  rank_x;            /* rank digits */
	int  file_y;            /* file letters */

	bool has_panel;
	int  panel_x, panel_y, panel_w, panel_h;

	int  msg_y;             /* message line */
	int  cmd_y;             /* command line */

	bool flipped;

} cb_layout_t;

/* Pure. No drawing, no globals, no framebuffer. */
void cb_layout(cb_layout_t *L, int ox, int oy, int w, int h, bool flipped);

/* Screen position of a board square's top-left corner. */
void cb_square_xy(const cb_layout_t *L, uint8_t sq, int *x, int *y);

/* Which square a point is over, or CE_SQ_NONE. */
uint8_t cb_xy_square(const cb_layout_t *L, int x, int y);

/* One-time probe of what this bitstream's blitter can do. Call before
 * any drawing; safe to call more than once. */
void cb_init(void);

/* -- drawing -------------------------------------------------------- */

/* The whole content area. */
void cb_draw_all(const cb_layout_t *L, chess_game_t *g);

/* Just the board -- what a move needs. */
void cb_draw_board(const cb_layout_t *L, chess_game_t *g);

/* One square, including whatever highlight belongs on it. */
void cb_draw_square(const cb_layout_t *L, chess_game_t *g, uint8_t sq);

void cb_draw_panel(const cb_layout_t *L, chess_game_t *g);
void cb_draw_status(const cb_layout_t *L, chess_game_t *g);

#endif
