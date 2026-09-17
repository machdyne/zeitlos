/*
 * kidgames -- deterministic random numbers, for the host tests only.
 *
 * Replaces kgrand.c. Not merely a stub: the tests that use it are
 * checking things like "a level-3 word is five letters", which is a
 * property of the word list and not of the generator, and which has to
 * fail the SAME WAY every run to be worth anything. A test that draws
 * from real entropy and fails one time in forty is a test people learn
 * to re-run.
 *
 * xorshift32, seeded to a constant. Not good randomness and not
 * pretending to be -- kg_rand_seed() exists so a test can sweep many
 * sequences deliberately rather than hope one run covered the cases.
 */

#include "../kgrand.h"

static uint32_t state = 0x1234567u;

void kg_rand_seed(uint32_t s) { state = s ? s : 1u; }

uint32_t kg_rand(uint32_t n)
{
	if (!n) return 0;

	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;

	return state % n;
}

void kg_rand_init(void) { }
