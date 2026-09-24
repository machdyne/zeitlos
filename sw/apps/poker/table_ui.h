#ifndef PK_TABLE_UI_H
#define PK_TABLE_UI_H

/*
 * Zeitlos poker -- the table, drawn.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- one renderer, two very different places to draw --
 *
 * Same arrangement as sw/apps/chess/board_ui.h, and for the same
 * reason. The app has to work inside a window AND full screen in game
 * mode (docs/game_mode.md), and those look nothing alike from the
 * inside: a window sits somewhere on a 640x480 desktop, is inset by
 * the titlebar, may be partly covered and may be resized at any
 * moment; a game-mode page is a fixed 320x240 at a known offset with
 * nothing in front of it.
 *
 * Everything here is therefore written against an ORIGIN and a CLIP
 * RECTANGLE in absolute screen coordinates. Windowed passes the
 * content rectangle; game mode passes the back page. That is the
 * whole difference.
 *
 * The alternative -- window-relative helpers for one mode and raw
 * framebuffer writes for the other -- is two renderers that drift, and
 * the drift stays invisible until somebody switches modes mid-hand and
 * finds the table laid out differently on the two sides.
 *
 * -- why the layout is computed rather than constant --
 *
 * Because a window can be resized and somebody will resize it. The
 * layout is a pure function of the rectangle it is given, which also
 * makes it testable with no framebuffer at all: tests/test_layout.c
 * checks the relationships, tests/render.c draws it and writes a
 * picture out to be LOOKED at.
 *
 * Both matter. sw/common/tests/zrender.h's header explains what a
 * geometry assertion cannot catch, using sw/apps/logic's three
 * shipped-wrong panels as the worked example. For a card table most of
 * what matters is in that second category: whether a rank is legible
 * at twenty pixels, whether an overlapped fan reads as several cards,
 * whether a face-down card is instantly distinguishable from a face-up
 * one. None of those is an assertion and all of them are obvious in
 * one look.
 *
 * -- the seats are laid out in ROWS, not around an oval --
 *
 * An oval is what a poker table looks like and it was tried first. At
 * 320x240 it does not survive contact with seven-card stud: a seat
 * needs room for a name, a stack, a bet and up to seven cards, and
 * eight of those arranged on an ellipse overlap each other well before
 * they overlap the board in the middle.
 *
 * Rows also make the layout a pure function of the rectangle, which an
 * ellipse fitted to a resizable window is not.
 */

#include <stdint.h>
#include <stdbool.h>

#include "../../common/zgfx.h"
#include "pk_game.h"
#include "pk_ai.h"
#include "../../common/games/zcardart.h"

#define PT_LINE_H     10      /* an 8-pixel font line plus breathing room */
#define PT_BTN_H      12
#define PT_GAP        3
#define PT_SEAT_H     (PT_LINE_H + Z_ART_MINI_H + PT_LINE_H)

/* Below this the table is not drawn at all and a note is drawn
 * instead. Chess shrinks by dropping its panel; there is nothing here
 * that can be dropped and still leave a playable table, because every
 * part of it is a thing somebody has to be able to see to act. */
#define PT_MIN_W      232
#define PT_MIN_H      190

#define PT_MSG_LEN    48
#define PT_CMD_LEN    40
#define PT_TAG_LEN    14

/* -- buttons --------------------------------------------------------
 *
 * Drawn always and greyed when unavailable, rather than hidden. A
 * control that moves depending on what is legal is a control that gets
 * clicked by accident, and the shaded fill (zgfx.h's dither) is what
 * this display has instead of a disabled colour.
 */
#define PT_BTN_FOLD   0
#define PT_BTN_CALL   1       /* check, when there is nothing to call */
#define PT_BTN_RAISE  2       /* bet, when there is nothing to raise */
#define PT_BTN_ALLIN  3
#define PT_BTN_LESS   4
#define PT_BTN_MORE   5
#define PT_NBTN       6

typedef struct {
    int x, y, w, h;
} pt_rect_t;

