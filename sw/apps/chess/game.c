/*
 * Zeitlos chess -- the game, as distinct from the position.
 * See game.h for why it is stored as a move list.
 */

#include <string.h>
#include <stdio.h>

#include "game.h"

void game_set_message(chess_game_t *g, const char *s) {
	snprintf(g->message, sizeof(g->message), "%s", s ? s : "");
}

void game_new(chess_game_t *g, const char *fen) {

	int level = g->level, mode = g->mode;
	bool flipped = g->flipped, hints = g->show_hints;

	if (!fen) fen = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR "
		"w KQkq - 0 1";

	/* Settings survive a new game; everything else does not. Keeping
	 * the level across "new" is the difference between setting the
	 * difficulty once and setting it before every single game. */
	memset(g, 0, sizeof(*g));
	g->level = level ? level : 3;
	g->mode = mode;
	g->flipped = flipped;
	g->show_hints = hints;

	snprintf(g->start_fen, sizeof(g->start_fen), "%s", fen);

	if (!ce_set_fen(&g->pos, g->start_fen)) {
		ce_start_position(&g->pos);
		snprintf(g->start_fen, sizeof(g->start_fen),
			"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
	}

	g->start_fullmove = g->pos.fullmove;
	g->start_side = g->pos.side;
	g->key[0] = g->pos.key;
	g->nply = 0;
	g->sel = CE_SQ_NONE;
	g->last_from = CE_SQ_NONE;
	g->last_to = CE_SQ_NONE;
	g->result = ce_result(&g->pos, g->key, 1);

	ce_search_new_game();

}

bool game_play(chess_game_t *g, uint32_t mv) {

	ce_undo_t u;
	char san[10];

	if (g->nply >= CG_MAX_PLIES) {
		game_set_message(g, "move list full");
		return false;
	}

	/* SAN is generated BEFORE the move is made, because that is the
	 * position it describes -- the disambiguation depends on what
	 * else could have gone to that square, which is only true
	 * beforehand. */
	ce_move_to_san(&g->pos, mv, san);

	if (!ce_make(&g->pos, mv, &u)) return false;

	g->move[g->nply] = mv;
	memcpy(g->san[g->nply], san, sizeof(g->san[0]));
	g->nply++;
	g->key[g->nply] = g->pos.key;

	g->last_from = CE_MOVE_FROM(mv);
	g->last_to = CE_MOVE_TO(mv);
	g->sel = CE_SQ_NONE;
	g->sel_n = 0;

	g->result = ce_result(&g->pos, g->key, g->nply + 1);

	return true;

}

void game_seek(chess_game_t *g, int ply) {

	int i;

	if (ply < 0) ply = 0;
	if (ply > g->nply) ply = g->nply;

	ce_set_fen(&g->pos, g->start_fen);
	g->key[0] = g->pos.key;

	for (i = 0; i < ply; i++) {
		ce_undo_t u;
		/* The stored moves came out of ce_gen_legal() for the
		 * position they were played in, and this replays them in the
		 * same order from the same start, so they are legal by
		 * construction. Checking anyway costs nothing per move and
		 * turns a corrupted list into a short game rather than a
		 * board with two white kings on it. */
		if (!ce_make(&g->pos, g->move[i], &u)) break;
		g->key[i + 1] = g->pos.key;
	}

	g->nply = i;

	if (i > 0) {
		g->last_from = CE_MOVE_FROM(g->move[i - 1]);
		g->last_to = CE_MOVE_TO(g->move[i - 1]);
	} else {
		g->last_from = CE_SQ_NONE;
		g->last_to = CE_SQ_NONE;
	}

	g->sel = CE_SQ_NONE;
	g->sel_n = 0;
	g->result = ce_result(&g->pos, g->key, g->nply + 1);

}

bool game_engine_to_move(const chess_game_t *g) {

	if (g->result != CE_RESULT_NONE) return false;

	switch (g->mode) {
	case CG_MODE_HUMAN_WHITE: return g->pos.side == CE_BLACK;
	case CG_MODE_HUMAN_BLACK: return g->pos.side == CE_WHITE;
	case CG_MODE_TWO_ENGINES: return true;
	default:                  return false;
	}

}

void game_undo(chess_game_t *g) {

	int target;

	if (g->nply == 0) {
		game_set_message(g, "nothing to take back");
		return;
	}

	target = g->nply - 1;

	/* Take back a FULL move when the computer has a colour. Undoing
	 * only its reply would hand the move straight back to it, and it
	 * would play the same thing again -- which looks like the undo
	 * did nothing at all. */
	if ((g->mode == CG_MODE_HUMAN_WHITE || g->mode == CG_MODE_HUMAN_BLACK) &&
		target > 0)
		target--;

	game_seek(g, target);
	game_set_message(g, "took it back");

}
