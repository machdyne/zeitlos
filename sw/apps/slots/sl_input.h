#ifndef SL_INPUT_H
#define SL_INPUT_H

/*
 * Zeitlos slots -- typed commands and mouse clicks.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Both routes end in the same action, as in the other three games.
 * There is less to do here than anywhere else -- a slot machine has one
 * verb -- so most of this is the stake, which is the only decision a
 * player actually makes.
 */

#include "sl_board.h"

typedef enum {
    SI_NONE = 0,
    SI_STATUS,      /* only the two text rows changed */
    SI_REDRAW,
    SI_SPIN,
    SI_BUYIN,
    SI_GAME_MODE,
    SI_QUIT
} si_action_t;

si_action_t sl_input_command(sl_view_t *v, const char *line);
si_action_t sl_input_key(sl_view_t *v, uint32_t keysym);
si_action_t sl_input_click(sl_view_t *v, const sl_layout_t *L, int x, int y);

const char *sl_input_help_line(int i);

/* What a spin costs at the current settings. */
int32_t sl_stake(const sl_view_t *v);

#endif