typedef struct {

    z_clip_t clip;              /* absolute; nothing is drawn outside */
    int  ox, oy, w, h;          /* the content rectangle, absolute */

    bool ok;                    /* false when the rectangle is too small */

    int  status_y;

    int  nopp;                  /* seats drawn as opponents */
    int  opp_rows;
    int  opp_row_y[2];
    int  opp_row_n[2];
    int  opp_cell_w[2];

    bool has_board;
    int  board_x, board_y, board_slots;
    int  pot_y;

    int  hero_x, hero_y, hero_slots;
    int  hero_info_y;

    pt_rect_t btn[PT_NBTN];
    int  amount_x, amount_w;

    int  msg_y, cmd_y;

} pt_layout_t;

/* -- what to draw ---------------------------------------------------
 *
 * Everything the renderer needs that is not in pk_game_t: which seat
 * the person is sitting in, what they have typed, and the bits of
 * presentation state that have no business inside the rules.
 */
typedef struct {

    pk_game_t *g;
    int        hero;
    int        level;

    bool       discard[PK_MAX_HOLE];   /* five-card draw */
    int32_t    bet_to;                 /* the amount in the box */

    char       message[PT_MSG_LEN];
    char       cmd[PT_CMD_LEN];
    int        cmd_len;

    bool       thinking;
    int        thinking_seat;

    /* True between the last action and the next deal: every hand is
     * turned over. The engine decides whether there WAS a showdown
     * (pk_game_t::showdown); this decides whether the app is currently
     * displaying one. */
    bool       reveal;

    int        hand_no;

    /* The shared bank, and what was taken out of it to sit down.
     *
     * Poker is the only game here whose boundary is not a round: a
     * stack rises and falls across many hands, so the bank is settled
     * when the TABLE ends rather than when a hand does. Until then
     * `bank` is what is left in /user/casino.dat and `buyin` is what is
     * on the table -- the two together are what the player is worth. */
    int32_t    bank;
    int32_t    buyin;
    char       tag[PK_MAX_SEATS][PT_TAG_LEN];   /* "calls 20", "folds" */
    char       name[PK_MAX_SEATS][8];

    /* -- requests from the command line ------------------------------
     *
     * input.c cannot re-seat the table itself: changing the variant or
     * the number of seats means tearing down pk_game_t and building
     * another, and the app owns that. So it records what was asked for
     * and returns PI_NEWGAME, and poker.c acts on it.
     *
     * The alternative -- input.c calling pk_game_init() directly --
     * would have the input layer destroying the game the renderer is
     * about to draw from, which is only safe because of where the call
     * happens to sit in the loop. That is the kind of thing that stays
     * true until somebody moves a line. */
    const pk_variant_t *pending_variant;
    int        pending_seats;
    int        pending_limit;

    /* Set by the `odds` command, cleared by the app once it has run
     * the estimate. Not computed here: it takes a poll callback and a
     * message pump, which belong to the app. */
    bool       want_odds;

} pt_view_t;

/* Pure. No drawing, no globals, no framebuffer. */
void pt_layout(pt_layout_t *L, const pt_view_t *v, int ox, int oy,
    int w, int h);

/* Where a hero card sits, and which one a point is over (-1 if none).
 * Both honour the layout's own card stride, so a click lands on the
 * card that was drawn rather than on where it would have been. */
void pt_hero_card_xy(const pt_layout_t *L, int i, int *x, int *y);
int  pt_hero_card_at(const pt_layout_t *L, const pt_view_t *v, int x, int y);

/* Which button a point is over, or -1. */
int  pt_button_at(const pt_layout_t *L, int x, int y);

/* One-time probe of what this bitstream's blitter can do. Call before
 * any drawing; safe to call more than once. */
void pt_init(void);

/* -- drawing --------------------------------------------------------- */

void pt_draw_all(const pt_layout_t *L, const pt_view_t *v);

/* The two bottom lines alone, for a keystroke that changed nothing
 * else. Text on a cleared strip, so it cannot tear in any way a person
 * would see, which is what lets it be drawn straight into the visible
 * page. */
void pt_draw_status(const pt_layout_t *L, const pt_view_t *v);

/* The action row alone, for the amount box changing under -/+. */
void pt_draw_actions(const pt_layout_t *L, const pt_view_t *v);

/* Unsigned decimal into `buf`, which must hold at least 12 bytes.
 * Exposed because the app formats the same numbers for the message
 * line, and because printf with a conversion specifier costs about
 * 100KB here (docs/app_runtime.md). */
char *pt_num(int32_t n, char *buf);

#endif
