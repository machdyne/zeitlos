/*
 * Zeitlos chess -- the search.
 * See ce_search.h for the shape and the reasoning behind it.
 */

#include <string.h>

#include "ce_search.h"
#include "ce_eval.h"
#include "ce_psq.h"
#include "ce_book.h"

/* How often the poll callback runs and the clock is read.
 *
 * 256 nodes. At the few thousand nodes per second this machine
 * manages that is about every 50ms, which is fast enough that a
 * window redraw during a search does not look stuck and slow enough
 * that the callback itself costs nothing measurable. Raising it makes
 * the app feel laggy while thinking; lowering it spends real time in
 * the message loop. */
#define CE_POLL_INTERVAL 256

/* -- transposition table -------------------------------------------- */

#define TT_SIZE (1u << CE_TT_BITS)
#define TT_MASK (TT_SIZE - 1u)

#define TT_EXACT 1
#define TT_ALPHA 2   /* score is an upper bound */
#define TT_BETA  3   /* score is a lower bound */

typedef struct {
	uint32_t key;
	int16_t  score;
	uint16_t move;
	int8_t   depth;
	uint8_t  flag;
} tt_entry_t;

/*
 * .bss, not the heap. An app's malloc() grows into its 16KB stack
 * allowance and would fail long before 24KB (docs/app_runtime.md,
 * "_sbrk()"), and a search that has to cope with its table not
 * existing is a search with two code paths.
 */
static tt_entry_t tt[TT_SIZE];

/* -- search state --------------------------------------------------- */

static uint32_t (*clock_ms)(void);
static bool (*poll_cb)(void *);
static void *poll_user;

static const uint32_t *game_hist;
static int game_hist_n;

static uint32_t rng = 0x2545F491u;

static uint32_t s_nodes;
static uint32_t s_start_ms;
static uint32_t s_max_nodes;
static uint32_t s_max_ms;
static bool s_abort;
static int s_seldepth;

static uint32_t killers[CE_MAX_PLY][2];

/* Indexed by [colour][piece type][destination square]. The obvious
 * [from][to] form would be 2*128*128 entries; this one is 2*7*64,
 * which is 1.7KB instead of 64KB and works about as well, because
 * what the heuristic is actually learning is "a knight going to f5 is
 * usually good here", not "the knight from g3 specifically". */
static int16_t history[2][7][64];

static uint32_t path_key[CE_MAX_PLY];

uint32_t ce_search_nodes(void) { return s_nodes; }

void ce_search_set_clock(uint32_t (*fn)(void)) { clock_ms = fn; }

void ce_search_set_poll(bool (*fn)(void *), void *user) {
	poll_cb = fn;
	poll_user = user;
}

void ce_search_set_history(const uint32_t *keys, int n) {
	game_hist = keys;
	game_hist_n = n;
}

void ce_search_seed(uint32_t seed) {
	/* Never let the state reach zero: xorshift is stuck there
	 * forever, and the symptom would be the engine playing the same
	 * "random" move every time at the easy levels. */
	rng = seed ? seed : 0x2545F491u;
}

static uint32_t rnd(void) {
	uint32_t x = rng;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	rng = x;
	return x;
}

void ce_search_new_game(void) {
	memset(tt, 0, sizeof(tt));
	memset(killers, 0, sizeof(killers));
	memset(history, 0, sizeof(history));
}

/* -- difficulty levels ---------------------------------------------- *
 *
 * The WALL CLOCK is the primary limit and the depth is whatever fits
 * inside it. That is the opposite of how an engine is usually
 * configured, and it is deliberate.
 *
 * A depth limit alone means a quiet position comes back instantly and
 * a sharp one with a dozen checks in it takes fifteen times as long
 * at the same nominal depth. The person sitting in front of the board
 * does not experience that as "this position is complicated", they
 * experience it as the program hanging at random. A time limit makes
 * every move at a given level take about the same time, which is what
 * makes the levels feel like difficulty settings rather than like
 * different amounts of waiting.
 *
 * It also means the app is portable across the boards Zeitlos runs
 * on without a table of per-board tuning: a faster board spends the
 * same two seconds and gets another ply out of them.
 *
 * The node caps are a backstop, not the limit -- they are what bounds
 * the search when no clock has been installed at all, which is the
 * case in the host tests, where a result that depended on how fast
 * the build machine is would not be a test.
 *
 * The depth ceilings are set well above what the time limit will
 * usually reach. They exist to stop a trivially simple endgame
 * searching to depth 40 and filling the transposition table with
 * entries nobody will use.
 */
