#ifndef SL_BOARD_H
#define SL_BOARD_H

/*
 * Zeitlos slots -- the machine, drawn and clicked.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Same origin-and-clip contract as the other three: everything is drawn
 * against an ORIGIN and a CLIP RECTANGLE in absolute screen
 * coordinates, so a window's content rect and a game-mode page are one
 * renderer with different arguments.
 *
 * -- the reels do not need the hardware scroll, and that is worth
 *    writing down because it was the plan --
 *
 * z_fb_hw_scroll() moves a rectangle's pixels and leaves only the
 * incoming strip to redraw, which looked like the obvious answer to
 * roulette's flicker. Measuring the alternative said otherwise.
 *
 * A cell is exactly the tile height, so the tiles ABUT: five opaque
 * blits cover a three-row reel completely, every pixel written exactly
 * once, nothing cleared. That is already flicker-free. The scroll would
 * save four blits, and cost a partial-tile path plus a fallback for
 * when z_fb_hw_scroll_allowed() says no.
 *
 * And it would be slower where it matters. Content moving DOWN costs
 * one blit per dy-deep strip; a reel crawling to its stop at two pixels
 * a frame would take forty-eight of them, against five for a redraw.
 * The scroll is fastest when a reel is moving fast, which is exactly
 * when nobody is looking closely.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zgfx.h"
#include "sl_game.h"
#include "sl_art.h"

#define SL_MSG_LEN   48
#define SL_CMD_LEN   40
#define SL_LINE_H    10
#define SL_BTN_H     12

#define SL_MIN_W     280
#define SL_MIN_H     190

/* A payline marker box, square. */
#define SL_MARK_W     9

#define SL_BTN_SPIN   0
#define SL_BTN_BET    1
#define SL_BTN_LINES  2
#define SL_BTN_MAX    3
#define SL_NBTN       4

typedef struct { int x, y, w, h; } sl_rect_t;

typedef struct {

    z_clip_t clip;
    int  ox, oy, w, h;
    bool ok;

    int  status_y;

    int  mark_l, mark_r;      /* the payline number columns, either side */
    int  lever_x;
    sl_rect_t lever;          /* and where it can be pulled */
    int  reel_x, reel_y;      /* top-left of reel 0's window */
    int  reel_pitch;          /* x distance between reels */
    int  reel_h;              /* SL_ROWS * SL_CELL */

    int  pay_x, pay_y;        /* the paytable panel */
    int  info_y;

    sl_rect_t btn[SL_NBTN];

    int  msg_y, cmd_y;

} sl_layout_t;

typedef struct {

    sl_spin_t *spin;

    int32_t  chips;
    int32_t  bet;             /* coins per line */
    int      lines;           /* paylines played */

    int32_t  last_win;
    int32_t  line_pays[SL_LINES];
    bool     have_result;

    bool     trng;
    char     message[SL_MSG_LEN];
    char     cmd[SL_CMD_LEN];
    int      cmd_len;

} sl_view_t;

void sl_board_layout(sl_layout_t *L, const sl_view_t *v, int ox, int oy,
    int w, int h);

int  sl_board_btn_at(const sl_layout_t *L, int x, int y);

/* True if (x, y) is on the handle. Pulling it spins, which is what a
 * handle is for -- and it is the most obvious thing on the machine to
 * click, so it had better do something. */
bool sl_board_lever_at(const sl_layout_t *L, int x, int y);

void sl_board_init(void);
void sl_board_draw(const sl_layout_t *L, const sl_view_t *v);
void sl_board_draw_status(const sl_layout_t *L, const sl_view_t *v);

/* Which row payline `line` passes through on the left and right edges,
 * and where its marker goes. Two lines share each of rows 0 and 2, so
 * the second of each pair is offset sideways. */
void sl_mark_pos(const sl_layout_t *L, int line, bool right, int *x, int *y);

/* Just the reels -- what a spin frame calls. Five opaque blits each,
 * covering every pixel exactly once. */
void sl_board_draw_reels(const sl_layout_t *L, const sl_view_t *v);

char *sl_num(int32_t n, char *buf);

#endif
