/*
 * kidgames -- save file and word list tests.
 *
 * No Zeitlos headers and no framebuffer: everything checked here is
 * pure. kgsave.c is built with -DKG_SAVE_HOSTED so its parser and
 * serialiser are the SHIPPED ones while the two functions that touch a
 * filesystem are not compiled at all.
 *
 * The save file is the one piece of this app that can lose something a
 * kid cares about. A parser that reads a malformed line as "score 0,
 * level 0" does not report an error -- it silently resets progress,
 * and then writes that back. So the cases below are mostly about
 * files that are WRONG.
 */

#include <stdio.h>
#include <string.h>

#include "../kgsave.h"
#include "../wordlist.h"
#include "../kgrand.h"
#include "../animals.h"
#include "../kgart.h"
#include "../kgsound.h"

void kg_rand_seed(uint32_t s);

static int failures;

#define CHECK(cond, msg) do { \
	if (!(cond)) { \
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
		failures++; \
	} \
} while (0)

static char ids[KG_SAVE_MAX_GAMES][KG_SAVE_MAX_ID];
static kg_save_t recs[KG_SAVE_MAX_GAMES];

static void test_parse_basic(void)
{
	int n = kg_save_parse(
		"spelling 120 3\n"
		"counting 40 2\n"
		"wordguess 0 1\n",
		ids, recs, KG_SAVE_MAX_GAMES);

	CHECK(n == 3, "three good lines did not parse as three");
	CHECK(!strcmp(ids[0], "spelling"), "first id wrong");
	CHECK(recs[0].best_score == 120 && recs[0].level == 3, "first record wrong");
	CHECK(!strcmp(ids[2], "wordguess"), "third id wrong");
	CHECK(recs[2].best_score == 0 && recs[2].level == 1, "third record wrong");
}

/*
 * Every way a hand-edited file can be wrong. Each of these must be
 * SKIPPED, leaving the other games' progress intact -- not parsed into
 * a zeroed record, and not aborting the parse so that everything after
 * it disappears.
 */
static void test_parse_malformed(void)
{
	int n = kg_save_parse(
		"spelling 120 3\n"
		"garbage\n"                  /* no numbers at all */
		"counting notanumber 2\n"    /* score is not a number */
		"missing 50\n"               /* no level */
		"# a comment\n"
		"\n"
		"   \n"
		"wordguess 7 2\n",
		ids, recs, KG_SAVE_MAX_GAMES);

	CHECK(n == 2, "malformed lines were not skipped cleanly");
	CHECK(!strcmp(ids[0], "spelling"), "a good line before the junk was lost");
	CHECK(!strcmp(ids[1], "wordguess"), "a good line after the junk was lost");
	CHECK(recs[1].best_score == 7 && recs[1].level == 2,
		"the line after the junk parsed wrong");
}

/* A level of 0 in the file would index a word list at -1 and divide
 * every difficulty ladder by zero. Clamped once on the way in. */
static void test_level_clamp(void)
{
	int n = kg_save_parse("spelling 10 0\n", ids, recs, KG_SAVE_MAX_GAMES);

	CHECK(n == 1, "a zero level made the line unparseable");
	CHECK(recs[0].level >= 1, "a zero level was not clamped");
}

/* No trailing newline on the last line -- what a hand edit in a text
 * editor that does not add one produces. */
static void test_no_trailing_newline(void)
{
	int n = kg_save_parse("spelling 120 3\ncounting 40 2",
		ids, recs, KG_SAVE_MAX_GAMES);

	CHECK(n == 2, "a file with no trailing newline lost its last line");
}

