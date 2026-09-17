/*
 * kidgames -- the shared word lists. See wordlist.h.
 *
 * Words are the original's, unchanged, including the level tiers and
 * the rhyme families deliberately filled in at level 1. Keeping them
 * identical matters more than it looks: Missing Letter accepts any
 * reconstruction that spells a word IN THIS LIST rather than only the
 * one it drew (wordlist_contains), so the list is not decoration, it
 * is the game's notion of what English is. Dropping a word makes a
 * correct answer wrong.
 *
 * Every word is lowercase a-z with nothing else in it, so big-font
 * display and typed-answer comparison both work with a single
 * uppercase fold and no filtering.
 */

#include <string.h>

#include "wordlist.h"
#include "kgrand.h"

static const char *const WORDS_L1[] = {	/* 3 letters */
	"cat", "dog", "sun", "hat", "pig", "bed", "cup", "run", "red", "big",
	"pen", "box", "fox", "hen", "jar", "owl", "bug", "ant",
	"bat", "mat", "rat", "sat", "den", "ten", "dig", "wig"
	/* The _AT family and friends are here on purpose: blanking a
	 * letter in a three-letter word usually has more than one real
	 * answer, and wordlist_contains() can only accept the alternative
	 * if the word it spells is actually in this list. This can never
	 * be exhaustive without a dictionary -- a kid can always type a
	 * real word nobody curated -- but each addition narrows the gap. */
};

static const char *const WORDS_L2[] = {	/* 4 letters */
	"frog", "fish", "book", "star", "milk", "duck", "jump", "blue",
	"tree", "ball", "lion", "bird", "cake", "rain", "wind", "snow",
	"leaf", "swim"
};

static const char *const WORDS_L3[] = {	/* 5 letters */
	"apple", "house", "happy", "water", "chair", "smile", "plant",
	"clock", "bread", "sheep", "horse", "mouse", "train", "chalk",
	"pizza", "candy", "eagle", "zebra"
};

static const char *const WORDS_L4[] = {	/* 6 letters */
	"school", "friend", "purple", "yellow", "monkey", "flower",
	"pencil", "garden", "summer", "orange", "rocket", "guitar",
	"rabbit", "turtle", "basket", "camera", "dragon", "planet"
};

static const char *const WORDS_L5[] = {	/* 7+ letters */
	"elephant", "birthday", "computer", "sandwich", "umbrella",
	"backpack", "dinosaur", "mountain", "sunshine", "airplane",
	"balloon", "triangle", "vegetable", "butterfly", "alphabet",
	"dolphin", "penguin", "kangaroo"
};

/* Counts derived, not written down. The original hardcoded 26/18/18/
 * 18/18 alongside the arrays, which is two facts that have to agree
 * every time somebody adds a word -- and the failure mode of getting
 * it wrong is reading off the end of the array, i.e. a word that is
 * whatever happened to be in memory. */
#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

const wordlist_t WORDLISTS[WORDLIST_MAX_LEVEL] = {
	{ WORDS_L1, NELEM(WORDS_L1) },
	{ WORDS_L2, NELEM(WORDS_L2) },
	{ WORDS_L3, NELEM(WORDS_L3) },
	{ WORDS_L4, NELEM(WORDS_L4) },
	{ WORDS_L5, NELEM(WORDS_L5) },
};

static const wordlist_t *list_for(int level)
{
	if (level < 1) level = 1;
	if (level > WORDLIST_MAX_LEVEL) level = WORDLIST_MAX_LEVEL;

	return &WORDLISTS[level - 1];
}

const char *wordlist_pick(int level)
{
	const wordlist_t *wl = list_for(level);

	return wl->words[kg_rand((uint32_t)wl->count)];
}

const char *wordlist_pick_excluding(int level, const char *exclude)
{
	const wordlist_t *wl = list_for(level);
	const char *w = 0;
	int tries;

	if (!exclude) return wordlist_pick(level);

	/* Bounded. A list with one word in it, or a run of bad luck, must
	 * not spin forever -- an immediate repeat is a small annoyance and
	 * a hung game is not. */
	for (tries = 0; tries < 8; tries++) {
		w = wl->words[kg_rand((uint32_t)wl->count)];
		if (!wordlist_equal(w, exclude)) return w;
	}

	return w;
}

bool wordlist_equal(const char *a, const char *b)
{
	if (!a || !b) return false;

	/* Case-insensitive, hand-rolled. strcasecmp is not C99 and
	 * tolower() would pull in ctype's table for a job that is one
	 * subtraction -- and every word here is known to be a-z. */
	while (*a && *b) {

		char ca = *a, cb = *b;

		if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');

		if (ca != cb) return false;

		a++;
		b++;

	}

	return *a == *b;
}

bool wordlist_contains(int level, const char *candidate)
{
	const wordlist_t *wl = list_for(level);
	int i;

	if (!candidate || !candidate[0]) return false;

	for (i = 0; i < wl->count; i++)
		if (wordlist_equal(wl->words[i], candidate)) return true;

	return false;
}

int wordlist_count(int level)
{
	return list_for(level)->count;
}

const char *wordlist_at(int level, int index)
{
	const wordlist_t *wl = list_for(level);

	if (index < 0 || index >= wl->count) return 0;

	return wl->words[index];
}