void ce_level_limits(int level, ce_limits_t *out) {

	memset(out, 0, sizeof(*out));

	if (level < CE_LEVEL_MIN) level = CE_LEVEL_MIN;
	if (level > CE_LEVEL_MAX) level = CE_LEVEL_MAX;

	out->use_book = level >= 3;

	switch (level) {

	case 1:  /* hangs pieces, answers instantly. For a child, or for
	          * somebody who has never played before. */
		out->max_depth = 1;
		out->max_ms = 200;
		out->max_nodes = 1500;
		out->blunder_cp = 200;
		out->random_pct = 30;
		break;

	case 2:
		out->max_depth = 2;
		out->max_ms = 400;
		out->max_nodes = 4000;
		out->blunder_cp = 120;
		out->random_pct = 12;
		break;

	case 3:  /* sees one-move tactics, misses most two-movers */
		out->max_depth = 3;
		out->max_ms = 800;
		out->max_nodes = 12000;
		out->blunder_cp = 60;
		out->random_pct = 4;
		break;

	case 4:  /* the last level that blunders on purpose */
		out->max_depth = 5;
		out->max_ms = 1500;
		out->max_nodes = 40000;
		out->blunder_cp = 25;
		break;

	case 5:  /* about here it stops giving material away */
		out->max_depth = 6;
		out->max_ms = 3000;
		out->max_nodes = 100000;
		break;

	case 6:
		out->max_depth = 8;
		out->max_ms = 6000;
		out->max_nodes = 250000;
		break;

	case 7:
		out->max_depth = 10;
		out->max_ms = 12000;
		out->max_nodes = 600000;
		break;

	default: /* 8 -- as strong as the clock allows */
		out->max_depth = 14;
		out->max_ms = 30000;
		out->max_nodes = 1500000;
		break;
	}

}

const char *ce_level_name(int level) {
	static const char *names[CE_LEVEL_MAX + 1] = {
		"", "Beginner", "Casual", "Novice", "Club", "Steady",
		"Strong", "Hard", "Toughest"
	};
	if (level < CE_LEVEL_MIN || level > CE_LEVEL_MAX) return "?";
	return names[level];
}

/* -- helpers -------------------------------------------------------- */

static bool time_up(void) {

	if (s_max_nodes && s_nodes >= s_max_nodes) return true;
	if (s_max_ms && clock_ms) {
		uint32_t now = clock_ms();
		/* Unsigned subtraction, so a counter that wraps is still
		 * measured correctly for any interval shorter than the wrap
		 * period -- the same rule docs/game_mode.md states for the
		 * frame counter. */
		if ((uint32_t)(now - s_start_ms) >= s_max_ms) return true;
	}
	return false;

}

static void check_limits(void) {
	if ((s_nodes & (CE_POLL_INTERVAL - 1)) != 0) return;
	if (time_up()) { s_abort = true; return; }
	if (poll_cb && poll_cb(poll_user)) s_abort = true;
}

/* A repetition ANYWHERE -- one occurrence in the current search path
 * or in the game so far -- is treated as a draw inside the search,
 * rather than waiting for a third. That is not the rule of chess, but
 * it is the right thing for a search to do: if a line repeats once
 * the side that wants the draw can simply repeat again, so the value
 * of the line already IS the draw, and insisting on the third
 * occurrence means searching the same subtree twice to learn it. */
static bool is_draw(const ce_pos_t *p, int ply) {

	int i;

	if (p->halfmove >= 100) return true;
	if (ce_insufficient_material(p)) return true;

	/* Only positions since the last irreversible move can repeat, so
	 * the scan is bounded by the halfmove clock rather than by the
	 * whole game. */
	for (i = ply - 1; i >= 0 && i >= ply - (int)p->halfmove; i--)
		if (path_key[i] == p->key) return true;

	for (i = game_hist_n - 1; i >= 0; i--)
		if (game_hist[i] == p->key) return true;

	return false;

}