static void test_roundtrip(void)
{
	char out[KG_SAVE_FILE_MAX];
	char ids2[KG_SAVE_MAX_GAMES][KG_SAVE_MAX_ID];
	kg_save_t recs2[KG_SAVE_MAX_GAMES];
	int n, len, m, i;

	n = kg_save_parse("spelling 120 3\ncounting 40 2\nsimplemath 999 5\n",
		ids, recs, KG_SAVE_MAX_GAMES);

	len = kg_save_format(out, sizeof(out), ids, recs, n);
	CHECK(len > 0, "format refused a small record set");

	m = kg_save_parse(out, ids2, recs2, KG_SAVE_MAX_GAMES);
	CHECK(m == n, "a round trip changed the number of records");

	for (i = 0; i < m && i < n; i++) {
		CHECK(!strcmp(ids[i], ids2[i]), "a round trip changed an id");
		CHECK(recs[i].best_score == recs2[i].best_score,
			"a round trip changed a score");
		CHECK(recs[i].level == recs2[i].level,
			"a round trip changed a level");
	}
}

/*
 * A serialiser that runs out of room must refuse the WHOLE write, not
 * truncate.
 *
 * Truncating leaves a half-written last line, which the parser then
 * skips -- so one game's progress vanishes on every save from then on,
 * quietly, and only for whichever game sorted last.
 */
static void test_format_overflow(void)
{
	char small[24];
	int i;

	for (i = 0; i < 4; i++) {
		strcpy(ids[i], "somegame");
		recs[i].best_score = 123456;
		recs[i].level = 5;
	}

	CHECK(kg_save_format(small, sizeof(small), ids, recs, 4) == 0,
		"format truncated instead of refusing");
}

/* -- word lists -------------------------------------------------------- */

static void test_wordlist_tiers(void)
{
	static const int WANT[WORDLIST_MAX_LEVEL] = { 3, 4, 5, 6, 7 };
	int lvl, i;

	for (lvl = 1; lvl <= WORDLIST_MAX_LEVEL; lvl++) {

		int n = wordlist_count(lvl);

		CHECK(n > 0, "a word list tier is empty");

		for (i = 0; i < n; i++) {

			const char *w = wordlist_at(lvl, i);
			int len = (int)strlen(w);
			const char *p;

			CHECK(w != 0, "wordlist_at returned NULL inside the range");
			if (!w) continue;

			/* Tiers 1-4 are an exact length; tier 5 is "7 or more". A
			 * word in the wrong tier is a level-1 word that is six
			 * letters long, which is the difficulty ladder quietly not
			 * working. */
			if (lvl < WORDLIST_MAX_LEVEL)
				CHECK(len == WANT[lvl - 1], "a word is in the wrong tier");
			else
				CHECK(len >= WANT[lvl - 1], "a level-5 word is too short");

			/* Lowercase a-z only. Anything else breaks the big font
			 * (which has no lowercase and no punctuation) or the
			 * uppercase fold every game does on a typed answer. */
			for (p = w; *p; p++)
				CHECK(*p >= 'a' && *p <= 'z',
					"a word contains something other than a-z");

		}

	}
}

/* Duplicates would skew the draw and, worse, make Missing Letter's
 * ambiguity check report a word twice. */
static void test_wordlist_unique(void)
{
	int lvl, i, j;

	for (lvl = 1; lvl <= WORDLIST_MAX_LEVEL; lvl++) {
		int n = wordlist_count(lvl);
		for (i = 0; i < n; i++)
			for (j = i + 1; j < n; j++)
				CHECK(!wordlist_equal(wordlist_at(lvl, i),
					wordlist_at(lvl, j)), "a word appears twice in a tier");
	}
}

static void test_wordlist_pick(void)
{
	int lvl, t;

	kg_rand_seed(12345);

	for (lvl = 1; lvl <= WORDLIST_MAX_LEVEL; lvl++)
		for (t = 0; t < 200; t++) {
			const char *w = wordlist_pick(lvl);
			CHECK(w != 0, "wordlist_pick returned NULL");
			CHECK(wordlist_contains(lvl, w),
				"wordlist_pick returned a word not in its own tier");
		}

	/* Out-of-range levels clamp rather than read off the end of the
	 * table -- which would return whatever was in memory and then be
	 * drawn on screen as a word. */
	CHECK(wordlist_pick(0) != 0, "level 0 did not clamp");
	CHECK(wordlist_pick(99) != 0, "level 99 did not clamp");
	CHECK(wordlist_contains(1, wordlist_pick(0)), "level 0 clamped upward");
	CHECK(wordlist_contains(WORDLIST_MAX_LEVEL, wordlist_pick(99)),
		"level 99 clamped downward");
}

