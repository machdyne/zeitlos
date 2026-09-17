#ifndef CR_BOARD_H
#define CR_BOARD_H

/*
 * Zeitlos craps -- the table, drawn and clicked.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Same origin-and-clip contract as the other five: everything is drawn
 * against an ORIGIN and a CLIP RECTANGLE in absolute screen
 * coordinates, so a window's content rect and a game-mode page are one
 * renderer with different arguments.
 *
 * -- the layout is the real one, squeezed --
 *
 * A casino craps table is a mirrored pair of ends around a shared
 * middle. There is no room for that here, so this is ONE end: the place
 * numbers along the top, the come box under them, the field, then the
 * two line bets, with the proposition box as a strip at the bottom.
 *
 * Everything a player actually uses is reachable. What is missing is
 * the duplication, and the big-6-and-8 box -- which is a place bet
 * paying even money instead of 7:6, and exists only to catch people who
 * do not know that.
 *
 * -- the dice do not need clearing --
 *
 * A die tile from sw/common/games/zdice.h has a LIT body, so it is
 * opaque: drawn over the previous face it covers it completely. The
 * roll animation is therefore two blits a frame at a FIXED position,
 * with nothing erased and no moment when either die is blank.
 *
 * Moving them around while they tumble would look better and would
 * require clearing where they had been, which is what makes
 * sw/apps/roulette flash. Fixed and opaque is the trade this tree keeps
 * making, and it is the right one on a display with no back buffer.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zgfx.h"
#include "../../common/games/zdice.h"
#include "cr_game.h"

#define CR_MSG_LEN  56
#define CR_CMD_LEN  40
#define CR_LINE_H   10

#define CR_MIN_W    300
#define CR_MIN_H    200

/* The spots on the felt that can be clicked. Ordered so a hit test can
 * walk them, and named so a message can say what was hit. */
#define CR_SPOT_NONE     -1
#define CR_NSPOTS        24

typedef struct { int x, y, w, h; } cr_rect_t;

typedef struct {
    cr_rect_t r;
    uint8_t   type;
    uint8_t   sel;
    const char *label;
} cr_spot_t;

typedef struct {

    z_clip_t clip;
    int  ox, oy, w, h;
    bool ok;

    int  status_y;

    int  dice_x, dice_y;      /* the two dice, side by side */
    int  puck_x, puck_y;      /* the point marker */
    int  info_x;

    cr_spot_t spot[CR_NSPOTS];
    int  nspots;

    int  msg_y, cmd_y;

} cr_layout_t;

typedef struct {

    cr_game_t *g;

    int32_t  chips;
    int32_t  bet;             /* the working stake for a click */

    /* What the dice should show right now. During a roll these tumble;
     * once it lands they are the real result. */
    int      show_d1, show_d2;
    bool     rolling;

    bool     trng;
    char     message[CR_MSG_LEN];
    char     cmd[CR_CMD_LEN];
    int      cmd_len;

} cr_view_t;

void cr_board_layout(cr_layout_t *L, const cr_view_t *v, int ox, int oy,
    int w, int h);

/* True if (x, y) is on the dice. Clicking them rolls, which is what
 * picking up dice is for -- and they are the most obvious thing on the
 * table to click. */
bool cr_board_dice_at(const cr_layout_t *L, int x, int y);

/* Which spot is under a point, or CR_SPOT_NONE. */
int cr_board_spot_at(const cr_layout_t *L, int x, int y);

/* The selector a spot means right now. The odds boxes have no number of
 * their own -- the point is their number, and it moves under them. */
int cr_spot_sel(const cr_layout_t *L, const cr_view_t *v, int i);

void cr_board_init(void);
void cr_board_draw(const cr_layout_t *L, const cr_view_t *v);
void cr_board_draw_status(const cr_layout_t *L, const cr_view_t *v);

/* Just the two dice -- what a roll frame calls. Two opaque blits, no
 * clear. */
void cr_board_draw_dice(const cr_layout_t *L, const cr_view_t *v);

char *cr_num(int32_t n, char *buf);

#endif
