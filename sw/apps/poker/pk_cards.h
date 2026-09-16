#ifndef PK_CARDS_H
#define PK_CARDS_H

/*
 * Zeitlos poker -- the card encoding, shared by every other layer.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A card is one byte, 0..51, and the two halves are recovered by
 * shifting rather than by dividing:
 *
 *     rank = card >> 2      0 = deuce ... 12 = ace
 *     suit = card & 3       0 = clubs, 1 = diamonds, 2 = hearts,
 *                           3 = spades
 *
 * -- why rank is the HIGH half and suit the low half --
 *
 * Because it makes `card / 4` into a shift and, more usefully, makes
 * the natural ordering of the byte the same as the ordering of the
 * rank. Sorting a hand by card value sorts it by rank, which the
 * evaluator and the display both want and neither then has to do.
 *
 * The suit order is clubs, diamonds, hearts, spades -- alphabetical,
 * and the order used everywhere the rules do care.
 *
 * NO HAND-RANKING RULE BREAKS A TIE BY SUIT. Two flushes of the same
 * ranks split the pot, always. Anything in pk_eval.c that appears to
 * rank one suit above another is a bug, and tests/eval_test.c asserts
 * the negative directly.
 *
 * THE STUD BRING-IN IS THE EXCEPTION, and it is the reason this order
 * is alphabetical rather than arbitrary. On third street the forced
 * bet falls on the lowest exposed card, and a tie there IS broken by
 * suit with clubs lowest -- the only place in the game where a suit
 * ranks. pk_game.c's bring-in selection can therefore compare card
 * bytes directly. An earlier draft of this comment claimed no rule
 * anywhere used suits, which was wrong.
 *
 * -- the deuce is rank 0, not rank 2 --
 *
 * So that a rank fits in a nibble with room for the ace, which is
 * what lets pk_eval() pack a category and five tiebreak ranks into
 * 24 bits. The cost is that PK_RANK() does not return the number
 * printed on the card, and every place that needs that number goes
 * through pk_rank_char(). Getting this wrong produces an evaluator
 * that is correct except that aces are low, which is exactly the kind
 * of thing that survives casual testing.
 */

#include <stdint.h>
#include <stdbool.h>

#define PK_NRANKS   13
#define PK_NSUITS   4
#define PK_NCARDS   52

#define PK_RANK(c)  ((int)((c) >> 2))
#define PK_SUIT(c)  ((int)((c) & 3))
#define PK_CARD(r, s) ((uint8_t)(((r) << 2) | (s)))

/* Not a card. 0xff rather than 0 because 0 is the two of clubs, and a
 * zeroed structure full of deuces is far harder to notice than one
 * full of obvious nonsense. */
#define PK_CARD_NONE 0xffu

#define PK_RANK_2   0
#define PK_RANK_5   3
#define PK_RANK_T   8
#define PK_RANK_J   9
#define PK_RANK_Q   10
#define PK_RANK_K   11
#define PK_RANK_A   12

#define PK_CLUBS    0
#define PK_DIAMONDS 1
#define PK_HEARTS   2
#define PK_SPADES   3

/* 'A', 'K', 'Q', 'J', 'T', '9' ... '2' for rank 12 down to 0. */
char pk_rank_char(int rank);

/* 'c', 'd', 'h', 's'. */
char pk_suit_char(int suit);

/* Writes three bytes: rank, suit, NUL. `buf` must hold at least 3.
 * A card of PK_CARD_NONE writes "--". Returns buf. */
char *pk_card_str(uint8_t card, char *buf);

/* Parses "Ah", "td", "2C" -- rank first, suit second, either case.
 * Returns PK_CARD_NONE on anything else. Used by the tests and by
 * the `deal` debug command; not on any hot path. */
uint8_t pk_card_parse(const char *s);

#endif