static int tt_score_to_table(int score, int ply) {
	if (score > CE_MATE_BOUND) return score + ply;
	if (score < -CE_MATE_BOUND) return score - ply;
	return score;
}

static int tt_score_from_table(int score, int ply) {
	if (score > CE_MATE_BOUND) return score - ply;
	if (score < -CE_MATE_BOUND) return score + ply;
	return score;
}

static void tt_store(uint32_t key, int depth, int ply, int score,
	int flag, uint32_t move) {

	tt_entry_t *e = &tt[key & TT_MASK];

	/* Depth-preferred, except that an entry for a DIFFERENT position
	 * is always replaced. Keeping a deep entry for a position that is
	 * not on the board any more, in preference to a shallow one for
	 * the position actually being searched, is the classic way a
	 * transposition table stops helping. */
	if (e->key == key && e->depth > depth && e->flag == TT_EXACT) return;

	e->key = key;
	e->score = (int16_t)tt_score_to_table(score, ply);
	e->move = move ? CE_MOVE_PACK16(move) : 0;
	e->depth = (int8_t)depth;
	e->flag = (uint8_t)flag;

}

/* Turns a packed 16-bit table move back into a real move by finding
 * it in the move list. That lookup is not a cost saving gone wrong --
 * it is the VERIFICATION: a 32-bit key collides occasionally, and a
 * move from a colliding entry would otherwise be made on a board
 * where it is nonsense. Searching for it in the legal-ish move list
 * means a collision costs a wasted ordering hint and nothing else. */
static uint32_t tt_unpack(uint16_t packed, const uint32_t *moves, int n) {

	int i;

	if (!packed) return CE_MOVE_NONE;

	for (i = 0; i < n; i++)
		if (CE_MOVE_PACK16(moves[i]) == packed) return moves[i];

	return CE_MOVE_NONE;

}

/* -- move ordering -------------------------------------------------- *
 *
 * Ordering is where almost all of alpha-beta's benefit comes from. A
 * perfectly ordered tree costs the square root of an unordered one,
 * which at these depths is the difference between two seconds and two
 * minutes.
 */

static int move_score(const ce_pos_t *p, uint32_t mv, uint32_t ttmove,
	int ply) {

	uint8_t to = CE_MOVE_TO(mv);
	uint8_t from = CE_MOVE_FROM(mv);
	int attacker = CE_TYPE(p->board[from]);

	if (ttmove != CE_MOVE_NONE && CE_MOVE_SAME(mv, ttmove))
		return 1 << 28;

	if (CE_MOVE_FLAGS(mv) & CE_MF_PROMO)
		return (1 << 26) + CE_MOVE_PROMO(mv);

	if (CE_MOVE_FLAGS(mv) & CE_MF_CAPTURE) {
		/* Most valuable victim, least valuable attacker. En passant's
		 * victim is not on the destination square, so it is scored as
		 * the pawn capture it is rather than read off an empty
		 * square. */
		int victim = (CE_MOVE_FLAGS(mv) & CE_MF_EP)
			? CE_PAWN : CE_TYPE(p->board[to]);
		return (1 << 24) + victim * 16 - attacker;
	}

	if (killers[ply][0] != CE_MOVE_NONE && CE_MOVE_SAME(mv, killers[ply][0]))
		return 1 << 23;
	if (killers[ply][1] != CE_MOVE_NONE && CE_MOVE_SAME(mv, killers[ply][1]))
		return (1 << 23) - 1;

	return history[p->side][attacker][CE_SQ64(to)];

}

/* Selection sort, one pick per iteration, rather than sorting the
 * whole list up front. Most nodes fail high on the first or second
 * move, so sorting the remaining thirty is work thrown away. */
