#ifndef CHESS_GAME_H
#define CHESS_GAME_H

/*
 * Zeitlos chess -- the state one game consists of, shared between the
 * app (chess.c), the board renderer (board_ui.c) and the input layer
 * (input.c).
 *
 * -- why the game is stored as a move list --
 *
 * Not as a stack of ce_undo_t records, which is what an engine would
 * do. Taking a move back replays the game from the start instead.
 *
 * Two reasons. An undo stack is 16 bytes a ply against a move's 4, and
 * this app already spends 24KB on a transposition table out of a
 * process allocation that comes from a 1MB system-wide pool
 * (sw/os/mem.h). And a move list is the thing the app needs ANYWAY --
 * to draw the move list panel, to save a game, to work out whether a
 * position has repeated. Deriving the undo information from it costs a
 * few hundred make() calls, which at this depth is under a
 * millisecond and happens only when somebody presses a key.
 *
 * The cost is that the app must be able to reconstruct any earlier
 * position from `start_fen` plus the first n moves, which is
 * game_seek() below. That constraint is worth having: it is also
 * exactly what "load a saved game" needs.
 */

#include "ce_core.h"
#include "ce_search.h"

#define CG_MAX_PLIES 400      /* 200 moves; longer games truncate the
                               * list rather than the game */
#define CG_MSG_LEN   48
#define CG_CMD_LEN   40

/* Who the computer plays. */
#define CG_MODE_HUMAN_WHITE 0
#define CG_MODE_HUMAN_BLACK 1
#define CG_MODE_TWO_HUMANS  2
#define CG_MODE_TWO_ENGINES 3

typedef struct {

	ce_pos_t    pos;

	char        start_fen[96];
	uint32_t    move[CG_MAX_PLIES];
	uint32_t    key[CG_MAX_PLIES + 1];   /* key[i] is BEFORE move[i] */
	char        san[CG_MAX_PLIES][10];
	int         nply;
	int         start_fullmove;
	int         start_side;

	int         level;
	int         mode;
	bool        flipped;          /* black at the bottom */
	bool        show_hints;       /* mark legal destinations on click */

	ce_result_t result;

	/* -- interaction -- */
	uint8_t     sel;              /* selected square, or CE_SQ_NONE */
	uint32_t    sel_moves[CE_MAX_MOVES];
	int         sel_n;
	uint8_t     last_from, last_to;

	bool        thinking;
	int         think_depth;
	uint32_t    think_nodes;

	char        message[CG_MSG_LEN];
	char        cmd[CG_CMD_LEN];
	int         cmd_len;

	/* Set by the poll callback when the person asks for the search to
	 * stop, read by the app once ce_search_go() returns. */
	bool        cancel_search;

} chess_game_t;

/* -- lifecycle ------------------------------------------------------ */

void game_new(chess_game_t *g, const char *fen);

/* Plays `mv`, which must be legal. Records it and updates the result. */
bool game_play(chess_game_t *g, uint32_t mv);

/* Rewinds to `ply` plies played, by replaying from the start. */
void game_seek(chess_game_t *g, int ply);

/* Takes back one move, or one full move (both sides) when the
 * computer is playing the other colour -- taking back only the
 * computer's reply would leave it to move again and it would simply
 * play the same thing. */
void game_undo(chess_game_t *g);

bool game_engine_to_move(const chess_game_t *g);

void game_set_message(chess_game_t *g, const char *s);

#endif
