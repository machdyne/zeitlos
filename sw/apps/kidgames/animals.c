/*
 * kidgames -- the animal roster. See animals.h.
 *
 * The eight animals are the original's, and so is the art direction
 * note that goes with them, which transfers to pixels better than it
 * did to ASCII:
 *
 *   Each animal leans on ONE unmistakable trait, and where possible
 *   that trait is an overall SILHOUETTE PROPORTION rather than fine
 *   facial detail -- a giraffe's absurdly long neck-to-body ratio, a
 *   snake's long undulating line. Delicate detail is easy to get
 *   subtly wrong and hard for a viewer to parse at this scale; an
 *   earlier attempt there used small faces differing only by ear
 *   shape and did not work.
 *
 * At 64x48 in one bit that advice is not a preference, it is the only
 * thing that can work: there are not enough pixels for an eye.
 */

#include "animals.h"
#include "kgart.h"
#include "kgrand.h"
#include "wordlist.h"	/* wordlist_equal, the shared case-insensitive compare */

static const animal_t ANIMALS_L1[] = {	/* <= 4 letters */
	{ "cat",  kg_art_cat },
	{ "crab", kg_art_crab },
	{ "dog",  kg_art_dog },
	{ "fish", kg_art_fish },
	{ "owl",  kg_art_owl },
	{ "pig",  kg_art_pig },
};

static const animal_t ANIMALS_L2[] = {	/* 5-6 letters */
	{ "horse",  kg_art_horse },
	{ "monkey", kg_art_monkey },
	{ "rabbit", kg_art_rabbit },
	{ "snake",  kg_art_snake },
	{ "spider", kg_art_spider },
	{ "turtle", kg_art_turtle },
};

static const animal_t ANIMALS_L3[] = {	/* 7+ letters */
	{ "crocodile", kg_art_crocodile },
	{ "elephant",  kg_art_elephant },
	{ "giraffe",   kg_art_giraffe },
	{ "kangaroo",  kg_art_kangaroo },
};

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

const animal_tier_t ANIMAL_TIERS[ANIMALS_MAX_LEVEL] = {
	{ ANIMALS_L1, NELEM(ANIMALS_L1) },
	{ ANIMALS_L2, NELEM(ANIMALS_L2) },
	{ ANIMALS_L3, NELEM(ANIMALS_L3) },
};

static const animal_tier_t *tier_for(int level)
{
	if (level < 1) level = 1;
	if (level > ANIMALS_MAX_LEVEL) level = ANIMALS_MAX_LEVEL;

	return &ANIMAL_TIERS[level - 1];
}

const animal_t *animals_pick_excluding(int level, const animal_t *exclude)
{
	const animal_tier_t *t = tier_for(level);
	const animal_t *a = 0;
	int tries;

	/*
	 * Bounded rather than looping until it succeeds.
	 *
	 * With six, six and four entries a repeat is merely unlucky, and
	 * eight tries makes one vanishingly rare. The bound is not for
	 * today's roster though: a tier with a single animal in it has no
	 * other to pick, and an unbounded retry would hang the game --
	 * which is exactly what the top tier was before the roster grew,
	 * and could be again if somebody adds a fourth tier.
	 */
	for (tries = 0; tries < 8; tries++) {
		a = &t->animals[kg_rand((uint32_t)t->count)];
		if (!exclude || a != exclude) return a;
	}

	return a;
}

int animals_count(int level)
{
	return tier_for(level)->count;
}

const animal_t *animals_at(int level, int index)
{
	const animal_tier_t *t = tier_for(level);

	if (index < 0 || index >= t->count) return 0;

	return &t->animals[index];
}
