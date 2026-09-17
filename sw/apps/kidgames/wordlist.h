#ifndef KG_WORDLIST_H
#define KG_WORDLIST_H

/*
 * kidgames -- shared, level-tiered word lists.
 *
 * Every word-based game draws from here rather than carrying a private
 * copy, so extending the vocabulary once benefits all of them and
 * there is one place to review for age-appropriateness. Spelling,
 * Unscramble, Missing Letter and Word Guess all use it.
 *
 * Tiers are roughly by length -- 3, 4, 5, 6, 7+ letters -- matching
 * the difficulty ladder every game shares: get five right in a row,
 * the level goes up, longer words come in.
 *
 * All words are lowercase a-z. Callers that display them uppercase
 * (which is all of them; the big font has no lowercase) fold on the
 * fly.
 */

#include "kg.h"

#define WORDLIST_MAX_LEVEL 5

typedef struct {
	const char *const *words;
	int count;
} wordlist_t;

/* One entry per level. Conceptually 1-indexed, 0-indexed in the array
 * -- wordlist_pick() and friends handle the offset, and nothing else
 * should index this directly; use wordlist_at(). */
extern const wordlist_t WORDLISTS[WORDLIST_MAX_LEVEL];

/* A random word for `level`, clamped to [1, WORDLIST_MAX_LEVEL]. */
const char *wordlist_pick(int level);

/* Same, but retries a bounded number of times to avoid returning
 * `exclude` -- pass the previously shown word so a game does not
 * immediately repeat itself. NULL means no exclusion. Gives up rather
 * than looping if the list is too small to avoid a repeat. */
const char *wordlist_pick_excluding(int level, const char *exclude);

/*
 * True if `candidate` is any word in the level's list.
 *
 * This is the ambiguity tolerance Missing Letter needs. Blanking a
 * letter in a short word usually leaves more than one real answer --
 * blank the first letter of "cat" and "hat", "bat", "mat", "rat" and
 * "sat" are all correct English and all in the level-1 list. A game
 * that only accepted the word it happened to draw would be telling a
 * kid that "hat" is not a word.
 */
bool wordlist_contains(int level, const char *candidate);

/* Case-insensitive exact compare, exposed because every game needs it
 * to check a typed answer and none of them should be reaching for
 * strcasecmp (not C99) or ctype (a table, for one subtraction). */
bool wordlist_equal(const char *a, const char *b);

/* Enumeration, for the tests and for any game that wants to walk a
 * tier rather than sample it. Returns NULL past the end. */
int wordlist_count(int level);
const char *wordlist_at(int level, int index);

#endif
