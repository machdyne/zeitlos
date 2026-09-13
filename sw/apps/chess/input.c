/*
 * Zeitlos chess -- typed commands and mouse clicks.
 * See input.h for why there are two ways in and why they meet.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../common/zkbd.h"

#include "input.h"
#include "ce_search.h"
#include "ce_book.h"

/* -- small string helpers ------------------------------------------- *
 *
 * Written out rather than pulled from a library because there is no
 * strcasecmp in this libc and strtok_r would need state this does not
 * want. They are four lines each.
 */

static int ieq(const char *a, const char *b) {
	for (; *a && *b; a++, b++) {
		char ca = *a, cb = *b;
		if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
		if (ca != cb) return 0;
	}
	return *a == 0 && *b == 0;
}

/* Splits off the first word; returns a pointer to the rest. */
static const char *word(const char *s, char *out, int outlen) {

	int n = 0;

	while (*s == ' ' || *s == '\t') s++;
	while (*s && *s != ' ' && *s != '\t' && n < outlen - 1) out[n++] = *s++;
	out[n] = 0;
	while (*s == ' ' || *s == '\t') s++;

	return s;

}

/* -- help ----------------------------------------------------------- */

static const char *const help[] = {
	"Type a move: e4, Nf3, e2e4, O-O",
	"or click a piece then its square.",
	"",
	"new [white|black|two|demo]  start over",
	"level 1-8                   difficulty",
	"undo        take back a move",
	"flip        turn the board round",
	"hints       toggle move markers",
	"hint        suggest a move",
	"moves       list legal moves",
	"fen [...]   show or set a position",
	"game        full screen (Esc to leave)",
	"bench       time this board",
	"help, quit",
	NULL
};

const char *input_help_line(int i) {
	int n = (int)(sizeof(help) / sizeof(help[0])) - 1;
	return (i >= 0 && i < n) ? help[i] : NULL;
}

/* -- commands ------------------------------------------------------- */

static ci_action_t cmd_new(chess_game_t *g, const char *rest) {

	char w[16];

	word(rest, w, sizeof(w));

	if (ieq(w, "black"))      g->mode = CG_MODE_HUMAN_BLACK;
	else if (ieq(w, "white")) g->mode = CG_MODE_HUMAN_WHITE;
	else if (ieq(w, "two"))   g->mode = CG_MODE_TWO_HUMANS;
	else if (ieq(w, "demo"))  g->mode = CG_MODE_TWO_ENGINES;

	game_new(g, NULL);

	/* Playing Black means seeing the board from Black's side. Anybody
	 * who wants it the other way round can say so with `flip`, but
	 * defaulting to it saves them having to. */
	g->flipped = (g->mode == CG_MODE_HUMAN_BLACK);

	game_set_message(g, "new game");
	return CI_NEWGAME;

}

static ci_action_t cmd_level(chess_game_t *g, const char *rest) {

	char w[16];
	int n;

	word(rest, w, sizeof(w));
	n = atoi(w);

	if (n < CE_LEVEL_MIN || n > CE_LEVEL_MAX) {
		snprintf(g->message, sizeof(g->message), "level must be %d-%d",
			CE_LEVEL_MIN, CE_LEVEL_MAX);
		return CI_REDRAW;
	}

	g->level = n;
	snprintf(g->message, sizeof(g->message), "level %d: %s", n,
		ce_level_name(n));

	return CI_REDRAW;

}

static ci_action_t cmd_moves(chess_game_t *g) {

	uint32_t list[CE_MAX_MOVES];
	int n = ce_gen_legal(&g->pos, list);
	int i, used = 0;

	g->message[0] = 0;

	/* As many as fit on the message line, which is the honest thing
	 * to do: the alternative is a scrolling list, and a scrolling
	 * list in a one-line message area is worse than a truncated
	 * one. */
	for (i = 0; i < n; i++) {
		char san[10];
		int len;
		ce_move_to_san(&g->pos, list[i], san);
		len = (int)strlen(san);
		if (used + len + 1 >= (int)sizeof(g->message) - 4) {
			snprintf(g->message + used, sizeof(g->message) - used, "...");
			break;
		}
		snprintf(g->message + used, sizeof(g->message) - used, "%s%s",
			used ? " " : "", san);
		used += len + (used ? 1 : 0);
	}

	if (n == 0) game_set_message(g, "no legal moves");

	return CI_REDRAW;

}