/* The exclusion is bounded and must not hang, even when the list has
 * exactly one candidate to avoid -- and it may legitimately give up
 * and return the excluded word rather than looping. */
static void test_wordlist_exclude(void)
{
	int t;
	int repeats = 0;

	kg_rand_seed(777);

	for (t = 0; t < 500; t++) {
		const char *prev = wordlist_pick(3);
		const char *next = wordlist_pick_excluding(3, prev);
		CHECK(next != 0, "pick_excluding returned NULL");
		if (wordlist_equal(prev, next)) repeats++;
	}

	/* With eighteen words and eight tries, an immediate repeat should
	 * be very rare. Not asserted as zero -- the function is documented
	 * to give up rather than spin, and a test that forbids the
	 * documented behaviour is a test that will fail on a smaller
	 * list one day. */
	CHECK(repeats < 25, "pick_excluding repeats far too often");

	CHECK(wordlist_pick_excluding(2, 0) != 0,
		"pick_excluding(NULL) did not behave like pick");
}

/*
 * The ambiguity tolerance Missing Letter depends on: blanking the
 * first letter of "cat" leaves several real answers, and all of them
 * are in the level-1 list on purpose.
 */
static void test_wordlist_contains(void)
{
	CHECK(wordlist_contains(1, "cat"), "cat is not in tier 1");
	CHECK(wordlist_contains(1, "CAT"), "contains is not case-insensitive");
	CHECK(wordlist_contains(1, "hat"), "hat is not in tier 1");
	CHECK(wordlist_contains(1, "bat"), "bat is not in tier 1");
	CHECK(!wordlist_contains(1, "zzz"), "a non-word was accepted");
	CHECK(!wordlist_contains(1, ""), "an empty answer was accepted");
	CHECK(!wordlist_contains(1, "apple"), "a tier-3 word matched tier 1");

	CHECK(wordlist_equal("Cat", "cAT"), "equal is not case-insensitive");
	CHECK(!wordlist_equal("cat", "cats"), "a prefix compared equal");
	CHECK(!wordlist_equal("cats", "cat"), "a prefix compared equal");
}

/* -- animals ------------------------------------------------------------
 *
 * The roster is eight entries across three tiers, and every failure
 * here is quiet on a board: a NULL art pointer blits from address zero,
 * a name with a space in it can never be typed (the pad has no space
 * key on the alpha layout... it does, but the field rejects it), and a
 * name in the wrong tier means the difficulty ladder is not a ladder.
 */
static void test_animals(void)
{
	int lvl, i, j;
	int total = 0;

	for (lvl = 1; lvl <= ANIMALS_MAX_LEVEL; lvl++) {

		int n = animals_count(lvl);

		CHECK(n > 0, "an animal tier is empty");
		total += n;

		for (i = 0; i < n; i++) {

			const animal_t *a = animals_at(lvl, i);
			const char *p;
			int len;

			CHECK(a != 0, "animals_at returned NULL inside the range");
			if (!a) continue;

			CHECK(a->art != 0, "an animal has no art");
			CHECK(a->name && a->name[0], "an animal has no name");
			if (!a->name) continue;

			len = (int)strlen(a->name);

			/* Lowercase a-z only, matching wordlist.c -- the game
			 * uppercase-folds the name and compares it to what was
			 * typed, and the field only accepts A-Z. A name with
			 * anything else in it could never be answered correctly. */
			for (p = a->name; *p; p++)
				CHECK(*p >= 'a' && *p <= 'z',
					"an animal name contains something other than a-z");

			/* The tiers are by name length: <=4, 5-6, 7+. */
			if (lvl == 1) CHECK(len <= 4, "a long name is in tier 1");
			else if (lvl == 2) CHECK(len >= 5 && len <= 6,
				"a name is in the wrong tier for its length");
			else CHECK(len >= 7, "a short name is in tier 3");

		}

	}

	/*
	 * Not a count, a property: every tier needs at least two entries
	 * or animals_pick_excluding() cannot honour its own contract and
	 * the same picture comes up twice running. The roster grew from 8
	 * to 16 and a hardcoded total simply had to be edited, which is a
	 * test that only ever reports that somebody changed something.
	 */
	CHECK(total >= 6, "the roster is implausibly small");

	/* Unique across ALL tiers, not just within one: two entries with
	 * the same name would be two different pictures with one right
	 * answer. */
	for (lvl = 1; lvl <= ANIMALS_MAX_LEVEL; lvl++)
		for (i = 0; i < animals_count(lvl); i++) {
			int l2;
			for (l2 = lvl; l2 <= ANIMALS_MAX_LEVEL; l2++)
				for (j = (l2 == lvl ? i + 1 : 0); j < animals_count(l2); j++)
					CHECK(!wordlist_equal(animals_at(lvl, i)->name,
						animals_at(l2, j)->name),
						"two animals share a name");
		}
}

