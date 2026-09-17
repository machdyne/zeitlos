#ifndef BJ_INPUT_H
#define BJ_INPUT_H

/*
 * Zeitlos blackjack -- typed commands and mouse clicks.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Both routes end at bj_act(), with an action bj_options() has already
 * accepted. Same arrangement as the other two games: clicking `hit` and
 * typing it are the same operation, and neither is a special case.
 *
 * Blackjack is the one game here where single keys are unambiguous --
 * h, s, d, p are the standard shorthand and none of them is a prefix of
 * a longer command that means something else. They still go through the
 * line, because a person who has started typing `deck 6` should not
 * have their `d` taken as a double.
 */

#include "bj_board.h"

typedef enum {
    BI_NONE = 0,
    /* Only the message and command lines changed. Typing returned
     * BI_REDRAW at first, so every keystroke repainted the whole
     * table. */
    BI_STATUS,
    BI_REDRAW,
    BI_ACTED,
    BI_DEAL,
    BI_NEWGAME,     /* rules changed: rebuild the shoe */
    BI_BUYIN,
    BI_GAME_MODE,
    BI_QUIT
} bi_action_t;

bi_action_t bj_input_command(bj_view_t *v, const char *line);
bi_action_t bj_input_key(bj_view_t *v, uint32_t keysym);
bi_action_t bj_input_click(bj_view_t *v, const bj_layout_t *L, int x, int y);

const char *bj_input_help_line(int i);

#endif
