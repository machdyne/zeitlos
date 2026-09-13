#ifndef CHESS_INPUT_H
#define CHESS_INPUT_H

/*
 * Zeitlos chess -- typed commands and mouse clicks.
 *
 * -- both, and neither one bolted on --
 *
 * The app has to be fully playable with no mouse at all, because
 * plenty of Zeitlos boards are run from a keyboard over a serial
 * console or with only a USB keyboard attached, and it has to be
 * playable with a mouse, because clicking a piece is what anybody
 * expects to be able to do.
 *
 * So the two paths meet at the same place: both of them end up
 * calling game_play() with a move that came out of ce_gen_legal().
 * Clicking a piece and typing its move are the same operation
 * expressed differently, and neither is a special case of the other.
 *
 * -- why the command line accepts so many spellings --
 *
 * ce_parse_move() takes coordinates, SAN, long algebraic, and the
 * sloppy versions of each. That is not indulgence: the app cannot
 * know whether the person in front of it thinks in "e2e4" or "e4" or
 * "Pe2-e4", and being told "unknown command" for a move that is
 * perfectly clear is the fastest way to make a text interface feel
 * hostile.
 */

#include "board_ui.h"
#include "game.h"

typedef enum {
	CI_NONE = 0,     /* nothing happened */
	CI_REDRAW,       /* state changed, repaint */
	CI_MOVED,        /* a move was played -- the engine may owe a reply */
	CI_NEWGAME,
	CI_GAME_MODE,    /* toggle full screen */
	CI_BENCH,
	CI_QUIT,
} ci_action_t;

/* Runs one command line. Anything that is a legal move is played;
 * anything else is looked up in the command table. */
ci_action_t input_command(chess_game_t *g, const char *line);

/* A click at absolute screen coordinates. Outside the board it does
 * nothing, which is deliberate -- a click on the panel that cleared
 * the selection would be a trap. */
ci_action_t input_click(chess_game_t *g, const cb_layout_t *L, int x, int y);

/* Editing of the command line. Returns CI_REDRAW for an edit, or the
 * result of running the line on Return. */
ci_action_t input_key(chess_game_t *g, uint32_t keysym);

/* The text `help` prints, one line at a time, so both the panel and
 * the serial console can show it. Returns NULL past the end. */
const char *input_help_line(int i);

#endif
