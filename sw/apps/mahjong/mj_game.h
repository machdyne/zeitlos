#ifndef MJ_GAME_H
#define MJ_GAME_H

/*
 * Zeitlos mahjong -- the game: tiles, the deal, the rules.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * No I/O, no Zeitlos headers, no floating point. tests/engine_test.c
 * links this with mj_layout.c and nothing else.
 *
 * -- the set --
 *
 * 144 tiles: three suits of nine (dots, bamboo, characters), four of
 * each; four winds and three dragons, four of each; and the eight
 * bonus tiles, which are each unique -- four flowers and four seasons.
 * That is 42 faces, numbered as MJ_F_* below.
 *
 * Two tiles MATCH when they have the same face, except that any flower
 * matches any flower and any season any season. So there are 36
 * "classes" of four tiles each, and matching is equality of class.
 *
 * -- every deal can be won --
 *
 * A deal is made BACKWARDS: the layout is first emptied by taking
 * random pairs of free tiles, faces not yet decided, and then each
 * pair taken is given a matching pair of faces. Playing that order
 * forwards clears the board, so a solution exists by construction
 * (mj_game_t.sol keeps it; the tests replay it). A player can still
 * get stuck by choosing different pairs -- that is the game -- and
 * then has undo, and a shuffle that is made the same way: the tiles
 * that are left are redealt onto the places that are left, again so
 * that they can be cleared.
 *
 * The emptying can itself end with one free tile and others under it
 * (two tiles stacked are the simplest case). Then it starts again; a
 * layout where it always does so is reported as unplayable rather
 * than dealt.
 *
 * MJ_DEAL_RANDOM is the other kind: faces scattered with no regard to
 * whether the result can be cleared. Some players prefer it; the
 * status says which kind is on the table.
 *
 * -- numbered deals --
 *
 * A deal is a 32-bit number. The engine draws from its own small
 * generator seeded with it, NOT from zg_rng, so that "deal 4711" is
 * the same tiles on every board and in every test, and "the same deal
 * again" is just the number. The app picks the number from zg_rng.
 */

#include <stdint.h>
#include <stdbool.h>

#include "mj_layout.h"

/* Faces. */
#define MJ_F_DOTS       0       /* 0..8:   one..nine of dots */
#define MJ_F_BAMBOO     9       /* 9..17:  one..nine of bamboo */
#define MJ_F_CHARS      18      /* 18..26: one..nine of characters */
#define MJ_F_WIND       27      /* 27..30: east, south, west, north */
#define MJ_F_DRAGON     31      /* 31..33: red, green, white */
#define MJ_F_FLOWER     34      /* 34..37: plum, orchid, chrysanthemum, bamboo */
#define MJ_F_SEASON     38      /* 38..41: spring, summer, autumn, winter */
#define MJ_FACES        42
#define MJ_CLASSES      36
#define MJ_GONE         0xff

#define MJ_DEAL_SOLVABLE 0
#define MJ_DEAL_RANDOM   1

#define MJ_PLAYING      0
#define MJ_WON          1
#define MJ_STUCK        2       /* tiles left, no pair can be taken */

typedef struct {
	uint16_t a, b;
	uint8_t  fa, fb;
} mj_move_t;

typedef struct {
	const mj_layout_t *L;
	uint32_t  seed;
	int       mode;

	uint8_t   face[MJ_MAX];     /* MJ_GONE once taken */
	int       left;

	/* hist[0..nhist) are the pairs taken, in order; hist[nhist..ntop)
	 * are pairs undone and not yet redone. */
	mj_move_t hist[MJ_MAX / 2];
	int       nhist, ntop;

	/* One way to clear what was dealt (or what the last shuffle left):
	 * pairs of positions, in order. Kept for the tests and for a demo;
	 * a game never needs to look at it. nsol is in positions. */
	uint16_t  sol[MJ_MAX];
	int       nsol;

	int       shuffles, hints, undos;
	int       hint_next;

	/* The deal's own generator, carried on into shuffles, so a deal
	 * shuffled at the same point shuffles the same way. */
	uint32_t  rng;
} mj_game_t;

/* Classes and names. */
int  mj_class(int face);                    /* 0..35 */
bool mj_match(int fa, int fb);
/* "three of dots", "east wind", "red dragon", "plum", "spring". */
void mj_face_name(int face, char *buf, int len);

/* A new deal of layout L, numbered seed. False when the layout cannot
 * be emptied at all (see above); the game is then empty. */
bool mj_new(mj_game_t *g, const mj_layout_t *L, uint32_t seed, int mode);

/* The same deal from the start: mj_new() with the same number. */
void mj_restart(mj_game_t *g);

bool mj_present(const mj_game_t *g, int t);
bool mj_free(const mj_game_t *g, int t);
/* Every tile's freedom at once, a byte each: one pass, where calling
 * mj_free() for each tile marks the table again every time. */
void mj_free_all(const mj_game_t *g, uint8_t *out);

/* Whether a and b may be taken together now, and taking them. */
bool mj_can_take(const mj_game_t *g, int a, int b);
bool mj_take(mj_game_t *g, int a, int b);

bool mj_undo(mj_game_t *g);
bool mj_redo(mj_game_t *g);

/* Every pair that may be taken now, up to max, as a[i], b[i] with
 * a < b. Returns how many there are in all (which may exceed max). */
int  mj_pairs(const mj_game_t *g, uint16_t *a, uint16_t *b, int max);

/* The next pair to suggest: successive calls without a move between
 * them go round every pair. False when there is none. Counts a hint. */
bool mj_hint(mj_game_t *g, int *a, int *b);

int  mj_state(const mj_game_t *g);

/* Redeals the tiles that are left onto the places that are left, so
 * that they can be cleared. Clears undo: the pairs taken before it were
 * taken from a board that no longer exists. False when nothing is left
 * or the places left cannot be emptied (a stack of two); nothing then
 * changes. */
bool mj_shuffle(mj_game_t *g);

/* How many attempts the last mj_new() or mj_shuffle() needed to empty
 * the layout (1 is the usual; more than 64 means it gave up). For the
 * tests, which hold the built-in layouts to rarely needing a second. */
extern int mj_tries;

#endif
