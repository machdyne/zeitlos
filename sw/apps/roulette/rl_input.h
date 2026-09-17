#ifndef RL_INPUT_H
#define RL_INPUT_H

/*
 * Zeitlos roulette -- typed commands and mouse clicks.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Both routes end at rl_bet_place(), with a bet rl_bet_valid() has
 * already accepted. Same arrangement as sw/apps/poker/input.h: clicking
 * a square and typing its name are the same operation expressed
 * differently, and neither is a special case of the other.
 *
 * -- what the keyboard can reach that the mouse cannot, and vice versa
 *
 * Splits and corners are natural with a mouse -- you put the chip on
 * the line -- and awkward to type, because they have no names. So they
 * are typed by their MEMBERS (`split 17 20`, `corner 17`) and the
 * command looks up which selector covers exactly those numbers. That
 * keeps every bet on the table reachable both ways, which matters on a
 * board with no mouse attached.
 *
 * The reverse case is the wheel type and the bank: there is nowhere
 * sensible to click for "switch to an american wheel", so those are
 * commands only.
 */

#include "rl_board.h"

typedef enum {
    RI_NONE = 0,
    /* Only the message and command lines changed -- two rows of text.
     *
     * Typing returned RI_REDRAW at first, so every keystroke repainted
     * the whole table: the wheel, 37 pockets, the grid, every chip. On
     * the device that is a visible flash per character. Nothing was
     * wrong with the picture, which is why no test noticed; it was the
     * cost of getting there. */
    RI_STATUS,
    RI_REDRAW,
    RI_SPIN,
    RI_NEWGAME,       /* re-seat: wheel type changed */
    RI_BUYIN,
    RI_GAME_MODE,
    RI_QUIT
} ri_action_t;

ri_action_t rl_input_command(rl_view_t *v, const char *line);
ri_action_t rl_input_key(rl_view_t *v, uint32_t keysym);
ri_action_t rl_input_click(rl_view_t *v, const rl_layout_t *L,
    int x, int y, bool remove);

const char *rl_input_help_line(int i);

/* True if the bank can cover `amount` on top of what is already on the
 * table. Exposed because both routes need the same answer and a game
 * that lets you stake chips you do not have is not a game. */
bool rl_can_stake(const rl_view_t *v, int32_t amount);

#endif
