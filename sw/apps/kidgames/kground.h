#ifndef KG_ROUND_H
#define KG_ROUND_H

/*
 * kidgames -- the shared question-and-answer round.
 *
 * Nine of the ten games are the same loop: show a question, take an
 * answer, say whether it was right, add to a score, raise the level
 * after a streak, save. In the original every one of them writes that
 * out longhand -- draw_hud(), show_result() and a scoring block are
 * copy-pasted across eight files with the praise wording and the pause
 * lengths varying slightly and, in a couple of places, accidentally.
 *
 * That was reasonable there and is not here, for a reason specific to
 * this port: the layout is now hand-placed pixels against fixed
 * constants (kg.h) rather than terminal rows. Eight private copies of
 * "where does the face go" is eight places for a layout to drift, and
 * the renders that catch such drift are per-screen.
 *
 * So the pieces every game shares live here, and each game still
 * writes its own loop -- which keeps the original's structure, where
 * you can read one game start to finish in one file, and keeps games
 * that are genuinely different (Memory Match, Letter Hunt) from having
 * to pretend they fit a mould.
 *
 * -- WHAT IS SHARED AND WHAT IS NOT --
 *
 * Shared: the header, the result screen, the scoring and levelling
 * rule, the save. These must be identical everywhere or the app feels
 * inconsistent in ways a kid notices before an adult does.
 *
 * Not shared: the question. Every game draws its own, because that is
 * the game.
 */

#include "kg.h"

typedef struct {
	const char *id;		/* save-file key; see game.h */
	const char *title;	/* header, left */
	int max_level;
	int score;
	int level;
	int streak;
	int best;
} kg_round_t;

/* Five right in a row raises the level. The original's number, in one
 * place rather than eight -- where it was already drifting. */
#define KG_STREAK_TO_LEVEL_UP 5

/* Loads the saved record, clamping the level into [1, max_level]. */
void kg_round_begin(kg_round_t *r, const char *id, const char *title,
	int max_level);

/* Saves. Call on the way out of a game, including on Escape -- the
 * original saves there too, and a kid who quits after levelling up
 * should not lose it. */
void kg_round_end(kg_round_t *r);

/* Header plus a one-line prompt under it. Every game's question screen
 * starts with this. */
void kg_round_header(const kg_round_t *r, const char *prompt);

/*
 * Score the answer, save, show the result screen, and celebrate a new
 * level if one was reached. One call, because the ORDER matters and
 * was subtly different between games in the original: the save has to
 * happen before the result screen (which blocks for seconds), and the
 * level-up celebration has to come after it, or the kid sees "LEVEL
 * UP" before being told they were right.
 *
 * `praise` and `miss` are the two headlines -- "Great job!", "Nice
 * try!". `answer_shown` is drawn in big dithered letters under the
 * miss headline and may be NULL, for a game where showing the answer
 * gives away nothing worth having.
 */
void kg_round_finish(kg_round_t *r, bool correct, const char *praise,
	const char *miss, const char *answer_shown);

/*
 * Just the result screen: face, headline, optional answer, pause.
 * No scoring, no saving, no level-up.
 *
 * For a game that has already done its own scoring, which is every
 * game where a "round" is more than one answer -- Number Guessing pays
 * by how few guesses it took, and Memory Match by how efficiently the
 * grid was cleared. Calling kg_round_finish() from those would score
 * the round a second time, which is not a hypothetical: Memory Match
 * was written that way first and awarded double points and a double
 * streak on every cleared grid.
 */
void kg_round_show(kg_round_t *r, bool correct, const char *headline,
	const char *answer_shown);

/* A happy or sad face, `size` pixels square, top-left at (x,y).
 *
 * Drawn from rectangles rather than blitted from art, and this is not
 * a placeholder for phase 5. A face is two eyes and a mouth; at 64
 * pixels in one bit, that IS the drawing, and anything more detailed
 * turns to noise. It also costs seven fills and no data.
 */
void kg_face(int x, int y, int size, bool happy);

/* Uppercase `src` into `dst` (a-z only, everything else copied). The
 * big font has no lowercase and every word list entry is lowercase, so
 * every word game needs this -- once, here, rather than as an inline
 * loop in each. */
void kg_upper(char *dst, int dstlen, const char *src);

#endif