static void pick_move(uint32_t *moves, int *scores, int n, int i) {

	int j, best = i;

	for (j = i + 1; j < n; j++)
		if (scores[j] > scores[best]) best = j;

	if (best != i) {
		uint32_t m = moves[i]; moves[i] = moves[best]; moves[best] = m;
		{ int s = scores[i]; scores[i] = scores[best]; scores[best] = s; }
	}

}

/* -- quiescence ----------------------------------------------------- */

static int quiesce(ce_pos_t *p, int alpha, int beta, int ply) {

	uint32_t moves[CE_MAX_MOVES];
	int scores[CE_MAX_MOVES];
	int n, i, stand, best;

	s_nodes++;
	check_limits();
	if (s_abort) return 0;

	if (ply > s_seldepth) s_seldepth = ply;

	/* The recursion is bounded by the ply array, not only by running
	 * out of captures: a position with a long series of recaptures
	 * plus promotions can go deeper than anybody expects, and running
	 * off the end of path_key[] would be a stack smash rather than a
	 * bad move. */
	if (ply >= CE_MAX_PLY - 1) return ce_eval(p);

	stand = ce_eval(p);
	best = stand;
	if (stand >= beta) return stand;
	if (stand > alpha) alpha = stand;

	/* Delta pruning. If even winning a queen outright would not drag
	 * this position up to alpha, no capture here is going to matter.
	 * Switched off once the position is materially thin, because in
	 * an endgame the margin is large relative to what is left and the
	 * pruning starts throwing away real lines. */
	if (p->npmat[CE_WHITE] + p->npmat[CE_BLACK] > 1300 &&
		stand + ce_val_mg[CE_QUEEN] + 200 < alpha)
		return alpha;

	n = ce_gen_captures(p, moves);

	for (i = 0; i < n; i++)
		scores[i] = move_score(p, moves[i], CE_MOVE_NONE, ply);

	for (i = 0; i < n; i++) {

		ce_undo_t u;
		int score;

		pick_move(moves, scores, n, i);

		if (!ce_make(p, moves[i], &u)) continue;

		score = -quiesce(p, -beta, -alpha, ply + 1);

		ce_unmake(p, &u);

		if (s_abort) return 0;

		if (score > best) {
			best = score;
			if (score >= beta) return score;
			if (score > alpha) alpha = score;
		}
	}

	return best;

}

/* -- main search ---------------------------------------------------- */

