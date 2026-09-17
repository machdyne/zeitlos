#ifndef PK_INPUT_H
#define PK_INPUT_H

/*
 * Zeitlos poker -- typed commands and mouse clicks.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * -- both, and neither one bolted on --
 *
 * The app has to be fully playable with no mouse at all, because
 * plenty of Zeitlos boards run from a keyboard alone, and it has to be
 * playable with a mouse, because clicking a button is what anybody
 * expects to be able to do.
 *
 * So the two paths meet at the same place: both end up calling
 * pk_act() with an action pk_legal() has already accepted. Clicking
 * `fold` and typing `fold` are the same operation expressed
 * differently, and neither is a special case of the other. Same
 * arrangement as sw/apps/chess/input.h, deliberately.
 *
 * -- why a command line rather than single-key actions --
 *
 * Single keys are tempting for a game with four or five things you
 * can do. They collide immediately: `c` has to mean "call" and also
 * has to be the first letter of a `cards` or `chips` command, and
 * resolving that by "act on the key unless the line is non-empty"
 * makes typing `call` fold you on the `c`.
 *
 * So everything goes through the line and Return runs it, exactly as
 * chess does. Single letters ARE commands -- `f`, `c`, `r 60` -- so it
 * is two keystrokes per action, not five.
 *
 * ONE exception, and it is the one that is safe to make: Return or
 * Space on an EMPTY line CHECKS, when checking is legal. Checking
 * costs nothing and cannot be regretted, so it gets the one-key path.
 * Every action that costs chips needs a word or a click -- calling a
 * bet by accident is exactly the mistake worth designing out.
 */

#include "table_ui.h"

typedef enum {
    PI_NONE = 0,     /* nothing happened */
    /* Only the message and command lines changed. Typing returned
     * PI_REDRAW at first, so every keystroke repainted the whole
     * table -- cheaper here than at a roulette wheel, but the same
     * mistake. */
    PI_STATUS,
    PI_REDRAW,       /* state changed, repaint */
    PI_ACTED,        /* an action was taken; the hand may need to move on */
    PI_NEWHAND,      /* deal again */
    PI_NEWGAME,      /* re-seat everybody with fresh stacks */
    PI_GAME_MODE,    /* toggle full screen */
    PI_QUIT
} pi_action_t;

/* Runs one command line. */
pi_action_t input_command(pt_view_t *v, const char *line);

/* A click at absolute screen coordinates. */
pi_action_t input_click(pt_view_t *v, const pt_layout_t *L, int x, int y);

/* Editing of the command line. Returns PI_REDRAW for an edit, or the
 * result of running the line on Return. */
pi_action_t input_key(pt_view_t *v, uint32_t keysym);

/* The text `help` prints, one line at a time, so both the message line
 * and the serial console can show it. NULL past the end. */
const char *input_help_line(int i);

/* Clamps v->bet_to into the range that is currently legal, and steps
 * it by `delta` bets first when delta is nonzero. Exposed because the
 * -/+ buttons and the -/+ keys are the same operation. */
void input_bet_step(pt_view_t *v, int delta);

#endif
