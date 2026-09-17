#ifndef CR_INPUT_H
#define CR_INPUT_H

/*
 * Zeitlos craps -- typed commands and mouse clicks.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Both routes end at cr_place(), with a bet cr_can_place() has already
 * accepted. Same arrangement as the other five.
 *
 * Craps has more bets than any other game here, so the command line
 * matters more: there are spots on the felt a mouse can reach faster,
 * and numbers a keyboard can reach faster, and neither is a subset of
 * the other.
 */

#include "cr_board.h"

typedef enum {
    CI_NONE = 0,
    CI_STATUS,      /* only the two text rows changed */
    CI_REDRAW,
    CI_ROLL,
    CI_BUYIN,
    CI_GAME_MODE,
    CI_QUIT
} ci_action_t;

ci_action_t cr_input_command(cr_view_t *v, const char *line);
ci_action_t cr_input_key(cr_view_t *v, uint32_t keysym);
ci_action_t cr_input_click(cr_view_t *v, const cr_layout_t *L, int x, int y);

const char *cr_input_help_line(int i);

/* True if the bank can cover `amount` on top of what is already on the
 * felt. Exposed because both routes need the same answer. */
bool cr_can_afford(const cr_view_t *v, int32_t amount);

#endif