static int alphabeta(ce_pos_t *p, int depth, int alpha, int beta, int ply,
	bool allow_null) {

	uint32_t moves[CE_MAX_MOVES];
	int scores[CE_MAX_MOVES];
	uint32_t ttmove = CE_MOVE_NONE;
	uint32_t best_move = CE_MOVE_NONE;
	tt_entry_t *e;
	int n, i, legal = 0;
	int best_score = -CE_INFINITY;
	int alpha_orig = alpha;
	bool in_check;

	s_nodes++;
	check_limits();
	if (s_abort) return 0;

	/* The ply ceiling, enforced before anything indexes by ply.
	 *
	 * It is not decoration. The check extension below increases depth
	 * without bound along a forcing sequence, so a long series of
	 * checks can drive ply past CE_MAX_PLY -- at which point
	 * path_key[ply] and killers[ply] write off the end of their
	 * arrays into whatever follows them in .bss. The first version of
	 * this file had the guard in quiesce() only, and the symptom was
	 * not a crash: it was the engine playing weak moves in sharp
	 * positions, because what it was corrupting was its own killer
	 * table. */
	if (ply >= CE_MAX_PLY - 2) return ce_eval(p);

	if (ply > 0) {

		if (is_draw(p, ply)) return 0;

		/* Mate-distance pruning. If a mate has already been found
		 * closer to the root than anything this subtree could
		 * produce, the subtree cannot change the answer. Cheap, and
		 * it is what stops the engine announcing mate in 7 and then
		 * playing a move that mates in 9. */
		if (alpha < -CE_MATE + ply) alpha = -CE_MATE + ply;
		if (beta > CE_MATE - ply - 1) beta = CE_MATE - ply - 1;
		if (alpha >= beta) return alpha;
	}

	path_key[ply] = p->key;

	in_check = ce_in_check(p, p->side);

	/* Check extension. Searching a forced sequence to a fixed depth
	 * cuts it off in the middle, which is how an engine walks into a
	 * mate it could have seen. Applied before the depth <= 0 test so
	 * a check at the horizon is always resolved. */
	if (in_check && depth < CE_MAX_PLY - 2) depth++;

	if (depth <= 0) return quiesce(p, alpha, beta, ply);

	n = ce_gen_moves(p, moves);

	e = &tt[p->key & TT_MASK];
	if (e->key == p->key) {

		ttmove = tt_unpack(e->move, moves, n);

		if (e->depth >= depth && ply > 0) {
			int score = tt_score_from_table(e->score, ply);
			if (e->flag == TT_EXACT) return score;
			if (e->flag == TT_BETA && score >= beta) return score;
			if (e->flag == TT_ALPHA && score <= alpha) return score;
		}
	}

	/* Null-move pruning: give the opponent a free move, and if the
	 * position is still good enough to fail high, it was good enough
	 * without searching it properly.
	 *
	 * Three guards, all of them load-bearing:
	 *  - not in check, because passing while in check is not a legal
	 *    position to reason about at all
	 *  - the side to move has a piece other than pawns, because in a
	 *    king-and-pawn endgame zugzwang is common and the whole
	 *    premise of the null move ("a free move can only help") is
	 *    false there
	 *  - not twice in a row, or two null moves in sequence prove
	 *    nothing about either side
	 */
	if (allow_null && !in_check && depth >= 3 && ply > 0 &&
		p->npmat[p->side] > 0) {

		ce_undo_t u;
		int R = depth >= 6 ? 3 : 2;
		int score;

		ce_make_null(p, &u);
		score = -alphabeta(p, depth - 1 - R, -beta, -beta + 1, ply + 1,
			false);
		ce_unmake_null(p, &u);

		if (s_abort) return 0;
		if (score >= beta) return beta;
	}

	for (i = 0; i < n; i++)
		scores[i] = move_score(p, moves[i], ttmove, ply);

	for (i = 0; i < n; i++) {

		ce_undo_t u;
		int score;

		pick_move(moves, scores, n, i);

		if (!ce_make(p, moves[i], &u)) continue;
		legal++;

		score = -alphabeta(p, depth - 1, -beta, -alpha, ply + 1, true);

		ce_unmake(p, &u);

		if (s_abort) return 0;

		if (score > best_score) {

			best_score = score;
			best_move = moves[i];

			if (score >= beta) {

				/* Killers and history are updated only for QUIET
				 * moves. A capture that causes a cutoff is already
				 * ordered first by the capture score, so recording
				 * it would displace a genuinely useful quiet move
				 * from the killer slot for no gain. */
				if (!(CE_MOVE_FLAGS(moves[i]) & CE_MF_CAPTURE)) {
					if (!CE_MOVE_SAME(moves[i], killers[ply][0])) {
						killers[ply][1] = killers[ply][0];
						killers[ply][0] = moves[i];
					}
					{
						int t = CE_TYPE(p->board[CE_MOVE_FROM(moves[i])]);
						int sq = CE_SQ64(CE_MOVE_TO(moves[i]));
						int16_t *h = &history[p->side][t][sq];
						int v = *h + depth * depth;
						/* Saturate rather than wrap. A wrapped
						 * history value is a large NEGATIVE score,
						 * which sends a good quiet move to the back
						 * of the list -- visible as the engine
						 * getting worse the longer it thinks. */
						*h = (int16_t)(v > 16000 ? 16000 : v);
					}
				}

				tt_store(p->key, depth, ply, best_score, TT_BETA,
					moves[i]);
				return best_score;
			}

			if (score > alpha) alpha = score;
		}
	}

	if (legal == 0)
		return in_check ? -CE_MATE + ply : 0;

	/* Fail-soft, so the value returned is the best one actually seen
	 * rather than the window edge it was clamped to.
	 *
	 * This is not a refinement, it is load-bearing here. The ROOT
	 * needs a real score for every move, because that is what the
	 * blunder margin on the easy levels chooses among -- with
	 * fail-hard, every move that fails low records a score of exactly
	 * alpha, so all of them look tied with the best one and "pick
	 * something within 12 centipawns of best" degenerates into "pick
	 * anything at all". The symptom was the engine finding mate in
	 * one, scoring it correctly, and then playing a random pawn move.
	 *
	 * The flag says which kind of value it is: EXACT if some move
	 * raised alpha, otherwise an upper bound. */
	tt_store(p->key, depth, ply, best_score,
		best_score > alpha_orig ? TT_EXACT : TT_ALPHA, best_move);

	return best_score;

}

