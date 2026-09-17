#ifndef KG_ANIMALS_H
#define KG_ANIMALS_H

/*
 * kidgames -- name-and-picture pairs for Name That Animal.
 *
 * Separate from kgart.h (which is generated and holds raw bitmaps) the
 * same way wordlist.c is separate from the games that use it: this is
 * domain data for one game, and a second picture-guessing game could
 * reuse it unchanged.
 *
 * Names are lowercase a-z with nothing else in them, matching
 * wordlist.c's convention, so the same uppercase-fold and compare
 * works without special cases.
 *
 * -- WHY THREE TIERS AND NOT FIVE --
 *
 * Tiers are by name length, like WORDLISTS, but there are three rather
 * than five because the roster is eight animals. Five tiers would
 * leave several holding a single entry, which makes "do not repeat the
 * last one" impossible to honour at those levels -- and a game that
 * shows the same picture twice running has stopped being a quiz.
 *
 * The tiers still matter: without them "giraffe" turns up at level 1
 * next to "cat", which defeats levelling entirely.
 */

#include "kg.h"

#define ANIMALS_MAX_LEVEL 3

typedef struct {
	const char *name;
	const uint8_t *art;	/* KG_ART_W x KG_ART_H, from kgart.h */
} animal_t;

typedef struct {
	const animal_t *animals;
	int count;
} animal_tier_t;

extern const animal_tier_t ANIMAL_TIERS[ANIMALS_MAX_LEVEL];

/* A random animal from `level`'s tier, retrying a bounded number of
 * times to avoid `exclude`. NULL for no exclusion. */
const animal_t *animals_pick_excluding(int level, const animal_t *exclude);

/* Enumeration, for the tests. */
int animals_count(int level);
const animal_t *animals_at(int level, int index);

#endif
