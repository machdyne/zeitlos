#ifndef CE_BOOK_H
#define CE_BOOK_H

/*
 * Zeitlos chess -- a very small opening book.
 *
 * -- what it is for, which is not strength --
 *
 * The book is about twenty five main lines, eight to ten plies each.
 * At that size it is worth almost nothing in Elo: the engine would
 * find most of these moves anyway, and an opponent who leaves the
 * book on move three gets no benefit from it at all.
 *
 * It is here for VARIETY. Without a book the engine is deterministic
 * from move one, so every game against a given level opens exactly
 * the same way, and the second game feels like a replay of the first.
 * That is a much bigger problem for a game somebody plays for fun
 * than fifty Elo in either direction.
 *
 * -- why coordinate notation and not SAN --
 *
 * The book is built at startup by replaying the lines from the
 * starting position, so the lines have to be parsed on the target.
 * ce_parse_move() handles SAN, but it does so by generating the SAN
 * of every legal move and comparing -- and generating one SAN makes
 * and unmakes the move and generates the legal move list AGAIN for
 * disambiguation. That is on the order of a thousand make/unmake
 * pairs per ply, times a couple of hundred plies of book, on a
 * machine that manages a few thousand nodes a second. It would be a
 * visible pause every time the app started.
 *
 * Coordinate notation needs none of that: four characters compared
 * against the coordinate form of each legal move. The lines are less
 * pleasant to read in the source, which is what the comments beside
 * them are for.
 */

#include "ce_core.h"

/* Builds the book. Safe to call more than once; the second call does
 * nothing. Called automatically by ce_book_move() if needed. */
void ce_book_init(void);

/* A book move for this position, or CE_MOVE_NONE. `r` chooses among
 * the alternatives the book holds for the position -- pass a random
 * value, or the same value every time for a deterministic book. */
uint32_t ce_book_move(ce_pos_t *p, uint32_t r);

/* How many entries the book actually built. If this is lower than the
 * lines imply, a line contains an illegal move and was truncated --
 * tests/search_test.c checks for exactly that. */
int ce_book_size(void);

/* Number of book lines, and the nth line's text, for the test. */
int ce_book_line_count(void);
const char *ce_book_line(int i);

#endif