static ci_action_t cmd_hint(chess_game_t *g) {

	ce_limits_t lim;
	ce_info_t info;
	char san[10];

	if (g->result != CE_RESULT_NONE) {
		game_set_message(g, "the game is over");
		return CI_REDRAW;
	}

	/* A hint is searched at a FIXED, quick setting rather than at the
	 * game's level. At level 8 a hint would take half a minute, and
	 * somebody asking for a hint wants an answer now; at level 1 it
	 * would suggest a blunder, which is not what the word means. */
	ce_level_limits(5, &lim);
	lim.blunder_cp = 0;
	lim.random_pct = 0;
	lim.max_ms = 1500;

	ce_search_set_history(g->key, g->nply + 1);
	/* The poll callback the app installs aborts on this flag, and it
	 * is left set by whatever stopped the last search. Clearing it
	 * here is what stops a hint returning instantly and empty because
	 * somebody pressed Escape two moves ago. */
	g->cancel_search = false;
	ce_search_go(&g->pos, &lim, &info);

	if (info.best == CE_MOVE_NONE) {
		game_set_message(g, "no suggestion");
		return CI_REDRAW;
	}

	ce_move_to_san(&g->pos, info.best, san);
	snprintf(g->message, sizeof(g->message), "try %s", san);

	/* Show it on the board as well as in words -- "try Nf3" is only
	 * useful to somebody who already reads notation. */
	g->sel = CE_MOVE_FROM(info.best);
	g->sel_moves[0] = info.best;
	g->sel_n = 1;

	return CI_REDRAW;

}

static ci_action_t cmd_fen(chess_game_t *g, const char *rest) {

	if (!*rest) {
		ce_get_fen(&g->pos, g->message, sizeof(g->message));
		return CI_REDRAW;
	}

	{
		ce_pos_t test;
		if (!ce_set_fen(&test, rest)) {
			game_set_message(g, "that FEN does not parse");
			return CI_REDRAW;
		}
	}

	game_new(g, rest);
	game_set_message(g, "position set");

	return CI_NEWGAME;

}

ci_action_t input_command(chess_game_t *g, const char *line) {

	char w[24];
	const char *rest;
	bool ambiguous = false;
	uint32_t mv;

	rest = word(line, w, sizeof(w));

	if (!w[0]) return CI_NONE;

	/* -- a move first, before any command --
	 *
	 * The order matters and it is this way round on purpose. Chess
	 * notation collides with plausible command names -- "b4" is a
	 * move, "d4" is a move -- and a person typing at a chess board is
	 * far more often making a move than issuing a command. Commands
	 * are words; moves are not. */
	if (g->result == CE_RESULT_NONE) {
		mv = ce_parse_move(&g->pos, line, &ambiguous);
		if (mv != CE_MOVE_NONE) {
			if (game_engine_to_move(g)) {
				game_set_message(g, "not your move");
				return CI_REDRAW;
			}
			if (!game_play(g, mv)) {
				game_set_message(g, "could not play that");
				return CI_REDRAW;
			}
			g->message[0] = 0;
			return CI_MOVED;
		}
		if (ambiguous) {
			game_set_message(g, "ambiguous -- say which piece");
			return CI_REDRAW;
		}
	}

	if (ieq(w, "new"))    return cmd_new(g, rest);
	if (ieq(w, "level"))  return cmd_level(g, rest);
	if (ieq(w, "moves"))  return cmd_moves(g);
	if (ieq(w, "hint"))   return cmd_hint(g);
	if (ieq(w, "fen"))    return cmd_fen(g, rest);

	if (ieq(w, "undo") || ieq(w, "takeback") || ieq(w, "back")) {
		game_undo(g);
		return CI_REDRAW;
	}

	if (ieq(w, "flip")) {
		g->flipped = !g->flipped;
		game_set_message(g, g->flipped ? "black at the bottom"
			: "white at the bottom");
		return CI_REDRAW;
	}

	if (ieq(w, "hints")) {
		g->show_hints = !g->show_hints;
		game_set_message(g, g->show_hints ? "move markers on"
			: "move markers off");
		return CI_REDRAW;
	}

	if (ieq(w, "game") || ieq(w, "full") || ieq(w, "fullscreen"))
		return CI_GAME_MODE;

	if (ieq(w, "bench")) return CI_BENCH;

	if (ieq(w, "quit") || ieq(w, "exit")) return CI_QUIT;

	if (ieq(w, "help") || ieq(w, "?")) {
		game_set_message(g, "see the panel; moves: e4, Nf3, e2e4");
		return CI_REDRAW;
	}

	/* The message says what was actually wrong. "Unknown command" is
	 * unhelpful when what the person typed was meant to be a move
	 * and simply is not legal in this position -- which is the common
	 * case by a wide margin. */
	snprintf(g->message, sizeof(g->message), "not a legal move or command");

	return CI_REDRAW;

}

