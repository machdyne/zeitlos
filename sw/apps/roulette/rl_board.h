#ifndef RL_BOARD_H
#define RL_BOARD_H

/*
 * Zeitlos roulette -- the betting layout, drawn and clicked.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- one renderer, two rectangles --
 *
 * Same contract as sw/apps/poker/table_ui.h and for the same reason:
 * everything is drawn against an ORIGIN and a CLIP RECTANGLE in
 * absolute screen coordinates, so a window's content rect and a
 * game-mode page are the same code with different arguments. Two
 * renderers drift, and the drift is invisible until somebody switches
 * modes mid-round.
 *
 * -- the grid is three rows of twelve, and the numbers run upward --
 *
 *      3  6  9 12 15 18 21 24 27 30 33 36      <- row 0, column bet 2:1
 *      2  5  8 11 14 17 20 23 26 29 32 35      <- row 1
 *      1  4  7 10 13 16 19 22 25 28 31 34      <- row 2
 *
 * number(col, row) = col * 3 + (3 - row). That is the printed layout,
 * and it is why "column" and "row" are confusing words here: a COLUMN
 * BET is one of the three horizontal rows above, because it is a
 * column on the table as the player faces it. The code says `row` for
 * the grid and `column bet` for the wager, and never mixes them.
 *
 * -- clicking a line, not a square --
 *
 * On a real table a split is bet by placing a chip ON THE LINE between
 * two numbers, a corner on the point where four meet, a street on the
 * outer edge of a row of three. Offering only straight-up bets by
 * click, and making everything else a typed command, would leave most
 * of the table unreachable with a mouse.
 *
 * So rl_board_hit() resolves a point to whatever it is nearest: the
 * middle of a cell is a straight-up bet, near an edge is a split, near
 * an interior corner is a corner, and below the grid is a street or a
 * six line. The threshold is a third of a cell, which is generous
 * enough to hit with a mouse on a 320-pixel screen and tight enough
 * that the middle of a cell is unambiguous.
 *
 * tests/board_test.c checks the mapping the only way that means
 * anything: for every point it resolves, the bet's coverage must be
 * exactly the set of numbers whose cells touch that point.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zgfx.h"
#include "rl_table.h"
#include "rl_wheel.h"

#define RL_MSG_LEN     48
#define RL_CMD_LEN     40
#define RL_HISTORY     8

#define RL_LINE_H      10
#define RL_CELL_W      22
#define RL_CELL_H      14
#define RL_ZERO_W      18
#define RL_OUTER_H     12       /* the dozens and even-money rows */

/* Below this there is no room for a table anybody can click. */
#define RL_MIN_W       300
#define RL_MIN_H       200

/* The chip denominations, smallest first. */
#define RL_NCHIPS 4
extern const int32_t rl_chip_values[RL_NCHIPS];

typedef struct {
    int x, y, w, h;
} rl_rect_t;

typedef struct {

    z_clip_t clip;
    int  ox, oy, w, h;
    bool ok;

    int  status_y;

    /* The wheel sits left of an information panel. */
    int  wheel_cx, wheel_cy, wheel_r;
    int  panel_x, panel_y, panel_w;

    /* The numbers grid, and the outside bets under it. */
    int  grid_x, grid_y;        /* top-left of the 12x3 number cells */
    int  zero_x;                /* the zero cell, left of the grid */
    int  colbet_x;              /* the three column bets, right of it */
    int  street_y;              /* the strip below the grid */
    int  dozen_y;
    int  even_y;

    /* The chip selector and the spin button. */
    rl_rect_t chip[RL_NCHIPS];
    rl_rect_t spin;
    rl_rect_t clear;

    int  msg_y, cmd_y;

} rl_layout_t;

typedef struct {

    rl_round_t *round;
    rl_wheel_t *wheel;

    int32_t  chips;             /* the bank, as last read */
    int      chip_sel;          /* index into rl_chip_values */

    int      result;            /* last spun pocket, -1 if none yet */
    int32_t  last_return;       /* what the last round paid back */
    int32_t  last_staked;

    int      history[RL_HISTORY];
    int      nhistory;

    bool     trng;              /* is the generator hardware-seeded */
    bool     spinning;

    char     message[RL_MSG_LEN];
    char     cmd[RL_CMD_LEN];
    int      cmd_len;

} rl_view_t;

/* Pure. No drawing, no globals, no framebuffer. */
void rl_board_layout(rl_layout_t *L, const rl_view_t *v, int ox, int oy,
    int w, int h);

/* Resolves a point to a bet. Returns false if it is not on anything.
 * `type` and `sel` are as rl_table.h defines them. */
bool rl_board_hit(const rl_layout_t *L, const rl_view_t *v, int x, int y,
    int *type, int *sel);

/* The chip denomination button under a point, or -1. */
int rl_board_chip_at(const rl_layout_t *L, int x, int y);

/* Where a number's cell is, for drawing a chip on it. */
void rl_board_cell(const rl_layout_t *L, int number, rl_rect_t *out);

void rl_board_init(void);
void rl_board_draw(const rl_layout_t *L, const rl_view_t *v);
void rl_board_draw_wheel(const rl_layout_t *L, const rl_view_t *v,
    const z_clip_t *clip);
void rl_board_draw_status(const rl_layout_t *L, const rl_view_t *v);

char *rl_num(int32_t n, char *buf);

#endif