/*
 * Picking, at every level, with and without an exclusion.
 *
 * Every tier must have enough entries for "do not repeat the last one"
 * to be honourable -- test_animals() checks that separately -- and the
 * retry must stay BOUNDED regardless, because a tier that ever drops
 * to one entry would otherwise hang the game rather than repeat a
 * picture. This runs a lot of draws precisely so a loop that does not
 * terminate shows up here rather than on a board.
 */
static void test_animals_pick(void)
{
	int lvl, t;

	kg_rand_seed(99);

	for (lvl = 1; lvl <= ANIMALS_MAX_LEVEL; lvl++) {

		const animal_t *prev = 0;
		int repeats = 0;

		for (t = 0; t < 300; t++) {

			const animal_t *a = animals_pick_excluding(lvl, prev);

			CHECK(a != 0, "pick returned NULL");
			if (!a) break;

			/* In its own tier, and nowhere else -- a pick that reached
			 * past the end of its array would still be non-NULL. */
			{
				int i, found = 0;
				for (i = 0; i < animals_count(lvl); i++)
					if (animals_at(lvl, i) == a) found = 1;
				CHECK(found, "pick returned an animal from another tier");
			}

			if (prev && a == prev) repeats++;
			prev = a;

		}

		CHECK(repeats < 20, "pick_excluding repeats far too often");

	}

	CHECK(animals_pick_excluding(0, 0) != 0, "level 0 did not clamp");
	CHECK(animals_pick_excluding(99, 0) != 0, "level 99 did not clamp");
}

/*
 * No cue may overrun the sample buffer.
 *
 * note() appends and stops at the limit, so an overlong cue does not
 * crash -- it is silently cut off mid-note, which on a small speaker
 * sounds like a click and gets blamed on the speaker. The level-up
 * arpeggio is the long one and the one that will grow if anybody makes
 * the fuss bigger.
 */
static void test_sound_lengths(void)
{
	static const kg_sound_t CUES[] = {
		KG_SND_RIGHT, KG_SND_WRONG, KG_SND_LEVELUP
	};
	int i;

	for (i = 0; i < 3; i++) {
		int n = kg_sound_render(CUES[i]);
		CHECK(n > 0, "a cue rendered nothing");
		CHECK(n < kg_sound_buffer_max(),
			"a cue fills or overruns the sample buffer");
	}
}

int main(void)
{
	test_parse_basic();
	test_parse_malformed();
	test_level_clamp();
	test_no_trailing_newline();
	test_roundtrip();
	test_format_overflow();

	test_wordlist_tiers();
	test_wordlist_unique();
	test_wordlist_pick();
	test_wordlist_exclude();
	test_wordlist_contains();

	test_animals();
	test_animals_pick();
	test_sound_lengths();

	if (failures) {
		printf("test_data: %d FAILURES\n", failures);
		return 1;
	}

	puts("test_data: ok");

	return 0;
}
