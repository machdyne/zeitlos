/*
 * Zeitlos chess -- evaluation, search and book. Runs on the HOST:
 *
 *   cd sw/apps/chess && make test
 *
 * -- what can actually be tested about a search --
 *
 * Not "does it play well". Strength is a statistical property of
 * thousands of games and no unit test is going to measure it. What
 * IS testable, and what breaks in practice, is narrower:
 *
 *   - the evaluation is symmetrical. Mirror a position, swap the
 *     colours, and the score must negate exactly. An asymmetry here
 *     means the engine plays White and Black differently for no
 *     reason, and it is invisible in a game.
 *   - forced mates are found, and the distance reported is right.
 *     Checked against a brute-force mate solver written in this file,
 *     which shares no code with ce_search.c.
 *   - it never returns an illegal move, under any limit, including
 *     when aborted mid-search.
 *   - aborting works, promptly, and still yields a playable move.
 *   - the difficulty levels are ordered: a strong level beats a weak
 *     one over a set of games.
 *   - every line in the opening book is legal.
 *
 * The mate solver is the interesting one. It is the same technique
 * tests/perft_test.c uses for move generation: a second, deliberately
 * naive implementation, so that a disagreement names which of the two
 * is wrong rather than leaving a number nobody can check.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "../ce_core.h"
#include "../ce_eval.h"
#include "../ce_search.h"
#include "../ce_book.h"

static int checks, fails;

static void okmsg(int cond, const char *what) {
	checks++;
	if (!cond) { fails++; printf("  FAIL: %s\n", what); }
}

static void eq_int(long got, long want, const char *what) {
	checks++;
	if (got != want) {
		fails++;
		printf("  FAIL: %s -- got %ld, want %ld\n", what, got, want);
	}
}

/* -- an independent brute-force mate solver ------------------------- *
 *
 * "Can the side to move force mate within n moves?" Answered by
 * definition rather than by search: a move works if, after it, EVERY
 * opponent reply allows mate in n-1. No alpha-beta, no evaluation, no
 * transposition table, no move ordering. It is exponential and it is
 * meant to be.
 */
static int forces_mate_in(ce_pos_t *p, int moves);

/* Can the side to move avoid being mated within `moves` moves? */
static bool can_avoid_mate(ce_pos_t *p, int moves) {

	uint32_t list[CE_MAX_MOVES];
	int n = ce_gen_legal(p, list);
	int i;

	if (n == 0) return false;          /* mated or stalemated; either
	                                    * way it cannot move */

	for (i = 0; i < n; i++) {
		ce_undo_t u;
		bool safe;
		ce_make(p, list[i], &u);
		safe = !forces_mate_in(p, moves);
		ce_unmake(p, &u);
		if (safe) return true;
	}

	return false;

}

static int forces_mate_in(ce_pos_t *p, int moves) {

	uint32_t list[CE_MAX_MOVES];
	int n, i, m;

	if (moves <= 0) return 0;

	n = ce_gen_legal(p, list);
	if (n == 0) return 0;

	for (m = 1; m <= moves; m++) {
		for (i = 0; i < n; i++) {
			ce_undo_t u;
			uint32_t reply[CE_MAX_MOVES];
			bool mate;
			ce_make(p, list[i], &u);
			if (ce_gen_legal(p, reply) == 0) {
				mate = ce_in_check(p, p->side);   /* mate, not stalemate */
				ce_unmake(p, &u);
				if (mate && m == 1) return 1;
				continue;
			}
			mate = !can_avoid_mate(p, m - 1);
			ce_unmake(p, &u);
			if (mate) return m;
		}
	}

	return 0;

}

/* -- evaluation symmetry -------------------------------------------- */

/* Builds the FEN of the colour-and-rank mirror of a position: every
 * piece moves to the square reflected through the middle of the board
 * and changes colour, and the side to move swaps. The mirrored
 * position is strategically identical with the colours reversed, so
 * ce_eval() must return the same number. */