/* -- root ----------------------------------------------------------- */

typedef struct {
	uint32_t move;
	int      score;
} root_move_t;

void ce_search_go(ce_pos_t *p, const ce_limits_t *lim, ce_info_t *out) {

	root_move_t root[CE_MAX_MOVES];
	uint32_t moves[CE_MAX_MOVES];
	int nroot, i, depth;
	uint32_t best = CE_MOVE_NONE;
	int best_score = 0, best_depth = 0;

	memset(out, 0, sizeof(*out));

	s_nodes = 0;
	s_abort = false;
	s_seldepth = 0;
	s_max_nodes = lim->max_nodes;
	s_max_ms = lim->max_ms;
	s_start_ms = clock_ms ? clock_ms() : 0;

	nroot = ce_gen_legal(p, moves);
	if (nroot == 0) return;          /* mate or stalemate; caller's job */

	for (i = 0; i < nroot; i++) {
		root[i].move = moves[i];
		root[i].score = -CE_INFINITY;
	}

	out->best = root[0].move;

	/* The opening book, before any searching at all.
	 *
	 * Not for strength -- the book is tiny and the moves in it are
	 * ones the engine would mostly find anyway. It is for VARIETY: a
	 * deterministic engine plays the same opening every single game,
	 * and the second game against it feels like the first one again. */
	if (lim->use_book) {
		uint32_t bm = ce_book_move(p, rnd());
		if (bm != CE_MOVE_NONE) {
			for (i = 0; i < nroot; i++)
				if (CE_MOVE_SAME(bm, root[i].move)) {
					out->best = root[i].move;
					out->from_book = true;
					out->depth = 0;
					return;
				}
		}
	}

	/* Play a random legal move outright, at the levels that do that.
	 * Done here rather than after searching so the easy levels are
	 * also the FAST ones -- a beginner should not wait two seconds
	 * for a move that was going to be random anyway. */
	if (lim->random_pct > 0 && (int)(rnd() % 100u) < lim->random_pct) {
		out->best = root[rnd() % (uint32_t)nroot].move;
		out->depth = 0;
		out->nodes = 0;
		return;
	}

	/* A single legal move needs no search, and searching it wastes
	 * the person's time on a decision that has already been made. */
	if (nroot == 1) {
		out->best = root[0].move;
		out->depth = 0;
		return;
	}

	/* -- why the easy levels search the root differently --
	 *
	 * A normal root search narrows its window as it goes: once a move
	 * worth +5.4 has been found, the rest are searched against that,
	 * and any move that cannot beat it is cut off early. That is
	 * where most of alpha-beta's speed comes from, and the price is
	 * that a cut-off move gets no real score -- only "not better than
	 * the best one".
	 *
	 * "Not better than the best one" is exactly the information the
	 * blunder margin needs and cannot use: picking uniformly among
	 * moves within 12 centipawns of best requires knowing which moves
	 * those are, and a cut-off move's score can land anywhere up to
	 * the best score. With a narrowing window the engine would find
	 * mate in one, score it correctly, and then choose uniformly
	 * among thirty moves that all appear to be tied with it.
	 *
	 * So the levels that blunder on purpose search every root move
	 * with a full window and get exact scores. They are the shallow,
	 * cheap levels -- depth 5 and under, with node caps in the tens
	 * of thousands -- so paying two or three times the nodes at the
	 * root is affordable there and nowhere else. The strong levels,
	 * which never blunder and therefore never need the losing moves'
	 * scores, keep the fast narrowing search. */
	{
		bool exact_root = lim->blunder_cp > 0;

	for (depth = 1; depth <= lim->max_depth; depth++) {

		int alpha = -CE_INFINITY, beta = CE_INFINITY;
		int iter_best_idx = 0;
		root_move_t scored[CE_MAX_MOVES];

		for (i = 0; i < nroot; i++) {

			ce_undo_t u;
			int score;
			int window = exact_root ? -CE_INFINITY : alpha;

			scored[i] = root[i];
			scored[i].score = -CE_INFINITY;

			if (!ce_make(p, root[i].move, &u)) continue;

			path_key[0] = u.key;
			score = -alphabeta(p, depth - 1, -beta, -window, 1, true);

			ce_unmake(p, &u);

			if (s_abort) break;

			scored[i].score = score;
			if (score > alpha) {
				alpha = score;
				iter_best_idx = i;
			}
		}

		if (s_abort) break;

		/* The iteration completed, so its results supersede the
		 * previous one's entirely. */
		for (i = 0; i < nroot; i++) root[i] = scored[i];

		best = root[iter_best_idx].move;
		best_score = root[iter_best_idx].score;
		best_depth = depth;

		/* Sort by score so the next iteration tries the best move
		 * first. This is where most of iterative deepening's value
		 * is: the shallow searches are not wasted work, they are the
		 * move ordering for the deep one. */
		for (i = 1; i < nroot; i++) {
			root_move_t t = root[i];
			int j = i - 1;
			while (j >= 0 && root[j].score < t.score) {
				root[j + 1] = root[j];
				j--;
			}
			root[j + 1] = t;
		}

		/* A forced mate is the end of the search: nothing deeper can
		 * improve on it, and continuing means the person waits for
		 * an answer that will not change. */
		if (best_score > CE_MATE_BOUND || best_score < -CE_MATE_BOUND)
			break;

		/* Do not start an iteration there is no chance of finishing.
		 * Each one costs roughly three to five times the last, so
		 * having used more than a third of the budget is already
		 * enough to know. */
		if (s_max_ms && clock_ms &&
			(uint32_t)(clock_ms() - s_start_ms) * 3 >= s_max_ms)
			break;
	}
	}

	if (best == CE_MOVE_NONE) {
		/* Aborted during the very first iteration -- before anything
		 * was scored. Falling back to the first legal move is better
		 * than returning nothing: it is a legal move, and the
		 * alternative is an app with no move to play. */
		best = root[0].move;
		best_score = 0;
	}

	/* -- deliberate imperfection, for the easy levels --
	 *
	 * Applied to the SORTED root list, so the candidates are the
	 * moves that genuinely came within the margin, not an arbitrary
	 * subset. A move that loses a rook is never in that set unless
	 * every move loses a rook.
	 *
	 * Skipped entirely when a forced mate has been found. Missing a
	 * mate is a realistic thing for a weak player to do, but SEEING
	 * one and then playing something else reads as the program being
	 * broken rather than as the opponent being weak -- and the easy
	 * levels are shallow enough that anything they can see, they have
	 * earned. */
	if (lim->blunder_cp > 0 && best_score < CE_MATE_BOUND) {
		int nc = 0;
		for (i = 0; i < nroot; i++)
			if (root[i].score > -CE_INFINITY &&
				best_score - root[i].score <= lim->blunder_cp) nc++;
			else break;                /* the list is sorted */
		if (nc > 1) {
			int pick = (int)(rnd() % (uint32_t)nc);
			best = root[pick].move;
			best_score = root[pick].score;
		}
	}

	out->best = best;
	out->score = best_score;
	out->depth = best_depth;
	out->seldepth = s_seldepth;
	out->nodes = s_nodes;
	out->aborted = s_abort;
	out->ms = clock_ms ? (uint32_t)(clock_ms() - s_start_ms) : 0;

	if (best_score > CE_MATE_BOUND)
		out->mate_in = (CE_MATE - best_score + 1) / 2;
	else if (best_score < -CE_MATE_BOUND)
		out->mate_in = -((CE_MATE + best_score + 1) / 2);

}