/* -- mouse ---------------------------------------------------------- */

ci_action_t input_click(chess_game_t *g, const cb_layout_t *L, int x, int y) {

	uint8_t sq = cb_xy_square(L, x, y);
	uint8_t pc;
	int i;

	if (sq == CE_SQ_NONE) return CI_NONE;

	if (g->result != CE_RESULT_NONE) {
		game_set_message(g, "the game is over -- `new` to start again");
		return CI_REDRAW;
	}

	if (game_engine_to_move(g)) {
		game_set_message(g, "not your move");
		return CI_REDRAW;
	}

	/* Second click: is this square one of the selected piece's
	 * destinations? */
	if (g->sel != CE_SQ_NONE) {

		uint32_t chosen = CE_MOVE_NONE;
		int promos = 0;

		for (i = 0; i < g->sel_n; i++) {
			if (CE_MOVE_TO(g->sel_moves[i]) != sq) continue;
			/* Four moves share a promotion square. Taking the first
			 * is not arbitrary: ce_gen_moves() emits the queen first
			 * precisely so that anything picking one move per square
			 * gets the queen. */
			if (chosen == CE_MOVE_NONE) chosen = g->sel_moves[i];
			promos++;
		}

		if (chosen != CE_MOVE_NONE) {
			if (!game_play(g, chosen)) {
				game_set_message(g, "could not play that");
				return CI_REDRAW;
			}
			if (promos > 1)
				game_set_message(g, "promoted to a queen "
					"(type e8=N for a knight)");
			else
				g->message[0] = 0;
			return CI_MOVED;
		}

		/* Not a destination. Clicking the selected piece again puts
		 * it down; clicking another of your own pieces picks that one
		 * up instead, which is what a person expects and saves a
		 * click. */
		if (sq == g->sel) {
			g->sel = CE_SQ_NONE;
			g->sel_n = 0;
			return CI_REDRAW;
		}
	}

	pc = g->pos.board[sq];

	if (pc == CE_EMPTY || CE_COLOR(pc) != g->pos.side) {
		g->sel = CE_SQ_NONE;
		g->sel_n = 0;
		return CI_REDRAW;
	}

	/* Select. The destination list is the LEGAL moves of that piece,
	 * filtered from the full legal list -- so a pinned piece shows no
	 * destinations at all, which is a more useful thing to learn from
	 * than being allowed to pick it up and then refused. */
	{
		uint32_t all[CE_MAX_MOVES];
		int n = ce_gen_legal(&g->pos, all);

		g->sel_n = 0;
		for (i = 0; i < n; i++)
			if (CE_MOVE_FROM(all[i]) == sq && g->sel_n < CE_MAX_MOVES)
				g->sel_moves[g->sel_n++] = all[i];

		if (g->sel_n == 0) {
			g->sel = CE_SQ_NONE;
			game_set_message(g, "that piece has no legal move");
			return CI_REDRAW;
		}

		g->sel = sq;
		g->message[0] = 0;
	}

	return CI_REDRAW;

}

/* -- the command line ----------------------------------------------- */

ci_action_t input_key(chess_game_t *g, uint32_t keysym) {

	if (keysym == '\r' || keysym == '\n') {
		char line[CG_CMD_LEN];
		ci_action_t a;
		snprintf(line, sizeof(line), "%s", g->cmd);
		g->cmd[0] = 0;
		g->cmd_len = 0;
		a = input_command(g, line);
		return a == CI_NONE ? CI_REDRAW : a;
	}

	if (keysym == '\b' || keysym == 0x7f) {
		if (g->cmd_len > 0) g->cmd[--g->cmd_len] = 0;
		return CI_REDRAW;
	}

	if (keysym == 0x1b) {
		/* Escape clears the line AND puts down whatever piece is
		 * selected. One key that means "never mind" is better than
		 * two that each half-mean it. */
		g->cmd[0] = 0;
		g->cmd_len = 0;
		g->sel = CE_SQ_NONE;
		g->sel_n = 0;
		g->message[0] = 0;
		return CI_REDRAW;
	}

	if (keysym >= 0x20 && keysym < 0x7f) {
		if (g->cmd_len < CG_CMD_LEN - 1) {
			g->cmd[g->cmd_len++] = (char)keysym;
			g->cmd[g->cmd_len] = 0;
		}
		return CI_REDRAW;
	}

	return CI_NONE;

}
