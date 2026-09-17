#ifndef BJ_HINT_H
#define BJ_HINT_H

/*
 * Zeitlos blackjack -- basic strategy.
 *
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * The correct play for every hand against every up-card, as a lookup.
 *
 * -- why this is a table and not a calculation --
 *
 * Basic strategy IS a table. It is the exact solution to a
 * multi-deck game with no counting, computed once decades ago and
 * published since; recomputing it here would mean a combinatorial
 * search per decision for an answer that never changes.
 *
 * That also makes it testable the way the poker hand counts and
 * roulette's 36-unit return are: these are facts about the game rather
 * than about anybody's code, so tests/hint_test.c asserts the cells
 * everybody knows -- always split aces and eights, never split tens or
 * fives, stand on hard seventeen, double eleven -- and the shape
 * properties that a mistyped table cannot satisfy.
 *
 * -- what it is for --
 *
 * The engine already knows every LEGAL action. This is the one that is
 * CORRECT, which is a different question and the one a person learning
 * the game actually wants answered. Showing it turns the app into a
 * teaching tool: play the hint every time and the house edge is about
 * half a percent; deviate and you can watch what it costs.
 *
 * -- what it does NOT model --
 *
 * The table here is the multi-deck one for a dealer who STANDS on soft
 * seventeen, with doubling after a split allowed. The handful of cells
 * that differ under H17 or no-DAS are not varied, and that is a
 * deliberate limit rather than an oversight: the differences are worth
 * a few hundredths of a percent, and a hint that is subtly wrong in
 * ways the player cannot see is worse than one that is honestly
 * approximate. bj_hint_exact() says whether the current rules are the
 * ones the table was built for.
 */

#include <stdbool.h>
#include "bj_game.h"

/* The advice, in the same vocabulary bj_act() takes. */
#define BJ_HINT_HIT        BJ_HIT
#define BJ_HINT_STAND      BJ_STAND
#define BJ_HINT_DOUBLE     BJ_DOUBLE
#define BJ_HINT_SPLIT      BJ_SPLIT
#define BJ_HINT_SURRENDER  BJ_SURRENDER

/* The correct play for the active hand, already reduced to something
 * bj_act() will accept: a double becomes a hit or a stand when doubling
 * is not available, a surrender becomes a hit, and a split is only ever
 * returned for a genuine pair.
 *
 * Returns -1 when there is nothing to advise -- not the player's turn,
 * or the hand is finished. */
int bj_hint(const bj_game_t *g);

/* The same advice BEFORE that reduction, so a display can say "double
 * if you can, otherwise hit" rather than just "hit". */
int bj_hint_ideal(const bj_game_t *g);

/* False when the table's assumptions do not match the rules in play, so
 * the display can say so rather than quietly advising a slightly wrong
 * game. */
bool bj_hint_exact(const bj_rules_t *r);

const char *bj_hint_name(int hint);

#endif
