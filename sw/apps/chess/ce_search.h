#ifndef CE_SEARCH_H
#define CE_SEARCH_H

/*
 * Zeitlos chess -- the search.
 *
 * -- the constraint that shaped this --
 *
 * This runs inside a windowed app, and an app that does not read its
 * message queue for eight seconds spends those eight seconds drawing
 * against a visible region that may already be wrong -- Z_WM_SET_CLIP
 * arrives as a message like any other, so a window dropped in front of
 * the board is painted straight over.
 *
 * It used to be worse than that: wm blocked waiting for a redraw
 * acknowledgement until it timed out, so the whole desktop froze along
 * with the app. wm no longer waits (docs/window_manager.md, "Content
 * z-order"), which makes the same mistake quiet and local rather than
 * loud and system-wide -- harder to diagnose, not better.
 *
 * So the search takes a POLL CALLBACK and calls it every few hundred
 * nodes. The app's callback pumps its message loop -- answering
 * redraws, clip changes and the key that cancels the search -- and
 * returns true to abort. Aborting is safe at any point: the
 * incomplete iteration is thrown away and the best move from the last
 * COMPLETED iteration is returned, which is the whole reason the
 * search is iteratively deepened rather than run once at full depth.
 *
 * The alternative -- a resumable search that returns to the caller
 * and is re-entered where it left off -- would mean an explicit stack
 * instead of the C one, and every local in the recursion becoming a
 * field in a frame struct. That is a large amount of machinery to buy
 * something the callback already buys.
 *
 * -- no threads, no allocation --
 *
 * There are none available and none wanted. The transposition table
 * is a fixed .bss array (see CE_TT_BITS): an app's heap is carved out
 * of its 16KB stack allowance (docs/app_runtime.md, "_sbrk()"), so
 * anything of size has to be static.
 */

#include "ce_core.h"

/* Mate is scored just under 32767 so it fits an int16_t in the
 * transposition table, with CE_MAX_PLY of headroom underneath for
 * "mate in n" to be distinguishable from "mate in n+1". Any score
 * above CE_MATE_BOUND is a forced mate. */
#define CE_MATE        30000
#define CE_MATE_BOUND  (CE_MATE - CE_MAX_PLY)
#define CE_INFINITY    31000

/* 2^11 entries x 12 bytes = 24KB of .bss.
 *
 * Sized down from the 32KB budget on purpose. Beyond a certain point
 * a bigger table buys depth the person has to wait for, and waiting
 * is the thing this app is least able to afford -- see the header
 * above. 24KB also leaves room for the app's own board, sprites and
 * move list inside a process allocation that has to come out of a 1MB
 * system-wide pool (sw/os/mem.h).
 *
 * Raise it here if a board turns out to have memory to spare; nothing
 * else needs to change. */
#define CE_TT_BITS 11

/* -- difficulty ----------------------------------------------------- */

#define CE_LEVEL_MIN 1
#define CE_LEVEL_MAX 8

typedef struct {

	int      max_depth;     /* iterative deepening ceiling */
	uint32_t max_nodes;     /* hard node cap, always set */
	uint32_t max_ms;        /* wall clock cap, 0 = none */

	/* -- how the weak levels are made weak --
	 *
	 * NOT by searching badly. A search crippled by a bad evaluation
	 * plays moves that are strange rather than moves that are weak,
	 * and losing to a computer that plays incomprehensibly is not
	 * fun. These two do it by choosing imperfectly from a correctly
	 * ordered list instead:
	 *
	 *   blunder_cp  pick uniformly among the root moves scoring
	 *               within this many centipawns of the best one. At
	 *               0 it always plays the best move it found.
	 *   random_pct  percentage chance of ignoring the search entirely
	 *               and playing a uniformly random legal move. This
	 *               is what makes the lowest level beatable by a
	 *               child; it hangs pieces, the way a beginner does.
	 *
	 * A shallow search with a wide blunder margin also happens to be
	 * fast, which is the second reason the easy levels are the right
	 * shape: a beginner gets an instant reply, and only somebody who
	 * asked for a strong opponent waits for one.
	 */
	int      blunder_cp;
	int      random_pct;

	bool     use_book;

} ce_limits_t;

void ce_level_limits(int level, ce_limits_t *out);
const char *ce_level_name(int level);

/* -- results -------------------------------------------------------- */

typedef struct {
	uint32_t best;
	int      score;        /* centipawns, side to move's point of view */
	int      depth;        /* deepest COMPLETED iteration */
	int      seldepth;
	uint32_t nodes;
	uint32_t ms;
	bool     aborted;      /* the poll callback or a limit stopped it */
	bool     from_book;
	int      mate_in;      /* 0 none, >0 we mate, <0 we get mated */
} ce_info_t;

/* -- hooks ---------------------------------------------------------- */

/* Milliseconds since anything, as long as it counts up. NULL (the
 * default) disables every wall-clock limit and leaves only the node
 * cap, which is what the host tests want: a test whose result depends
 * on how fast the build machine is is not a test. */
void ce_search_set_clock(uint32_t (*now_ms)(void));

/* Called every CE_POLL_INTERVAL nodes. Return true to abort. NULL to
 * disable. */
void ce_search_set_poll(bool (*poll)(void *user), void *user);

/* Positions already played, most recent last, INCLUDING the position
 * about to be searched. Used for repetition detection -- without it
 * the engine happily walks into a draw by repetition when it is
 * winning, and refuses one when it is losing. */
void ce_search_set_history(const uint32_t *keys, int n);

/* Seeds the choice among near-equal moves. Give it something that
 * differs per game (the uptime counter) or every game at a given
 * level opens identically. */
void ce_search_seed(uint32_t seed);

/* Wipes the transposition table, the killers and the history
 * heuristic. Call at the start of a new game: leaving a previous
 * game's entries in place is not incorrect (every hit is verified)
 * but it does waste the table. */
void ce_search_new_game(void);

/* -- the search ----------------------------------------------------- */

void ce_search_go(ce_pos_t *p, const ce_limits_t *lim, ce_info_t *out);

/* A cheap static count for the UI's "thinking" indicator. */
uint32_t ce_search_nodes(void);

#endif
