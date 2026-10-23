#ifndef BJ_BOARD_H
#define BJ_BOARD_H

/*
 * Zeitlos blackjack -- the table, drawn and clicked.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Same origin-and-clip contract as sw/apps/poker/table_ui.h and
 * sw/apps/roulette/rl_board.h: everything is drawn against an ORIGIN
 * and a CLIP RECTANGLE, so the desktop window's content rect and the
 * screen face's 320x240 (a dual app, docs/app_kinds.md) are the same
 * code with different arguments. Two renderers drift, and the drift stays invisible until
 * somebody switches modes mid-hand.
 *
 * -- one hand gets full cards, several get small ones --
 *
 * A single hand has the whole width and is drawn with the 20x28 tiles.
 * Split into two, three or four and each gets a quarter of the table,
 * which is not enough for full cards, so those hands use the 12x17
 * minis -- the same ones sw/apps/poker fans an opponent's stud hand
 * with, where the rank and suit both live in columns 2 to 6 precisely
 * so that an overlapped card is still readable.
 *
 * THE STRIDE IS COMPUTED, not constant. A blackjack hand has no fixed
 * length: twelve cards is possible (four aces, four twos, four threes)
 * and five or six is common after a couple of hits. A constant stride
 * that suits two cards runs off the table at seven, and one that suits
 * twelve wastes most of the width in the usual case. So the stride is
 * whatever makes the hand fit its cell, floored at the point where the
 * rank would start to be hidden.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zgfx.h"
#include "../../common/games/zcardart.h"
#include "../../common/games/zarcade.h"
#include "bj_game.h"
#include "bj_hint.h"

#define BJ_MSG_LEN   48
#define BJ_CMD_LEN   40
#define BJ_LINE_H    10
#define BJ_BTN_H     12

#define BJ_MIN_W     250
#define BJ_MIN_H     190

#define BJ_NCHIPS 4
extern const int32_t bj_chip_values[BJ_NCHIPS];

#define BJ_BTN_HIT        0
#define BJ_BTN_STAND      1
#define BJ_BTN_DOUBLE     2
#define BJ_BTN_SPLIT      3
#define BJ_BTN_SURRENDER  4
#define BJ_BTN_DEAL       5
#define BJ_NBTN           6

typedef struct { int x, y, w, h; } bj_rect_t;

typedef struct {

    z_clip_t clip;
    int  ox, oy, w, h;
    bool ok;

    int  status_y;

    int  dealer_x, dealer_y;     /* top-left of the dealer's cards */
    int  dealer_info_y;

    int  hands_y;                /* top of the player's hand row */
    int  hand_w;                 /* width of one hand's cell */
    int  hand_info_y;

    int  shoe_x, shoe_y, shoe_w; /* the penetration bar */

    bj_rect_t btn[BJ_NBTN];
    bj_rect_t chip[BJ_NCHIPS];

    int  msg_y, cmd_y;

} bj_layout_t;

typedef struct {

    bj_game_t *g;

    int32_t  chips;
    int      chip_sel;
    int32_t  bet;            /* what the next round will be dealt for */

    bool     trng;
    bool     hints;          /* show basic strategy */
    char     message[BJ_MSG_LEN];
    char     cmd[BJ_CMD_LEN];
    int      cmd_len;

} bj_view_t;

void bj_board_layout(bj_layout_t *L, const bj_view_t *v, int ox, int oy,
    int w, int h);

/* Where hand `i`'s cards start, and the stride between them. `mini`
 * says which tile size is in use. */
void bj_hand_geom(const bj_layout_t *L, const bj_view_t *v, int i,
    int *x, int *y, int *stride, bool *mini);

int  bj_board_btn_at(const bj_layout_t *L, int x, int y);
int  bj_board_chip_at(const bj_layout_t *L, int x, int y);

void bj_board_init(void);
void bj_board_draw(const bj_layout_t *L, const bj_view_t *v);
void bj_board_draw_status(const bj_layout_t *L, const bj_view_t *v);

char *bj_num(int32_t n, char *buf);

/* -- colour (docs/blackjack.md, "In colour") --
 *
 * Every colour the table asks for is a ROLE: the desktop draws INK,
 * ACT, FRAME and SHOE lit and the rest dark (exactly the one-bit
 * table), the screen face -- the table on the arcade runtime's 320x240
 * screen -- in bj_palette. bj_screen(true) sends every primitive to
 * ar_*(); the layout is then bj_board_layout(L, v, 0, 0, 320, 240). */
enum {
    BJ_C_BG = 0, BJ_C_INK, BJ_C_BLACK, BJ_C_RED, BJ_C_BACK, BJ_C_ACT,
    BJ_C_BTN, BJ_C_DIM, BJ_C_FRAME, BJ_C_SHOE,
    BJ_C_CHIP5, BJ_C_CHIP25, BJ_C_CHIP100, BJ_C_CHIP500
};
extern const uint16_t bj_palette[16];
void bj_screen(bool on);
void bj_mono_inks(ar_ink_t ink[16]);

#endif
