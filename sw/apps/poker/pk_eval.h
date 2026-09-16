#ifndef PK_EVAL_H
#define PK_EVAL_H

/*
 * Zeitlos poker -- hand ranking.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * Clean-room. The only external facts used anywhere in this file are
 * the rules of poker and the published combinatorial counts in
 * tests/eval_test.c, which are properties of a deck of cards rather
 * than anybody's code.
 *
 * -- one comparable integer, not a struct --
 *
 * pk_eval5() returns a uint32_t with the property that a better hand
 * is a larger number, always, with no comparison function and no
 * special cases:
 *
 *     bits 23..20   category, 0 (high card) .. 8 (straight flush)
 *     bits 19..16   first tiebreak rank
 *     bits 15..12   second
 *     bits 11..8    third
 *     bits  7..4    fourth
 *     bits  3..0    fifth
 *
 * That matters more than it looks. A showdown compares every live
 * hand against every other, a split pot needs exact equality, and the
 * Monte Carlo rollouts in pk_ai.c do both a few hundred thousand
 * times a hand. All three become `<`, `>` and `==` on a machine word.
 *
 * Ties are EXACT. Two hands that tie compare equal as integers, so
 * the pot splitter does not need an epsilon or a "close enough" rule
 * and cannot invent a winner out of a rounding difference.
 *
 * -- what the tiebreak ranks are --
 *
 * The hand's distinct ranks, ordered by how many cards share them and
 * then by rank, both descending. That single rule produces the right
 * answer for every category without any per-category code:
 *
 *     quads       quad rank, kicker
 *     full house  trips rank, pair rank
 *     two pair    high pair, low pair, kicker
 *     one pair    pair rank, three kickers
 *     flush       five ranks, high to low
 *     high card   five ranks, high to low
 *
 * Straights and straight flushes are the exception and are stored as
 * their high card alone, because a straight is fully described by it
 * and the remaining nibbles would be redundant.
 *
 * Unused nibbles are zero. A category always has the same shape, so
 * two hands in the same category always have their real ranks in the
 * same nibbles and the padding never decides anything.
 *
 * -- the wheel --
 *
 * A-2-3-4-5 is a five-high straight, so the ace plays low. It is the
 * one place in poker where an ace is not the highest card, it is
 * worth 40 of the 2,598,960 five-card hands at straight-flush level
 * alone, and an evaluator that misses it is otherwise indistinguish-
 * able from a correct one. pk_eval5() reports its high card as
 * PK_RANK_5, which makes it the lowest straight by the ordinary
 * comparison rather than by a special case anywhere else.
 *
 * There is no corresponding "round the corner" straight: Q-K-A-2-3 is
 * not a hand.
 */

#include <stdint.h>
#include "pk_cards.h"

#define PK_HIGH_CARD      0
#define PK_PAIR           1
#define PK_TWO_PAIR       2
#define PK_TRIPS          3
#define PK_STRAIGHT       4
#define PK_FLUSH          5
#define PK_FULL_HOUSE     6
#define PK_QUADS          7
#define PK_STRAIGHT_FLUSH 8

#define PK_NCATEGORIES    9

/* Worse than every real hand, including the worst possible high card
 * (7-5-4-3-2 offsuit). For a folded seat, or a seat that has not been
 * dealt in, so that "who has the best hand" needs no live-seat test
 * of its own. */
#define PK_EVAL_NONE 0u

#define PK_CATEGORY(v) ((int)((v) >> 20))

/* Exactly five cards. No duplicate check -- pk_deck.c cannot produce
 * one, and the tests that build hands by hand check their own input.
 * A duplicate here does not crash, it just returns a value for a hand
 * that cannot exist. */
uint32_t pk_eval5(const uint8_t *cards);

/* The best five-card hand available from `n` cards, 5 to 7.
 *
 * Every C(n,5) subset, evaluated. 21 of them at seven cards, which is
 * the only case that matters for speed and is around a microsecond on
 * this hardware. A faster method exists and is not worth the risk: the
 * published fast evaluators all come with lookup tables, which would
 * compromise the clean-room requirement for a saving that disappears
 * next to the rollouts that call this.
 *
 * `best` may be NULL. When it is not, the winning five cards are
 * written to it, which is what the showdown display needs to say WHY
 * a hand won rather than just that it did.
 */
uint32_t pk_eval_best(const uint8_t *cards, int n, uint8_t *best);

/* Omaha and its relatives: exactly `use_hole` of the hole cards and
 * exactly 5 - use_hole of the board, rather than the best five of all
 * of them.
 *
 * Separate from pk_eval_best() because it is a genuinely different
 * question and merging them produces the classic Omaha bug -- a board
 * showing four hearts and a player holding one heart believing they
 * have a flush. They do not, and the difference is invisible unless
 * the constraint is enforced here.
 *
 * Returns PK_EVAL_NONE if the counts cannot be satisfied.
 */
uint32_t pk_eval_constrained(const uint8_t *hole, int nhole,
    const uint8_t *board, int nboard, int use_hole, uint8_t *best);

/* Ranks 1 to 5 EXPOSED cards, for the stud act order.
 *
 * From fourth street onward the high hand showing acts first, and
 * "high hand showing" is decided on up-cards alone: at most four of
 * them in seven-card stud, at most four in five-card stud. Straights
 * and flushes are therefore unreachable and are NOT considered, which
 * is also what the rules say -- a board showing four to a flush does
 * not act before a pair.
 *
 * Returns the same comparable encoding pk_eval5() does, so the caller
 * compares with `>` and nothing else. The categories used are only
 * PK_HIGH_CARD through PK_QUADS.
 */
uint32_t pk_eval_upcards(const uint8_t *cards, int n);

/* "aces full of kings", "king-high flush", "five-high straight".
 * Writes at most `len` bytes including the NUL. For the message line
 * and the showdown panel. */
void pk_eval_name(uint32_t value, char *buf, int len);

#endif