static void mirror_fen(const ce_pos_t *p, char *out) {

	static const char wname[7] = { ' ', 'P', 'N', 'B', 'R', 'Q', 'K' };
	static const char bname[7] = { ' ', 'p', 'n', 'b', 'r', 'q', 'k' };
	int rank, file, n = 0;

	for (rank = 7; rank >= 0; rank--) {
		int run = 0;
		for (file = 0; file < 8; file++) {
			/* Output rank r shows source rank 7-r, colour flipped. */
			uint8_t pc = p->board[CE_SQ(file, 7 - rank)];
			if (pc == CE_EMPTY) { run++; continue; }
			if (run) { out[n++] = (char)('0' + run); run = 0; }
			out[n++] = CE_COLOR(pc) == CE_WHITE
				? bname[CE_TYPE(pc)] : wname[CE_TYPE(pc)];
		}
		if (run) out[n++] = (char)('0' + run);
		if (rank) out[n++] = '/';
	}

	out[n++] = ' ';
	out[n++] = p->side == CE_WHITE ? 'b' : 'w';
	/* Castling rights and en passant are dropped: a mirrored position
	 * with rights would need the rooks mirrored too, and ce_eval()
	 * does not look at either, so carrying them would only add a way
	 * for the test itself to be wrong. */
	out[n++] = ' '; out[n++] = '-';
	out[n++] = ' '; out[n++] = '-';
	out[n++] = ' '; out[n++] = '0';
	out[n++] = ' '; out[n++] = '1';
	out[n] = 0;

}

static void check_symmetry(const char *fen) {

	ce_pos_t a, b;
	char mfen[128];
	int sa, sb;

	if (!ce_set_fen(&a, fen)) { okmsg(0, "symmetry: bad FEN"); return; }

	mirror_fen(&a, mfen);
	if (!ce_set_fen(&b, mfen)) {
		printf("  FAIL: symmetry: mirrored FEN rejected: %s\n", mfen);
		checks++; fails++;
		return;
	}

	sa = ce_eval(&a);
	sb = ce_eval(&b);

	checks++;
	if (sa != sb) {
		fails++;
		printf("  FAIL: eval not symmetrical: %d vs %d\n", sa, sb);
		printf("        %s\n        %s\n", fen, mfen);
	}

}

/* -- self play ------------------------------------------------------ */

typedef struct {
	int white_wins, black_wins, draws;
	int illegal;
} match_t;

static void play_game(int white_level, int black_level, uint32_t seed,
	match_t *m) {

	ce_pos_t p;
	uint32_t hist[600];
	int hn = 0, ply;

	ce_start_position(&p);
	ce_search_new_game();
	ce_search_seed(seed);
	hist[hn++] = p.key;

	for (ply = 0; ply < 300; ply++) {

		ce_limits_t lim;
		ce_info_t info;
		ce_undo_t u;
		uint32_t legal[CE_MAX_MOVES];
		int n, i, found = 0;
		ce_result_t r;

		r = ce_result(&p, hist, hn);
		if (r != CE_RESULT_NONE) {
			if (r == CE_RESULT_CHECKMATE) {
				if (p.side == CE_BLACK) m->white_wins++;
				else m->black_wins++;
			} else {
				m->draws++;
			}
			return;
		}

		ce_level_limits(p.side == CE_WHITE ? white_level : black_level,
			&lim);
		ce_search_set_history(hist, hn);
		ce_search_go(&p, &lim, &info);

		/* The single most important assertion in this file. An engine
		 * that returns an illegal move corrupts the board and the
		 * game ends in something incomprehensible; everything else
		 * here is about quality, this one is about not being
		 * broken. */
		n = ce_gen_legal(&p, legal);
		for (i = 0; i < n; i++)
			if (CE_MOVE_SAME(legal[i], info.best)) { found = 1; break; }

		if (!found) {
			char c[8];
			ce_move_to_coord(info.best, c);
			printf("  FAIL: search returned an illegal move (%s) at "
				"ply %d\n", c, ply);
			m->illegal++;
			return;
		}

		ce_make(&p, info.best, &u);

		/* The history only needs to reach back to the last
		 * irreversible move, and resetting it there keeps the
		 * repetition scan short. */
		if (p.halfmove == 0) hn = 0;
		if (hn < 590) hist[hn++] = p.key;
	}

	m->draws++;     /* ran out of plies */

}

/* -- abort ---------------------------------------------------------- */

static int abort_after;
static int abort_calls;

static bool abort_poll(void *user) {
	(void)user;
	abort_calls++;
	return abort_calls >= abort_after;
}

int main(void) {

	ce_pos_t p;
	ce_limits_t lim;
	ce_info_t info;
	int i;

	ce_init();

	/* No clock: every limit in this file is a NODE limit, so the
	 * results do not depend on how fast the build machine is. */
	ce_search_set_clock(NULL);

	printf("search: opening book\n");

	ce_book_init();
	okmsg(ce_book_size() > 100, "book built a useful number of entries");

	/* Every token of every line must resolve to a legal move. A typo
	 * in ce_book.c would otherwise silently truncate that opening and
	 * nobody would notice until they wondered why the engine left the
	 * book on move three. */
	for (i = 0; i < ce_book_line_count(); i++) {

		const char *s = ce_book_line(i);
		int ply = 0;

		ce_start_position(&p);

		while (*s) {
			uint32_t mv;
			ce_undo_t u;
			bool amb;
			char tok[8];
			int k = 0;

			while (*s == ' ') s++;
			if (!*s) break;
			while (*s && *s != ' ' && k < 7) tok[k++] = *s++;
			tok[k] = 0;

			mv = ce_parse_move(&p, tok, &amb);
			checks++;
			if (mv == CE_MOVE_NONE) {
				fails++;
				printf("  FAIL: book line %d ply %d: '%s' is not legal\n",
					i, ply, tok);
				break;
			}
			ce_make(&p, mv, &u);
			ply++;
		}

		checks++;
		if (ply < 6) {
			fails++;
			printf("  FAIL: book line %d is only %d plies\n", i, ply);
		}
	}

	/* The book must offer more than one first move, or it is not
	 * doing the job it exists for. */
	{
		int distinct = 0;
		uint32_t seen[8];
		int j;
		ce_start_position(&p);
		for (i = 0; i < 200; i++) {
			uint32_t mv = ce_book_move(&p, (uint32_t)i * 2654435761u);
			int dup = 0;
			if (mv == CE_MOVE_NONE) continue;
			for (j = 0; j < distinct; j++)
				if (CE_MOVE_SAME(seen[j], mv)) dup = 1;
			if (!dup && distinct < 8) seen[distinct++] = mv;
		}
		okmsg(distinct >= 2, "book offers more than one first move");
	}

	printf("search: evaluation symmetry\n");

	check_symmetry("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w - - 0 1");
	check_symmetry("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w - - 0 1");
	check_symmetry("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
	check_symmetry("8/5k2/8/8/3PP3/8/5K2/8 w - - 0 1");
	check_symmetry("6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1");
	check_symmetry("2b1k3/8/8/8/8/8/8/2B1K3 w - - 0 1");
	check_symmetry("4k3/pp4pp/8/8/8/8/PP4PP/4K3 w - - 0 1");

	/* A position with an extra queen must evaluate as clearly
	 * winning, whichever side has it -- the cheapest possible check
	 * that the sign convention is right, and the one that would have
	 * caught the tempo term being added on the wrong side of the
	 * White/Black flip. */
	ce_set_fen(&p, "4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
	okmsg(ce_eval(&p) > 500, "white with a queen: good for white to move");
	ce_set_fen(&p, "4k3/8/8/8/8/8/8/3QK3 b - - 0 1");
	okmsg(ce_eval(&p) < -500, "white with a queen: bad for black to move");
	ce_set_fen(&p, "3qk3/8/8/8/8/8/8/4K3 b - - 0 1");
	okmsg(ce_eval(&p) > 500, "black with a queen: good for black to move");

	printf("search: forced mates\n");

	{
		/* Each of these is checked TWICE: the brute-force solver
		 * says what the mate distance is, and the search has to
		 * agree. Neither number is written down here, so neither can
		 * be wrong in a way the other shares. */
		static const char *mates[] = {
			"6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1",      /* back rank */
			"6k1/5ppp/8/8/8/8/5PPP/1R4K1 w - - 0 1",
			"k7/8/1K6/8/8/8/8/1Q6 w - - 0 1",            /* Q+K box */
			"7k/8/6K1/8/8/8/8/6R1 w - - 0 1",            /* R+K */
			"6k1/5ppp/8/8/8/8/5PPP/4R1K1 w - - 0 1",
			"8/8/8/8/8/2k5/1q6/K7 b - - 0 1",            /* black mates */
		};

		for (i = 0; i < (int)(sizeof(mates) / sizeof(mates[0])); i++) {

			int truth;
			char what[96];

			ce_set_fen(&p, mates[i]);
			truth = forces_mate_in(&p, 3);

			snprintf(what, sizeof(what),
				"position %d: brute force finds a mate", i);
			okmsg(truth > 0, what);
			if (truth <= 0) continue;

			ce_search_new_game();
			ce_search_set_history(NULL, 0);
			ce_level_limits(CE_LEVEL_MAX, &lim);
			lim.use_book = false;
			lim.max_depth = 2 * truth + 1;
			lim.max_ms = 0;
			ce_search_go(&p, &lim, &info);

			snprintf(what, sizeof(what),
				"position %d: search agrees mate in %d", i, truth);
			eq_int(info.mate_in, truth, what);

			/* And the move it returns must actually be the start of a
			 * mate that short -- a correct score with the wrong move
			 * attached is a real failure mode when the transposition
			 * table is involved. */
			{
				ce_undo_t u;
				int after;
				snprintf(what, sizeof(what),
					"position %d: returned move is legal", i);
				okmsg(ce_make(&p, info.best, &u), what);
				after = can_avoid_mate(&p, truth - 1) ? 0 : 1;
				ce_unmake(&p, &u);
				snprintf(what, sizeof(what),
					"position %d: returned move forces the mate", i);
				okmsg(after, what);
			}
		}
	}

	/* Not every position has a mate, and claiming one that is not
	 * there is worse than missing one. */
	ce_set_fen(&p, "4k3/8/8/8/8/8/8/4K2R w - - 0 1");
	ce_search_new_game();
	ce_level_limits(5, &lim);
	lim.use_book = false;
	ce_search_go(&p, &lim, &info);
	okmsg(info.mate_in >= 0, "no false mate claim against the engine");

	printf("search: aborting\n");

	ce_set_fen(&p, "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/"
		"R3K2R w KQkq - 0 1");
	ce_search_new_game();
	ce_level_limits(CE_LEVEL_MAX, &lim);
	lim.use_book = false;
	lim.max_ms = 0;
	abort_after = 3;
	abort_calls = 0;
	ce_search_set_poll(abort_poll, NULL);
	ce_search_go(&p, &lim, &info);
	ce_search_set_poll(NULL, NULL);

	okmsg(info.aborted, "poll callback aborted the search");
	{
		uint32_t legal[CE_MAX_MOVES];
		int n = ce_gen_legal(&p, legal), j, found = 0;
		for (j = 0; j < n; j++)
			if (CE_MOVE_SAME(legal[j], info.best)) found = 1;
		okmsg(found, "an aborted search still returns a legal move");
	}

	/* The node cap is a hard limit, not a suggestion. An engine that
	 * overshoots it by a lot is an engine that will one day sit there
	 * thinking while somebody waits. */
	ce_search_new_game();
	ce_level_limits(CE_LEVEL_MAX, &lim);
	lim.use_book = false;
	lim.max_nodes = 20000;
	lim.max_ms = 0;
	ce_search_go(&p, &lim, &info);
	okmsg(info.nodes <= 20000 + 512, "node cap respected");

	printf("search: legality over whole games\n");

	{
		match_t m;
		memset(&m, 0, sizeof(m));
		for (i = 0; i < 6; i++)
			play_game(3, 3, 0x1000u + (uint32_t)i * 7919u, &m);
		eq_int(m.illegal, 0, "no illegal move in six level-3 games");
		printf("        level 3 vs 3: +%d -%d =%d\n",
			m.white_wins, m.black_wins, m.draws);
	}

	printf("search: the levels are ordered\n");

	{
		/* Level 5 against level 1, colours alternating so the result
		 * is not a statement about the white advantage. Level 1 plays
		 * a random move 30% of the time, so this is not close; if it
		 * ever IS close, something in the difficulty mapping has
		 * broken. */
		match_t m;
		int strong_score = 0, games = 8;

		memset(&m, 0, sizeof(m));

		for (i = 0; i < games; i++) {
			match_t g;
			memset(&g, 0, sizeof(g));
			if (i & 1) {
				play_game(5, 1, 0x2000u + (uint32_t)i * 104729u, &g);
				strong_score += g.white_wins * 2 + g.draws;
			} else {
				play_game(1, 5, 0x2000u + (uint32_t)i * 104729u, &g);
				strong_score += g.black_wins * 2 + g.draws;
			}
			m.illegal += g.illegal;
		}

		eq_int(m.illegal, 0, "no illegal move in the level match");
		printf("        level 5 scored %d/%d\n", strong_score, games * 2);
		okmsg(strong_score >= games + games / 2,
			"level 5 comfortably beats level 1");
	}

	printf("search: %d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;

}
